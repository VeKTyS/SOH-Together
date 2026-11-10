// Boss sync: boss adapters, copied from TrueCoop BossAdapters/ (ActorSyncAdapter.h,
// AdapterRegistry.cpp, GohmaAdapter.cpp, EnGomaAdapter.cpp).
//
// A plain enemy is copied as is by CoopEnemies.cpp (host runs it, clients copy
// position, skeleton and the rest of its memory). Bosses are trickier: their
// intro and death are cutscenes that must run on every game. An adapter tells
// CoopEnemies.cpp, per boss, in which "phase" the boss is, when to copy it and
// what to do when the phase changes. Bosses without an adapter are not copied:
// everyone runs their own and only the health is shared.
//
// TrueCoop sends the adapter's extra fields as JSON. We already copy the whole
// enemy memory every frame, so the extras are just "these fields, always copy
// them" (ApplyExtras) and "these fields, never copy them" (no-copy ranges).
// The phase travels in the top 5 bits of EnemyState::submitted.
//
// Not ported: TrueCoop's EnDekubabaAdapter. It needs helper functions TrueCoop
// added to z_en_dekubaba.c (EnDekubaba_MirrorGetStemMode...), which 9.3.1 doesn't
// have. Our memory copy already carries what it streams (actionFunc, stem
// angles, timer), and deaths go through the normal kill messages.
//
// At the bottom: "Hold boss fights until everyone's there", from Crests of
// Courage boss.cpp (update_wait / hold_fight / release_fight). TrueCoop's
// BossEntry (CoopStory.cpp) pulls everybody into the boss room, but the first
// one in starts the fight alone while the others are still loading. This holds
// the boss until the whole party is in the room.

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Boss_Goma/z_boss_goma.h"
#include "src/overlays/actors/ovl_En_Goma/z_en_goma.h"
extern PlayState* gPlayState;
// not static in 9.3.1 (z_boss_goma.c:399)
void BossGoma_SetupDefeated(BossGoma* thisx, PlayState* play);
}

namespace CoopGame {
// Crests of Courage "Hold boss fights until everyone's there" host setting (CoopRoom.cpp)
bool RoomBossWait();
} // namespace CoopGame

namespace {

// a field inside the actor: offset from the actor start + size
struct Field {
    size_t offset;
    size_t size;
};
#define FIELD(Type, member) Field{ offsetof(Type, member), sizeof(((Type*)nullptr)->member) }

// TrueCoop ActorSyncAdapter. Every part is optional.
struct BossAdapter {
    // host: small phase number from the actor's fields (never from actionFunc)
    uint8_t (*GetPhase)(Actor* actor) = nullptr;
    // client: the host's phase changed (last host pose already applied). True =
    // stop copying it for good, our own game runs it from here (defeat cutscene)
    bool (*OnPhaseChange)(Actor* actor, uint8_t fromPhase, uint8_t toPhase) = nullptr;
    // client: false = run it ourselves this frame (intro cutscene...)
    bool (*ShouldMirror)(Actor* actor, uint8_t streamedPhase) = nullptr;
    // TrueCoop's SerializeExtras / DeserializeExtras fields: always copied from the host
    std::vector<Field> extras;
    // never copied from the host (things the local cutscene owns)
    std::vector<Field> noCopy;
};

// --- Gohma (TrueCoop GohmaAdapter.cpp) ---
//
// The intro cutscene IS Gohma's update, so every game runs its own intro
// (while disableGameplayLogic is set, Gohma has no colliders and no gameplay).
// Copying only starts in FIGHT, once our own intro is over too. On FIGHT ->
// DEFEATED we call BossGoma_SetupDefeated ourselves (just un-freezing it would
// leave a stale action) and our own game plays the death: cutscene, heart
// container, blue warp, clear flag, all local (flags are synced anyway).

enum GohmaPhase : uint8_t {
    GOHMA_PHASE_PREFIGHT = 0,
    GOHMA_PHASE_FIGHT = 1,
    GOHMA_PHASE_DEFEATED = 2,
};

uint8_t Gohma_GetPhase(Actor* actor) {
    BossGoma* goma = (BossGoma*)actor;
    if (goma->disableGameplayLogic) {
        return ((int8_t)actor->colChkInfo.health <= 0) ? GOHMA_PHASE_DEFEATED : GOHMA_PHASE_PREFIGHT;
    }
    return GOHMA_PHASE_FIGHT;
}

bool Gohma_OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    SPDLOG_INFO("[Coop] Gohma phase {} -> {}", fromPhase, toPhase);
    if (toPhase == GOHMA_PHASE_DEFEATED && fromPhase == GOHMA_PHASE_FIGHT && gPlayState != nullptr) {
        // last host pose is already applied: play the defeat here from it
        actor->colChkInfo.health = 0;
        BossGoma_SetupDefeated((BossGoma*)actor, gPlayState);
        SPDLOG_INFO("[Coop] Gohma beaten on the host, playing the defeat here");
        return true;
    }
    return false;
}

