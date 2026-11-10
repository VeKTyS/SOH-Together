// Story together: boss room co-entry, cutscenes, music.
// Copied from TrueCoop (a fork of SoH's Anchor): Packets/BossEntry.cpp, Packets/CutsceneSync.cpp and
// Packets/BgmSync.cpp. Same rules, same numbers. Only the network part changed: their json packets are
// our EventKind::BossEntry / CutsceneSync / BgmSync events, first byte of the data = sub-type.

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdarg>
#include <mutex>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
// SoH's audio editor (soh/Enhancements/audio/AudioEditor.cpp), the same call Audio_StartSequence makes
u16 AudioEditor_GetReplacementSeq(u16 seqId);
}

// libultra's r4300.h has a U32(x) macro that eats ByteWriter::U32 / ByteReader::U32 calls
#undef U32

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------------------------

// Anchor's IsSaveLoaded(): in game, on a real save file, not the title screen demo.
bool SaveLoaded() {
    if (gPlayState == nullptr || GET_PLAYER(gPlayState) == nullptr) {
        return false;
    }
    if (gSaveContext.fileNum < 0 || gSaveContext.fileNum > 2) {
        return false;
    }
    return gSaveContext.gameMode == GAMEMODE_NORMAL;
}

// Same thing without the "Link exists" part. OnSceneInit and the entrance cutscene check both run
// inside Play_Init BEFORE Link is spawned (z_play.c:476/493 vs Actor_InitContext at :566), so the full
// check above would always say no there and nothing would ever be sent.
bool SaveLoadedForSceneInit() {
    if (gPlayState == nullptr) {
        return false;
    }
    if (gSaveContext.fileNum < 0 || gSaveContext.fileNum > 2) {
        return false;
    }
    return gSaveContext.gameMode == GAMEMODE_NORMAL;
}

// Anchor's "targetTeamId": their server only passes these packets to the same team. We get everything,
// so we drop it here if the sender is on another team.
bool SameTeam(uint32_t origin) {
    const Coop::RemotePlayer* p = CoopGame::Session().FindById(origin);
    if (p == nullptr || !p->state) {
        return true; // don't know their team yet, same as the default team
    }
    return p->state->team == CoopGame::LocalState().team;
}

// TrueCoop: myLifeState >= LIFE_STATE_DOWNED (downed / spectating / game over)
bool DownedOrGameOver() {
    return CoopGame::LocalLifeState() >= CoopGame::LifeState::Downed;
}

bool ValidScene(int32_t sceneNum) {
    return sceneNum >= 0 && sceneNum < SCENE_ID_MAX;
}

bool ValidEntrance(int32_t entrance) {
    return entrance >= 0 && entrance < ENTR_MAX;
}

// ---------------------------------------------------------------------------------------------------------------
// Boss co-entry, copied from TrueCoop Packets/BossEntry.cpp
// When someone walks through a boss door, the others are pulled into the same fight.
// ---------------------------------------------------------------------------------------------------------------

constexpr uint8_t kBossEntry = 0; // {u16 sceneNum, u32 entranceIndex}

struct BossEntryInfo {
    s16 bossScene;
    s16 dungeonScene; // just for reading, the code only uses bossScene / bossEntrance
    s32 bossEntrance;
};

// The 9 boss fights (same table as TrueCoop, Ganon's final fight left out like them)
constexpr BossEntryInfo sBossTable[] = {
    { SCENE_DEKU_TREE_BOSS, SCENE_DEKU_TREE, ENTR_DEKU_TREE_BOSS_ENTRANCE },                   // 0x40F Gohma
    { SCENE_DODONGOS_CAVERN_BOSS, SCENE_DODONGOS_CAVERN, ENTR_DODONGOS_CAVERN_BOSS_ENTRANCE }, // 0x40B King Dodongo
    { SCENE_JABU_JABU_BOSS, SCENE_JABU_JABU, ENTR_JABU_JABU_BOSS_ENTRANCE },                   // 0x301 Barinade
    { SCENE_FOREST_TEMPLE_BOSS, SCENE_FOREST_TEMPLE, ENTR_FOREST_TEMPLE_BOSS_ENTRANCE },       // 0x00C Phantom Ganon
    { SCENE_FIRE_TEMPLE_BOSS, SCENE_FIRE_TEMPLE, ENTR_FIRE_TEMPLE_BOSS_ENTRANCE },             // 0x305 Volvagia
    { SCENE_WATER_TEMPLE_BOSS, SCENE_WATER_TEMPLE, ENTR_WATER_TEMPLE_BOSS_ENTRANCE },          // 0x417 Morpha
    { SCENE_SPIRIT_TEMPLE_BOSS, SCENE_SPIRIT_TEMPLE, ENTR_SPIRIT_TEMPLE_BOSS_ENTRANCE },       // 0x08D Twinrova
    { SCENE_SHADOW_TEMPLE_BOSS, SCENE_SHADOW_TEMPLE, ENTR_SHADOW_TEMPLE_BOSS_ENTRANCE },       // 0x413 Bongo Bongo
    { SCENE_GANONDORF_BOSS, SCENE_GANONS_TOWER, ENTR_GANONDORF_BOSS_0 },                       // 0x41F Ganondorf
};

const BossEntryInfo* FindBossByScene(s16 sceneNum) {
    for (const BossEntryInfo& e : sBossTable) {
        if (e.bossScene == sceneNum) {
            return &e;
        }
    }
    return nullptr;
}

// Echo guard: set right before WE get pulled into a boss room, so our own OnSceneInit there doesn't
// send BossEntry again (that would pull the first player back, forever).
bool sWarpedInByBossEntry = false;

// The warp can get replaced by another one or expire before we ever load the boss room, then the
// guard would stay on forever and eat our next real boss entry. So we keep track of the pull and
// let it go after 40 s (TrueCoop: longer than CoopWarp's 30 s expiry) or once we're there.
struct PendingBossEntry {
    bool active = false;
    s16 bossSceneNum = 0;
    Clock::time_point deadline;
};
PendingBossEntry sPendingBossEntry;
constexpr auto kReconcileTimeout = std::chrono::seconds(40);

void DropPendingBossEntry() {
    sWarpedInByBossEntry = false;
    sPendingBossEntry.active = false;
}

void ArmPendingBossEntry(s16 sceneNum) {
    DropPendingBossEntry(); // latest pull wins, like CoopWarp
    sWarpedInByBossEntry = true;
    sPendingBossEntry.active = true;
    sPendingBossEntry.bossSceneNum = sceneNum;
    sPendingBossEntry.deadline = Clock::now() + kReconcileTimeout;
}

// every frame (TrueCoop BossEntryTick)
void ReconcileBossEntry() {
    if (!sPendingBossEntry.active || gPlayState == nullptr) {
        return;
    }
    // our OnSceneInit used the guard: we landed in the boss room, done
    if (!sWarpedInByBossEntry) {
        sPendingBossEntry.active = false;
        return;
    }
    // we're in the boss room but the guard is still on (shouldn't happen), let it go
    if (gPlayState->sceneNum == sPendingBossEntry.bossSceneNum) {
        DropPendingBossEntry();
        return;
    }
    // never got there: the warp expired or got replaced
    if (Clock::now() >= sPendingBossEntry.deadline) {
        DropPendingBossEntry();
        SPDLOG_INFO("[Coop][BossEntry] reconcile: pull warp did not land, disarm echo latch");
    }
}

void SendBossEntry(s16 sceneNum, s32 entranceIndex) {
    Coop::ByteWriter w;
    w.U8(kBossEntry);
    w.U16(static_cast<uint16_t>(sceneNum));
    w.U32(static_cast<uint32_t>(entranceIndex));
    CoopGame::SendEvent(Coop::EventKind::BossEntry, w.Take());
    SPDLOG_INFO("[Coop][BossEntry] tx scene={} entrance={}", sceneNum, entranceIndex);
}

// TrueCoop HandlePacket_BossEntry
void OnBossEntryEvent(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint8_t type = 0;
    uint16_t scene = 0;
    uint32_t entrance = 0;
    if (!r.U8(type) || type != kBossEntry || !r.U16(scene) || !r.U32(entrance) || r.Remaining() != 0) {
        return;
    }
    if (!SaveLoaded() || !SameTeam(e.origin)) {
        return;
    }
    s16 sceneNum = static_cast<s16>(scene);
    s32 entranceIndex = static_cast<s32>(entrance);

    // the warp target comes from OUR table, never from the packet
    const BossEntryInfo* boss = FindBossByScene(sceneNum);
    if (boss == nullptr) {
        SPDLOG_INFO("[Coop][BossEntry] rx ignored (not a boss scene {})", sceneNum);
        return;
    }
    if (entranceIndex != boss->bossEntrance) {
        SPDLOG_WARN("[Coop][BossEntry] rx entrance mismatch: pkt={} table={}", entranceIndex, boss->bossEntrance);
    }

    // already there (both went in at the same time): nobody warps
    if (gPlayState->sceneNum == sceneNum) {
        SPDLOG_INFO("[Coop][BossEntry] rx ignored (already in scene {})", sceneNum);
        return;
    }
    // downed / game over: don't drag a dead player into a boss fight
    if (DownedOrGameOver()) {
        SPDLOG_INFO("[Coop][BossEntry] rx ignored (downed/game-over)");
        return;
    }

    // otherwise always pull. Guard first, then the warp (-1: the boss intro plays by itself on load)
    ArmPendingBossEntry(sceneNum);
    CoopGame::RequestEntranceWarp(static_cast<int16_t>(boss->bossEntrance), -1);
    SPDLOG_INFO("[Coop][BossEntry] rx -> pull entrance={}", boss->bossEntrance);
}