bool Gohma_ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    BossGoma* g = (BossGoma*)actor;
    // only during the fight proper, and only once our own intro is done
    return streamedPhase == GOHMA_PHASE_FIGHT && !g->disableGameplayLogic;
}

BossAdapter GohmaAdapter() {
    BossAdapter a;
    a.GetPhase = Gohma_GetPhase;
    a.OnPhaseChange = Gohma_OnPhaseChange;
    a.ShouldMirror = Gohma_ShouldMirror;
    // same list as TrueCoop's Gohma_SerializeExtras
    a.extras = {
        FIELD(BossGoma, eyeState),         FIELD(BossGoma, eyeIrisRotX),     FIELD(BossGoma, eyeIrisRotY),
        FIELD(BossGoma, eyeLidBottomRotX), FIELD(BossGoma, eyeLidTopRotX),   FIELD(BossGoma, eyeIrisScaleX),
        FIELD(BossGoma, eyeIrisScaleY),    FIELD(BossGoma, visualState),     FIELD(BossGoma, invincibilityFrames),
        FIELD(BossGoma, mainEnvColor),     FIELD(BossGoma, eyeEnvColor),     FIELD(BossGoma, tailLimbsScale),
    };
    // TrueCoop: "deadLimbsState / decayingProgress / noBackfaceCulling deliberately
    // not streamed: the defeat runs locally on both, and deadLimbsState would
    // double-spawn the limb pieces". Plus the cutscene camera, ours to keep.
    a.noCopy = {
        FIELD(BossGoma, deadLimbsState),   FIELD(BossGoma, decayingProgress),      FIELD(BossGoma, noBackfaceCulling),
        FIELD(BossGoma, subCameraId),      FIELD(BossGoma, subCameraEye),          FIELD(BossGoma, subCameraAt),
        FIELD(BossGoma, subCameraFollowSpeed), FIELD(BossGoma, defeatedLimbPositions),
    };
    return a;
}

// --- Gohma larvas (TrueCoop EnGomaAdapter.cpp) ---
//
// Movement comes from the normal enemy copy. These are the fields its update
// computes for drawing: without gomaType a client would draw a larva as an egg
// forever (the egg -> hatch switch happens in the update, which is frozen).
// Shell pieces and pieces of the dead Gohma (params >= 6) are never copied.

BossAdapter EnGomaAdapter() {
    BossAdapter a;
    // same list as TrueCoop's EnGoma_SerializeExtras
    a.extras = {
        FIELD(EnGoma, gomaType),   FIELD(EnGoma, eggScale),        FIELD(EnGoma, eggPitch),
        FIELD(EnGoma, eggSquishAngle), FIELD(EnGoma, eggSquishAmount), FIELD(EnGoma, eggYOffset),
        FIELD(EnGoma, eyePitch),   FIELD(EnGoma, eyeYaw),          FIELD(EnGoma, hurtTimer),
        FIELD(EnGoma, visualState), FIELD(EnGoma, slopePitch),     FIELD(EnGoma, slopeRoll),
        FIELD(EnGoma, eyeEnvColor),
    };
    a.noCopy = { FIELD(EnGoma, bossLimbDL) }; // a display list pointer
    return a;
}

// TrueCoop AdapterRegistry.cpp. Built on first use, no init order worries.
const std::unordered_map<int16_t, BossAdapter>& Adapters() {
    static const std::unordered_map<int16_t, BossAdapter> adapters = {
        { ACTOR_BOSS_GOMA, GohmaAdapter() },
        { ACTOR_EN_GOMA, EnGomaAdapter() },
    };
    return adapters;
}

const BossAdapter* Find(int16_t actorId) {
    auto it = Adapters().find(actorId);
    return it != Adapters().end() ? &it->second : nullptr;
}

// ============================================================================
// Hold boss fights until everyone's there (Crests of Courage boss.cpp)
// ============================================================================
//
// CoC freezes its two bosses' intro (Ook, Diababa: their demo mode / action)
// while somebody is missing. OoT bosses start their intro and their fight in
// their own update, so here we just don't run the update of the BOSS actors
// (ShouldActorUpdate) while we hold: no intro, no colliders, nothing moves.
// Released once everybody is in, after 60 s, or when someone presses D-Pad
// Down. Once released it stays released until we leave the boss room.
//
// CoC's MsgBossReady goes in EventKind::SkipVote, sub-type 1 (sub-type 0 is
// the cutscene vote, CoopSkipVote.cpp): {u16 sceneNum, u8 ready, u8 goAnyway}.