// TrueCoop RegisterBossEntryHooks, OnSceneInit part
void BossEntryOnSceneInit(int16_t sceneNum) {
    const BossEntryInfo* boss = FindBossByScene(sceneNum);
    if (boss == nullptr) {
        return; // not a boss room
    }
    // we got here because we were pulled in: use up the guard and stay quiet. Only for the boss room
    // we were pulled to: walking into another one ourselves still sends.
    if (sWarpedInByBossEntry && sceneNum == sPendingBossEntry.bossSceneNum) {
        sWarpedInByBossEntry = false;
        SPDLOG_INFO("[Coop][BossEntry] tx suppressed (self-warped) scene={}", sceneNum);
        return;
    }
    SendBossEntry(sceneNum, boss->bossEntrance);
}

// ---------------------------------------------------------------------------------------------------------------
// Cutscene sync, copied from TrueCoop Packets/CutsceneSync.cpp
// When someone starts a story cutscene, the others are pulled to the same spot and play it too.
//   kind 0: entrance cutscene (Deku Tree intro, Hyrule Field intro...), seen by VB_PLAY_ENTRANCE_CS
//   kind 1: scene layer cutscene (blue warps, Deku Tree death speech...), cutsceneIndex >= 0xFFF0 at OnSceneInit
// data: {u8 kind, u16 sceneNum, u32 entranceIndex, u32 cutsceneIndex, u16 csFlag}
// ---------------------------------------------------------------------------------------------------------------

// the game's own test for "this load is a scene layer cutscene" (z_play.c / z_demo.c)
constexpr s32 kSceneLayerCsMin = 0xFFF0;

// One echo guard per kind, keyed on the entrance we expect, so an old one can't eat an unrelated cutscene.
struct ReplayLatch {
    bool armed = false;
    s32 entrance = -1;
};
ReplayLatch sEntranceCsReplay;   // kind 0, used up by the VB_PLAY_ENTRANCE_CS hook
ReplayLatch sSceneLayerCsReplay; // kind 1, used up by OnSceneInit

bool ConsumeLatch(ReplayLatch& latch, s32 entrance) {
    if (latch.armed && latch.entrance == entrance) {
        latch.armed = false;
        return true;
    }
    return false;
}

// For kind 0 we clear the cutscene's eventChkInf flag so it plays again here. If it doesn't play
// (wrong age, the warp expired or got replaced...) the flag would stay cleared and the guard on,
// so we keep track of it and fix both (TrueCoop PendingReplay).
struct PendingReplay {
    bool active = false;
    u8 kind = 0;
    s32 csFlag = -1; // kind 0: the flag we cleared, -1 for kind 1
    s32 entrance = -1;
    s16 sceneNum = 0;
    Clock::time_point deadline;
};
PendingReplay sPendingReplay;

ReplayLatch& LatchForKind(u8 kind) {
    return (kind == 0) ? sEntranceCsReplay : sSceneLayerCsReplay;
}

// Raw SET_EVENTCHKINF on purpose (no Flags_SetEventChkInf, so no flag event sent back)
void ResetCsFlag(const char* why) {
    if (sPendingReplay.kind == 0 && sPendingReplay.csFlag >= 0 && !GET_EVENTCHKINF(sPendingReplay.csFlag)) {
        SET_EVENTCHKINF(sPendingReplay.csFlag);
        if (why != nullptr) {
            SPDLOG_INFO("[Coop][CutsceneSync] reconcile: {}, re-SET guard flag {}", why, sPendingReplay.csFlag);
        }
    }
}

void ResolvePendingReplay() {
    if (!sPendingReplay.active) {
        return;
    }
    ReplayLatch& latch = LatchForKind(sPendingReplay.kind);
    if (latch.armed) {
        ResetCsFlag(nullptr);
        latch.armed = false;
    }
    sPendingReplay.active = false;
}

// must be called BEFORE arming this replay's latch (the flush looks at the current latch)
void ArmPendingReplay(u8 kind, s32 csFlag, s32 entrance, s16 sceneNum) {
    ResolvePendingReplay();
    sPendingReplay.active = true;
    sPendingReplay.kind = kind;
    sPendingReplay.csFlag = csFlag;
    sPendingReplay.entrance = entrance;
    sPendingReplay.sceneNum = sceneNum;
    sPendingReplay.deadline = Clock::now() + kReconcileTimeout;
}

// every frame (TrueCoop CutsceneSyncTick)
void ReconcilePendingReplay() {
    if (!sPendingReplay.active || gPlayState == nullptr) {
        return;
    }
    ReplayLatch& latch = LatchForKind(sPendingReplay.kind);

    // the cutscene played here and used up the guard (and set the flag again). Done.
    if (!latch.armed) {
        sPendingReplay.active = false;
        return;
    }

    // We're at the right scene + entrance. The entrance cutscene check runs once in Play_Init, before
    // the first frame, so if the guard is still on the cutscene did NOT play (age mismatch...).
    if (gPlayState->sceneNum == sPendingReplay.sceneNum && gSaveContext.entranceIndex == sPendingReplay.entrance) {
        ResetCsFlag("CS did not fire on load");
        latch.armed = false;
        sPendingReplay.active = false;
        return;
    }

    // never got there: the warp expired or got replaced. Put the flag back and drop the guard.
    if (Clock::now() >= sPendingReplay.deadline) {
        ResetCsFlag("pull warp did not land");
        latch.armed = false;
        sPendingReplay.active = false;
    }
}

void SendCutsceneSync(u8 kind, s16 sceneNum, s32 entranceIndex, s32 cutsceneIndex, s16 csFlag) {
    Coop::ByteWriter w;
    w.U8(kind);
    w.U16(static_cast<uint16_t>(sceneNum));
    w.U32(static_cast<uint32_t>(entranceIndex));
    w.U32(static_cast<uint32_t>(cutsceneIndex));
    w.U16(static_cast<uint16_t>(csFlag));
    CoopGame::SendEvent(Coop::EventKind::CutsceneSync, w.Take());
    SPDLOG_INFO("[Coop][CutsceneSync] tx kind={} scene={} cs={}", kind, sceneNum, cutsceneIndex);
}

// TrueCoop HandlePacket_CutsceneSync
void OnCutsceneSyncEvent(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint8_t kind = 0;
    uint16_t scene = 0, flag = 0;
    uint32_t entrance = 0, cs = 0;
    if (!r.U8(kind) || !r.U16(scene) || !r.U32(entrance) || !r.U32(cs) || !r.U16(flag) || r.Remaining() != 0) {
        return;
    }
    if (!SaveLoaded() || !SameTeam(e.origin)) {
        return;
    }
    s16 sceneNum = static_cast<s16>(scene);
    s32 entranceIndex = static_cast<s32>(entrance);
    s32 cutsceneIndex = static_cast<s32>(cs);
    s16 csFlag = static_cast<s16>(flag);

    // packets can be garbage: never warp to a made-up entrance or poke a flag outside the array
    if (!ValidScene(sceneNum) || !ValidEntrance(entranceIndex)) {
        return;
    }
    if (kind == 0 && csFlag >= 0 && (csFlag >> 4) >= static_cast<s16>(ARRAY_COUNT(gSaveContext.eventChkInf))) {
        return;
    }
    if (kind == 1 && (cutsceneIndex < kSceneLayerCsMin || cutsceneIndex > 0xFFFF)) {
        return;
    }

    // already in that scene (both triggered it at the same time): nobody warps
    if (gPlayState->sceneNum == sceneNum) {
        SPDLOG_INFO("[Coop][CutsceneSync] rx ignored (already in scene {})", sceneNum);
        return;
    }
    // downed / game over: don't drag a dead player into a cutscene
    if (DownedOrGameOver()) {
        SPDLOG_INFO("[Coop][CutsceneSync] rx ignored (downed/game-over)");
        return;
    }

    if (kind == 0) {
        // track it first (flushes the previous one before we touch the guard)
        ArmPendingReplay(0, csFlag, entranceIndex, sceneNum);
        // Clear the cutscene's flag directly (CLEAR_EVENTCHKINF, no Flags_UnsetEventChkInf so no flag
        // event goes out). It was set on their side right before the cutscene and came to us with the
        // flag sync; clearing it lets the same cutscene trigger here on load. Playing it sets it again.
        if (csFlag >= 0) {
            CLEAR_EVENTCHKINF(csFlag);
        }
        // guard before the warp, so our own VB_PLAY_ENTRANCE_CS on load doesn't send it back
        sEntranceCsReplay = { true, entranceIndex };
        CoopGame::RequestEntranceWarp(static_cast<int16_t>(entranceIndex), -1); // -1: plays naturally
        SPDLOG_INFO("[Coop][CutsceneSync] rx -> replay entrance={}", entranceIndex);
    } else if (kind == 1) {
        ArmPendingReplay(1, -1, entranceIndex, sceneNum);
        sSceneLayerCsReplay = { true, entranceIndex };
        // with the cutsceneIndex, like z_demo.c does for these
        CoopGame::RequestEntranceWarp(static_cast<int16_t>(entranceIndex), cutsceneIndex);
        SPDLOG_INFO("[Coop][CutsceneSync] rx -> replay entrance={}", entranceIndex);
    } else {
        SPDLOG_INFO("[Coop][CutsceneSync] rx ignored (unknown kind {})", kind);
    }
}

// kind 0 sender, from VB_PLAY_ENTRANCE_CS (z_demo.c:2206, args: flag, entrance). Doesn't block the
// cutscene here (*should untouched).
void CutsceneOnEntranceCs(s32 flag, s32 entrance) {
    // we're playing it because we were pulled in: use up the guard, don't send it back
    if (ConsumeLatch(sEntranceCsReplay, entrance)) {
        SPDLOG_INFO("[Coop][CutsceneSync] tx suppressed (self-replay) kind=0 entrance={}", entrance);
        return;
    }
    SendCutsceneSync(0, gPlayState->sceneNum, entrance, gSaveContext.cutsceneIndex, static_cast<s16>(flag));
}