constexpr uint8_t kSubBossReady = 1;

// The boss rooms, same 9 as TrueCoop's BossEntry table (CoopStory.cpp)
constexpr s16 kBossScenes[] = {
    SCENE_DEKU_TREE_BOSS,    SCENE_DODONGOS_CAVERN_BOSS, SCENE_JABU_JABU_BOSS,
    SCENE_FOREST_TEMPLE_BOSS, SCENE_FIRE_TEMPLE_BOSS,    SCENE_WATER_TEMPLE_BOSS,
    SCENE_SPIRIT_TEMPLE_BOSS, SCENE_SHADOW_TEMPLE_BOSS,  SCENE_GANONDORF_BOSS,
};

// CoC's tick numbers are 30 per second, our frames 20: same durations
constexpr int kEntryGraceFrames = 60;      // CoC kEntryGraceTicks 90 = 3 s
constexpr int kReadyLatchCeiling = 400;    // CoC 60 * 10 ticks = 20 s
constexpr int kEventWedgedFrames = 120;    // CoC 60 * 3 ticks = 6 s
constexpr int kMaxWaitFrames = 20 * 60;    // CoC 30 * 60 ticks = 60 s
constexpr uint32_t kReadyStaleFrames = 133; // CoC kReadyStaleTicks 200
constexpr uint32_t kReadyResendFrames = 40; // CoC: every 60 ticks

// What a teammate last said (CoC s_remoteReady / s_readyStamp / s_readyStage)
struct RemoteReady {
    int16_t scene = -1;
    bool ready = false;
    uint32_t stamp = 0;
};
std::map<uint32_t, RemoteReady> sRemoteReady;

struct BossWait {
    int16_t scene = -1;       // the boss room we're in, -1 = not in one
    int roomFrames = 0;       // CoC s_roomTicks
    int eventFrames = 0;      // CoC s_eventTicks
    bool sawEvent = false;    // CoC s_sawEvent
    bool readyLatched = false; // CoC s_readyLatched: our own entry is done
    bool sentInRoom = false;  // CoC s_sentInRoom
    bool sentReady = false;
    uint32_t lastReadySent = 0;
    bool waiting = false;     // CoC s_wasWaiting
    bool released = false;    // the fight went on once: no more holding this visit
    bool forceStart = false;  // CoC s_forceStart (D-Pad Down, timeout, a teammate's go)
    int waitFrames = 0;
    int shownMissing = -1;    // the count last shown on screen
};
BossWait sWait;
uint32_t sTick = 0;
// read by ShouldActorUpdate, every actor every frame: a plain bool
bool sHolding = false;

bool IsBossScene(s16 scene) {
    for (s16 s : kBossScenes) {
        if (s == scene) {
            return true;
        }
    }
    return false;
}

// CoC "retired" (boss_defeated): beaten bosses don't spawn again, so no BOSS
// actor in the room = nothing to hold
bool BossPresent() {
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head; a != nullptr; a = a->next) {
        if (a->update != nullptr) {
            return true;
        }
    }
    return false;
}

// CoC's "alink->checkEventRun()": a cutscene or Link held by one
bool LocalEventRunning() {
    return Player_InCsMode(gPlayState) || gPlayState->csCtx.state != CS_STATE_IDLE;
}

// CoC update_ready_latch: we count as "here" once our own way in is over
void UpdateReadyLatch() {
    if (sWait.readyLatched) {
        return;
    }
    bool noEvent = !LocalEventRunning();
    sWait.eventFrames = noEvent ? 0 : sWait.eventFrames + 1;
    // our own hold can keep a cutscene from ever ending: don't wait on ourselves then
    bool wedged = sHolding && sWait.eventFrames > kEventWedgedFrames;
    if (!noEvent) {
        sWait.sawEvent = true;
    }
    bool settled = noEvent && (sWait.sawEvent || sWait.roomFrames > kEntryGraceFrames);
    if (settled || wedged || sWait.roomFrames > kReadyLatchCeiling) {
        sWait.readyLatched = true;
        SPDLOG_INFO("[Coop][Boss] entry done ({}), ready",
                    settled ? (sWait.sawEvent ? "the entry cutscene ended" : "there was no entry cutscene")
                            : (wedged ? "the intro is wedged behind our own hold" : "waited long enough"));
    }
}