// kind 1 sender. cutsceneIndex is already worked out when OnSceneInit fires. We send the FINAL
// entranceIndex so the others land at the same place even when z_demo.c rewrote the entrance.
void CutsceneOnSceneInit(int16_t sceneNum) {
    s32 cutsceneIndex = gSaveContext.cutsceneIndex;
    if (cutsceneIndex < kSceneLayerCsMin) {
        return; // normal load
    }
    s32 entrance = gSaveContext.entranceIndex;
    if (ConsumeLatch(sSceneLayerCsReplay, entrance)) {
        SPDLOG_INFO("[Coop][CutsceneSync] tx suppressed (self-replay) kind=1 entrance={}", entrance);
        return;
    }
    SendCutsceneSync(1, sceneNum, entrance, cutsceneIndex, -1);
}

// ---------------------------------------------------------------------------------------------------------------
// Music sync, copied from TrueCoop Packets/BgmSync.cpp
// Everyone hears the music of the area THEY are in. When two players are in the same area, the one
// who just arrived jumps its main music to where the one already there is, so no echo. The one
// already there is never restarted (except in restart mode / very long tracks).
// ---------------------------------------------------------------------------------------------------------------

constexpr uint8_t kBgmPosRequest = 0; // {u16 sceneNum}                                    arriver -> same scene
constexpr uint8_t kBgmPos = 1;        // {u32 target, u16 seqId, u32 scriptCounter, u16 sceneNum}  reply
constexpr uint8_t kBgmRestart = 2;    // {u16 seqId, u16 sceneNum}                          both restart from 0
constexpr uint8_t kBgmState = 3;      // {u16 seqId, u32 scriptCounter, u16 sceneNum}       ~5 s heartbeat

constexpr uint32_t BGM_HEARTBEAT_FRAMES = 300;
constexpr int32_t BGM_REQUEST_WINDOW = 120; // frames after a scene load where we keep asking
constexpr int32_t BGM_REQUEST_THROTTLE = 8; // frames between two asks
constexpr int32_t BGM_DRIFT_THRESHOLD_DEFAULT = 48;
constexpr uint32_t BGM_SKIP_TICK_CAP = 216000; // past this, jumping ahead takes too long: restart both instead

bool sSeekDone = false;             // jumped (or restarted) already for this scene visit
bool sRestartBroadcastDone = false; // restart mode, once per scene visit
int32_t sReqFramesLeft = 0;
int32_t sReqThrottle = 0;
uint32_t sHeartbeatFrames = 0;

// TrueCoop added these two to the engine (code_800F9280.c). 9.3.1 doesn't have them, so they're
// rebuilt here from what 9.3.1 has.

// Main music: which track (gActiveSeqs seqId, NA_BGM_DISABLED if none) and how far in it is.
// scriptCounter goes +1 each time the sequence player steps (audio_seqplayer.c), the same unit
// skipTicks counts in, so a position from one game is a jump target for another.
u32 Audio_GetBgmPlaybackInfo(u16* seqId) {
    *seqId = func_800FA0B4(SEQ_PLAYER_BGM_MAIN);
    if (*seqId == NA_BGM_DISABLED) {
        return 0;
    }
    return gAudioContext.seqPlayers[SEQ_PLAYER_BGM_MAIN].scriptCounter;
}

// Restart a track and fast-forward it by skipTicks with the notes muted. Same as the game's own
// Audio_StartSequence with seqArg 0x7F (code_800F9280.c:39), which queues audio command 0x85 ->
// AudioLoad_SyncInitSeqPlayerSkipTicks (code_800E4FE0.c:254, audio_load.c:567), but with an exact
// tick count instead of a duration in seconds.
void Audio_StartSeqSkipTicks(u8 playerIdx, u8 seqId, u32 skipTicks) {
    if (D_80133408 != 0 && playerIdx != SEQ_PLAYER_SFX) {
        return; // new sequences are switched off right now, Audio_StartSequence does the same
    }
    ActiveSequence& active = gActiveSeqs[playerIdx];
    bool sameTrack = active.seqId != NA_BGM_DISABLED && (active.seqId & 0xFF) == seqId;

    // Restarting what's playing: reuse the exact track loaded in the player (it's already the audio
    // editor's replacement). Otherwise ask the audio editor like Audio_StartSequence does.
    u16 resolvedSeqId;
    if (sameTrack && gAudioContext.seqPlayers[playerIdx].enabled) {
        resolvedSeqId = gAudioContext.seqPlayers[playerIdx].seqId;
    } else {
        resolvedSeqId = AudioEditor_GetReplacementSeq(seqId);
    }
    Audio_QueueCmdS32(0x85000000 | _SHIFTL(playerIdx, 16, 8) | (resolvedSeqId & 0xFFFF), static_cast<s32>(skipTicks));

    // Keep the seqArg byte the game had (Audio_StartSequence would store 0x7F there, and other game
    // code compares the whole value with plain NA_BGM_ ids).
    u16 seqArgs = sameTrack ? (active.seqId & 0xFF00) : 0;
    active.seqId = seqId | seqArgs;
    active.prevSeqId = seqId | seqArgs;

    // the rest is Audio_StartSequence as is
    if (active.volCur != 1.0f) {
        Audio_QueueCmdF32(0x41000000 | _SHIFTL(playerIdx, 16, 8), active.volCur);
    }
    active.tempoTimer = 0;
    active.tempoOriginal = 0;
    active.tempoCmd = 0;
    for (u8 i = 0; i < 0x10; i++) {
        active.channelData[i].volCur = 1.0f;
        active.channelData[i].volTimer = 0;
        active.channelData[i].freqScaleCur = 1.0f;
        active.channelData[i].freqScaleTimer = 0;
    }
    active.freqScaleChannelFlags = 0;
    active.volChannelFlags = 0;
}