// CoC fight_ready + player_ready for a teammate
bool TeammateReady(uint32_t id) {
    auto it = sRemoteReady.find(id);
    if (it == sRemoteReady.end()) {
        return false;
    }
    const RemoteReady& r = it->second;
    return r.ready && r.scene == sWait.scene && sTick - r.stamp <= kReadyStaleFrames;
}

// CoC peers_pending: teammates not in the room yet (us not counted)
int PeersPending() {
    int missing = 0;
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (!TeammateReady(p.id)) {
            ++missing;
        }
    }
    return missing;
}

// CoC send_ready
void SendReady(bool inRoom, bool goAnyway) {
    Coop::ByteWriter w;
    w.U8(kSubBossReady);
    w.U16(static_cast<uint16_t>(sWait.scene));
    w.U8(inRoom ? 1 : 0);
    w.U8(goAnyway ? 1 : 0);
    CoopGame::SendEvent(Coop::EventKind::SkipVote, w.Take());
}

void StopHolding(const char* why) {
    if (sHolding) {
        SPDLOG_INFO("[Coop][Boss] fight released ({})", why);
    }
    sHolding = false;
}

// left the boss room / session over: start over next time (CoC update_wait "!here" part)
void LeaveBossRoom() {
    if (sWait.sentInRoom && CoopGame::SessionActive()) {
        SendReady(false, false);
    }
    StopHolding("left the boss room");
    sWait = BossWait{};
}

// CoC boss_force_start
void ForceStart(const char* why) {
    if (!sWait.waiting || sWait.forceStart) {
        return;
    }
    sWait.forceStart = true;
    SendReady(sWait.sentInRoom, true);
    SPDLOG_INFO("[Coop][Boss] {}", why);
}

// CoC update_wait, every frame while in a session
void UpdateBossWait() {
    ++sTick;
    if (gPlayState == nullptr || !CoopGame::LocalState().inGame || !IsBossScene(gPlayState->sceneNum)) {
        if (sWait.scene != -1) {
            LeaveBossRoom();
        }
        return;
    }
    if (sWait.scene != gPlayState->sceneNum) {
        LeaveBossRoom();
        sWait.scene = gPlayState->sceneNum;
        SPDLOG_INFO("[Coop][Boss] entered boss room {} waitEnabled={}", sWait.scene, CoopGame::RoomBossWait());
    }

    ++sWait.roomFrames;
    UpdateReadyLatch();

    // tell the others we're here (and ready), on change and every ~2 s.
    // goAnyway stays on once the fight runs, so a late arrival starts too (CoC
    // sends s_forceStart with every ready, and checks "underway")
    bool fightOn = sWait.forceStart || sWait.released;
    if (sWait.readyLatched != sWait.sentReady || !sWait.sentInRoom || sTick - sWait.lastReadySent > kReadyResendFrames) {
        sWait.sentInRoom = true;
        sWait.sentReady = sWait.readyLatched;
        sWait.lastReadySent = sTick;
        SendReady(sWait.readyLatched, fightOn);
    }

    if (sWait.waiting && ++sWait.waitFrames > kMaxWaitFrames && !sWait.forceStart) {
        sWait.forceStart = true;
        SPDLOG_INFO("[Coop][Boss] wait timed out after {} frames, releasing", sWait.waitFrames);
    }

    int missingShown = PeersPending();
    int missing = missingShown + (sWait.readyLatched ? 0 : 1);
    bool retired = !BossPresent();
    bool waiting =
        CoopGame::RoomBossWait() && !sWait.forceStart && !sWait.released && missing > 0 && !retired;

    if (waiting) {
        sHolding = true;
        if (!sWait.waiting) {
            sWait.waiting = true;
            sWait.waitFrames = 0;
            SPDLOG_INFO("[Coop][Boss] fight held, {} missing", missing);
        }
        // CoC's popup, only about the others (not while our own way in plays)
        if (missingShown != sWait.shownMissing) {
            sWait.shownMissing = missingShown;
            if (missingShown > 0) {
                CoopGame::Notify(CoopGame::NoticeKind::Players,
                                 "Waiting for " + std::to_string(missingShown) +
                                     (missingShown == 1 ? " Player" : " Players") +
                                     "\nPress D-Pad Down to continue anyway");
            }
        }
        if (CHECK_BTN_ALL(gPlayState->state.input[0].press.button, BTN_DDOWN)) {
            ForceStart("go anyway pressed");
        }
    } else if (sWait.waiting) {
        sWait.waiting = false;
        sWait.released = true;
        sWait.waitFrames = 0;
        sWait.shownMissing = -1;
        StopHolding(sWait.forceStart ? "continue anyway" : "everyone is here");
    } else {
        StopHolding("not waiting");
    }
}