bool BgmSyncEnabled() {
    return CVarGetInteger(CVAR_COOP("BgmSync"), 1) != 0;
}

// 0 = arriver jumps (default), 1 = restart both from 0 (plan B, also restarts the one already there)
bool RestartMode() {
    return CVarGetInteger(CVAR_COOP("BgmSyncMode"), 0) == 1;
}

// Ocarina / song on screen: don't touch the music, it would fight it.
bool OcarinaActive() {
    if (gPlayState == nullptr) {
        return false;
    }
    u16 m = gPlayState->msgCtx.msgMode;
    return m >= MSGMODE_OCARINA_STARTING && m <= MSGMODE_OCARINA_AWAIT_INPUT;
}

// extra ticks added to a jump target (the position we got is a bit old by the time it arrives)
u32 SeekCompensation() {
    int32_t comp = CVarGetInteger(CVAR_COOP("BgmSyncCompensation"), 0);
    return comp > 0 ? static_cast<u32>(comp) : 0;
}

void ResetBgmLocal() {
    sSeekDone = false;
    sRestartBroadcastDone = false;
    sReqFramesLeft = 0;
    sReqThrottle = 0;
    sHeartbeatFrames = 0;
}

// TrueCoop sends to each player in our scene; we send once to everyone (the scene is in the data
// and the others check it), and only if somebody is in our scene, same as their loop.
bool TeammateInMyScene() {
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (p.state && p.state->inGame && p.state->sceneNum == gPlayState->sceneNum) {
            return true;
        }
    }
    return false;
}

void SendBgmPosRequest() {
    if (!TeammateInMyScene()) {
        return;
    }
    Coop::ByteWriter w;
    w.U8(kBgmPosRequest);
    w.U16(static_cast<uint16_t>(gPlayState->sceneNum));
    CoopGame::SendEvent(Coop::EventKind::BgmSync, w.Take());
}

void SendBgmPos(uint32_t target, u16 seqId, u32 scriptCounter, s16 sceneNum) {
    Coop::ByteWriter w;
    w.U8(kBgmPos);
    w.U32(target);
    w.U16(seqId);
    w.U32(scriptCounter);
    w.U16(static_cast<uint16_t>(sceneNum));
    CoopGame::SendEvent(Coop::EventKind::BgmSync, w.Take());
}

void SendBgmRestart(u16 seqId) {
    if (!TeammateInMyScene()) {
        return;
    }
    Coop::ByteWriter w;
    w.U8(kBgmRestart);
    w.U16(seqId);
    w.U16(static_cast<uint16_t>(gPlayState->sceneNum));
    CoopGame::SendEvent(Coop::EventKind::BgmSync, w.Take());
}

void SendBgmState() {
    u16 seqId;
    u32 pos = Audio_GetBgmPlaybackInfo(&seqId);
    if (seqId == NA_BGM_DISABLED || !TeammateInMyScene()) {
        return; // nothing to compare
    }
    Coop::ByteWriter w;
    w.U8(kBgmState);
    w.U16(seqId);
    w.U32(pos);
    w.U16(static_cast<uint16_t>(gPlayState->sceneNum));
    CoopGame::SendEvent(Coop::EventKind::BgmSync, w.Take());
}

// Already there: answer with our position. Our own music isn't touched.
void HandleBgmPosRequest(uint32_t requester, s16 payloadScene) {
    if (payloadScene != gPlayState->sceneNum) {
        return; // only people in the same scene answer
    }
    u16 seqId;
    u32 pos = Audio_GetBgmPlaybackInfo(&seqId);
    if (seqId == NA_BGM_DISABLED) {
        return;
    }
    SendBgmPos(requester, seqId, pos, gPlayState->sceneNum);
}

// Just arrived: jump our main music to their position (once per scene visit).
void HandleBgmPos(u16 payloadSeqId, u32 payloadPos, s16 payloadScene) {
    if (sSeekDone || OcarinaActive()) {
        return;
    }
    if (payloadScene != gPlayState->sceneNum) {
        return;
    }
    u16 ownSeqId;
    (void)Audio_GetBgmPlaybackInfo(&ownSeqId);
    if (ownSeqId == NA_BGM_DISABLED) {
        return; // our music isn't up yet, the window keeps asking
    }
    if ((payloadSeqId & 0xFF) != (ownSeqId & 0xFF)) {
        // other track (day/night, boss theme, still starting): do nothing, keep the window open
        SPDLOG_INFO("[Coop][Bgm] pos ignored: track mismatch peer={} own={}", payloadSeqId & 0xFF, ownSeqId & 0xFF);
        return;
    }
    // very long time in: jumping that far would take too long, restart both from 0 instead
    if (payloadPos > BGM_SKIP_TICK_CAP) {
        SPDLOG_INFO("[Coop][Bgm] peer pos {} over skip cap -> restart fallback", payloadPos);
        SendBgmRestart(ownSeqId & 0xFF);
        Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, ownSeqId & 0xFF, 0);
        sSeekDone = true;
        return;
    }
    u32 target = payloadPos + SeekCompensation();
    Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, ownSeqId & 0xFF, target);
    sSeekDone = true;
    SPDLOG_INFO("[Coop][Bgm] seek to tick={} (peer pos={})", target, payloadPos);
}

// Restart our main music from 0 if it's the same track.
void HandleBgmRestart(u16 payloadSeqId, s16 payloadScene) {
    if (OcarinaActive() || payloadScene != gPlayState->sceneNum) {
        return;
    }
    u16 ownSeqId;
    (void)Audio_GetBgmPlaybackInfo(&ownSeqId);
    if (ownSeqId == NA_BGM_DISABLED || (payloadSeqId & 0xFF) != (ownSeqId & 0xFF)) {
        return;
    }
    Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, ownSeqId & 0xFF, 0);
    SPDLOG_INFO("[Coop][Bgm] restart to seqId={}", ownSeqId & 0xFF);
}

// Heartbeat: if we drifted too far apart, only the HIGHER player id jumps (so two games never keep
// jumping at each other).
void HandleBgmState(uint32_t senderId, u16 payloadSeqId, u32 payloadPos, s16 payloadScene) {
    if (payloadScene != gPlayState->sceneNum) {
        return;
    }
    u16 ownSeqId;
    u32 ownPos = Audio_GetBgmPlaybackInfo(&ownSeqId);
    if (ownSeqId == NA_BGM_DISABLED || (payloadSeqId & 0xFF) != (ownSeqId & 0xFF)) {
        return;
    }
    s32 drift = static_cast<s32>(payloadPos - ownPos);
    SPDLOG_INFO("[Coop][Bgm] drift={} ticks (peer={} own={})", drift, payloadPos, ownPos);

    int32_t threshold = CVarGetInteger(CVAR_COOP("BgmDriftThreshold"), BGM_DRIFT_THRESHOLD_DEFAULT);
    bool autoReseek = CVarGetInteger(CVAR_COOP("BgmAutoReseek"), 1) != 0;
    s32 absDrift = drift < 0 ? -drift : drift;

    if (autoReseek && !OcarinaActive() && absDrift > threshold && CoopGame::SelfId() > senderId) {
        u32 target = payloadPos + SeekCompensation();
        if (target <= BGM_SKIP_TICK_CAP) {
            Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, ownSeqId & 0xFF, target);
            SPDLOG_INFO("[Coop][Bgm] reseek (higher-id) to tick={}", target);
        }
    }
}

void OnBgmSyncEvent(const Coop::Event& e) {
    if (!BgmSyncEnabled() || !SaveLoaded()) {
        return;
    }
    Coop::ByteReader r(e.data);
    uint8_t type = 0;
    if (!r.U8(type)) {
        return;
    }
    uint16_t scene = 0, seqId = 0;
    uint32_t pos = 0, target = 0;
    switch (type) {
        case kBgmPosRequest:
            if (r.U16(scene) && r.Remaining() == 0) {
                HandleBgmPosRequest(e.origin, static_cast<s16>(scene));
            }
            break;
        case kBgmPos:
            if (r.U32(target) && r.U16(seqId) && r.U32(pos) && r.U16(scene) && r.Remaining() == 0 &&
                target == CoopGame::SelfId()) {
                HandleBgmPos(seqId, pos, static_cast<s16>(scene));
            }
            break;
        case kBgmRestart:
            if (r.U16(seqId) && r.U16(scene) && r.Remaining() == 0) {
                HandleBgmRestart(seqId, static_cast<s16>(scene));
            }
            break;
        case kBgmState:
            if (r.U16(seqId) && r.U32(pos) && r.U16(scene) && r.Remaining() == 0) {
                HandleBgmState(e.origin, seqId, pos, static_cast<s16>(scene));
            }
            break;
        default:
            break;
    }
}