// CoC boss_on_message, kMsgBossReady part
void OnBossReadyEvent(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint8_t sub = 0;
    uint16_t scene = 0;
    uint8_t inRoom = 0;
    uint8_t goAnyway = 0;
    if (!r.U8(sub) || sub != kSubBossReady) {
        return; // the cutscene vote (CoopSkipVote.cpp)
    }
    if (!r.U16(scene) || !r.U8(inRoom) || !r.U8(goAnyway) || r.Remaining() != 0 || inRoom > 1 || goAnyway > 1 ||
        e.origin == 0 || e.origin == CoopGame::SelfId()) {
        return;
    }
    RemoteReady& ready = sRemoteReady[e.origin];
    ready.scene = static_cast<int16_t>(scene);
    ready.ready = inRoom != 0;
    ready.stamp = sTick;
    // CoC: a teammate in the same room said go: we go too
    if (goAnyway != 0 && sWait.sentInRoom && ready.scene == sWait.scene && sWait.waiting) {
        ForceStart("a teammate started the fight");
    }
}

void OnShouldActorUpdate(void* ptr, bool* should) {
    if (sHolding && static_cast<Actor*>(ptr)->category == ACTORCAT_BOSS) {
        *should = false; // held at the door: no intro, no fight yet
    }
}

void Register() {
    Adapters(); // build the table now rather than in the middle of a fight

    CoopGame::OnEveryFrame("CoopBosses.Wait", UpdateBossWait);
    CoopGame::OnEvent(Coop::EventKind::SkipVote, "CoopBosses.Wait", OnBossReadyEvent);
    COND_HOOK(ShouldActorUpdate, true, OnShouldActorUpdate);
    // Arm the hold before the boss's first update: a boss room with the
    // setting on starts held, the frame callback lets it go when it can
    COND_HOOK(OnSceneInit, true, [](int16_t sceneNum) {
        std::lock_guard lock(CoopGame::Mutex());
        if (CoopGame::SessionActive() && CoopGame::RoomBossWait() && IsBossScene(sceneNum)) {
            sHolding = true;
        }
    });
    // CoC: disconnected during the door wait -> release the fight
    COND_HOOK(OnGameFrameUpdate, true, [] {
        std::lock_guard lock(CoopGame::Mutex());
        if (!CoopGame::SessionActive() && (sHolding || sWait.scene != -1)) {
            StopHolding("session over");
            sWait = BossWait{};
            sRemoteReady.clear();
        }
    });
}

} // namespace

// Used by CoopEnemies.cpp (declared there).
namespace CoopBosses {

bool HasAdapter(int16_t actorId) {
    return Find(actorId) != nullptr;
}

uint8_t GetPhase(Actor* actor) {
    const BossAdapter* a = Find(actor->id);
    return (a != nullptr && a->GetPhase != nullptr) ? a->GetPhase(actor) : 0;
}

bool OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase) {
    const BossAdapter* a = Find(actor->id);
    return a != nullptr && a->OnPhaseChange != nullptr && a->OnPhaseChange(actor, fromPhase, toPhase);
}

bool ShouldMirror(Actor* actor, uint8_t streamedPhase) {
    const BossAdapter* a = Find(actor->id);
    return a == nullptr || a->ShouldMirror == nullptr || a->ShouldMirror(actor, streamedPhase);
}

// `body` is the host's memory after the common Actor part (EnemyState::body).
void ApplyExtras(Actor* actor, const std::vector<uint8_t>& body) {
    const BossAdapter* a = Find(actor->id);
    if (a == nullptr) {
        return;
    }
    uint8_t* bytes = reinterpret_cast<uint8_t*>(actor);
    for (const Field& f : a->extras) {
        if (f.offset < sizeof(Actor) || f.offset - sizeof(Actor) + f.size > body.size()) {
            continue; // not the same layout (or no body this frame): leave ours
        }
        memcpy(bytes + f.offset, body.data() + (f.offset - sizeof(Actor)), f.size);
    }
}

void AddNoCopyRanges(Actor* actor, std::vector<std::pair<size_t, size_t>>& out) {
    const BossAdapter* a = Find(actor->id);
    if (a == nullptr) {
        return;
    }
    for (const Field& f : a->noCopy) {
        out.emplace_back(f.offset, f.offset + f.size);
    }
}

} // namespace CoopBosses

static RegisterShipInitFunc sInit(Register);