// TrueCoop BgmSyncTick
void BgmTick() {
    if (!SaveLoaded()) {
        return;
    }
    bool enabled = BgmSyncEnabled();
    // (the spectating music from the same TrueCoop file lives in CoopLife.cpp UpdateMusic)

    if (!enabled) {
        return;
    }

    // Debug: set Coop.BgmSeekTest to N and our main music jumps N ticks ahead, once (TrueCoop's test
    // to check the jump works before trusting it over the network).
    int32_t seekTest = CVarGetInteger(CVAR_COOP("BgmSeekTest"), 0);
    if (seekTest != 0) {
        CVarClear(CVAR_COOP("BgmSeekTest"));
        u16 seqId;
        u32 pos = Audio_GetBgmPlaybackInfo(&seqId);
        if (seqId != NA_BGM_DISABLED) {
            u32 target = (seekTest > 0) ? pos + static_cast<u32>(seekTest) : pos;
            Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, seqId & 0xFF, target);
            SPDLOG_INFO("[Coop][Bgm] debug local reseek seqId={} from tick={} to tick={} (+{})", seqId & 0xFF, pos,
                        target, seekTest);
        } else {
            SPDLOG_INFO("[Coop][Bgm] debug local reseek skipped: no active MAIN bgm");
        }
    }

    bool ocarina = OcarinaActive();

    // just arrived: keep asking for a while (the music starts a few frames into the scene)
    if (!sSeekDone && sReqFramesLeft > 0 && !ocarina) {
        u16 seqId;
        (void)Audio_GetBgmPlaybackInfo(&seqId);
        if (seqId != NA_BGM_DISABLED) {
            if (RestartMode()) {
                // restart mode: restart ours from 0 and tell the others in the scene to do the same
                if (!sRestartBroadcastDone) {
                    SendBgmRestart(seqId & 0xFF);
                    Audio_StartSeqSkipTicks(SEQ_PLAYER_BGM_MAIN, seqId & 0xFF, 0);
                    sRestartBroadcastDone = true;
                    sSeekDone = true;
                    SPDLOG_INFO("[Coop][Bgm] restart-mode rendezvous seqId={}", seqId & 0xFF);
                }
            } else if (sReqThrottle == 0) {
                SendBgmPosRequest();
                sReqThrottle = BGM_REQUEST_THROTTLE;
            } else {
                sReqThrottle--;
            }
        }
        sReqFramesLeft--;
    }

    // drift heartbeat
    if (++sHeartbeatFrames >= BGM_HEARTBEAT_FRAMES) {
        sHeartbeatFrames = 0;
        SendBgmState();
    }
}

// TrueCoop OnTransitionEnd: a scene load, maybe we just joined someone. Only the one who moved asks.
void BgmOnTransitionEnd(int16_t) {
    sSeekDone = false;
    sRestartBroadcastDone = false;
    sReqFramesLeft = BGM_REQUEST_WINDOW;
    sReqThrottle = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------------------------------------------

// TrueCoop drops all this on disconnect. Our hooks stay registered, so we drop it the first time a
// hook fires while there's no session.
void ResetAll() {
    DropPendingBossEntry();
    sEntranceCsReplay = {};
    sSceneLayerCsReplay = {};
    sPendingReplay = {};
    ResetBgmLocal();
}

void Register() {
    // Both senders run at scene load. Boss first, then cutscene (TrueCoop registers them in that order).
    COND_HOOK(OnSceneInit, true, [](int16_t sceneNum) {
        std::lock_guard lock(CoopGame::Mutex());
        if (!CoopGame::SessionActive()) {
            ResetAll();
            return;
        }
        if (!SaveLoadedForSceneInit()) {
            return;
        }
        BossEntryOnSceneInit(sceneNum);
        CutsceneOnSceneInit(sceneNum);
    });

    COND_VB_SHOULD(VB_PLAY_ENTRANCE_CS, true, {
        // args as in z_demo.c (flag, entrance), read as s32 like SoH's timesaver handler
        s32 flag = va_arg(args, s32);
        s32 entrance = va_arg(args, s32);
        std::lock_guard lock(CoopGame::Mutex());
        if (CoopGame::SessionActive() && SaveLoadedForSceneInit()) {
            CutsceneOnEntranceCs(flag, entrance);
        }
    });

    COND_HOOK(OnTransitionEnd, true, [](int16_t sceneNum) {
        std::lock_guard lock(CoopGame::Mutex());
        if (!CoopGame::SessionActive()) {
            ResetAll();
            return;
        }
        BgmOnTransitionEnd(sceneNum);
    });

    // same order as TrueCoop's per-frame dispatcher: boss, cutscene, music
    CoopGame::OnEveryFrame("Story", []() {
        ReconcileBossEntry();
        ReconcilePendingReplay();
        BgmTick();
    });

    CoopGame::OnEvent(Coop::EventKind::BossEntry, "Story", OnBossEntryEvent);
    CoopGame::OnEvent(Coop::EventKind::CutsceneSync, "Story", OnCutsceneSyncEvent);
    CoopGame::OnEvent(Coop::EventKind::BgmSync, "Story", OnBgmSyncEvent);
}

} // namespace

static RegisterShipInitFunc sInit(Register);
