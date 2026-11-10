// Downed / dead / spectating / revived, copied from TrueCoop's CoopLifeSync.cpp
// (+ Packets/PlayerLifeState.cpp for the message, + the life bits of
// Packets/BgmSync.cpp for the music).
//
// How it plays (TrueCoop's words, more or less):
// - die with a fairy in a bottle: vanilla, the fairy revives you, nothing to do.
// - die without a fairy while a teammate is still up: you stay on the ground
//   (we block the game over menu, so the game doesn't pause for anyone) and
//   the camera follows that teammate. If they're in another scene we warp your
//   body there once.
// - everyone is down: the vanilla game over runs, so you all respawn together.
#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>

#include <map>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h" // gPlayerAnim_link_derth_rebirth
extern PlayState* gPlayState;

// z_player.c, not in any header (non-static in 9.3.1). Plays the collapse
// animation and starts the game over. We call it again to kill our body after
// a follow warp spawned Link standing. gPlayerAnim_link_derth_rebirth is a
// string (the asset path), cast at the call like TrueCoop does.
void func_80836448(PlayState* play, Player* thisx, LinkAnimationHeader* anim);
}

namespace {

using CoopGame::LifeState;

// Our own state. Same as LifeState plus Dying (death animation still playing,
// no fairy), which isn't sent: it goes out as Alive. We only say Downed once
// the animation is done and we're stuck on the ground.
enum class LocalLife { Alive, Reviving, Dying, Downed, GameOver };

LocalLife sLife = LocalLife::Alive;

// Set by VB_ADVANCE_TO_GAME_OVER_MENU the frame it holds back the game over
// menu (a teammate is still up). That's what tells Downed (lying there) from
// Dying (still falling): both are at GAMEOVER_DEATH_WAIT_GROUND.
bool sBlockedFunnel = false;

// Only ONE follow warp per scene change. The void damage skip in the position
// warp uses a single hook id, two warps at once would mess it up.
bool sFollowWarpPending = false;

// Frames left to undo the game over darkening after we became Downed.
s32 sUndimFrames = 0;

// Last frame's state, for the music (BgmSync's sPrevLifeState).
LifeState sPrevLife = LifeState::Alive;
// Frames since we last sent our state (resent every few seconds for late joiners).
uint32_t sResendFrames = 0;
constexpr uint32_t kResendFrames = 60; // ~3 s at 20 fps

// What the others told us. Unknown = Alive.
std::map<uint32_t, LifeState> sTeam;

// The state we send for our local state. Dying goes out as Alive.
LifeState WireOf(LocalLife s) {
    switch (s) {
        case LocalLife::Reviving:
            return LifeState::Reviving;
        case LocalLife::Downed:
            return LifeState::Downed;
        case LocalLife::GameOver:
            return LifeState::GameOver;
        case LocalLife::Alive:
        case LocalLife::Dying:
        default:
            return LifeState::Alive;
    }
}

LifeState TeammateLife(uint32_t id) {
    auto it = sTeam.find(id);
    return it == sTeam.end() ? LifeState::Alive : it->second;
}

// Anchor's IsClientAlive
bool IsAlive(LifeState s) {
    return s == LifeState::Alive || s == LifeState::Reviving;
}

// The first teammate in game who is alive (or being revived by a fairy). Looked
// up every frame, never kept. Any scene on purpose: a teammate alive somewhere
// else still holds back our game over (we go follow them). Someone who leaves
// is gone from Players(), so that releases our game over.
const Coop::RemotePlayer* FindLivingPeer() {
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (!p.state || !p.state->inGame) {
            continue;
        }
        if (IsAlive(TeammateLife(p.id))) {
            return &p;
        }
    }
    return nullptr;
}

bool PartnerAlive() {
    return FindLivingPeer() != nullptr;
}

// copied from Packets/PlayerLifeState.cpp SendPacket_PlayerLifeState: { state, sceneNum }
void SendLifeState(LifeState state) {
    if (gPlayState == nullptr) {
        return;
    }
    Coop::ByteWriter w;
    w.U8(static_cast<uint8_t>(state));
    w.U16(static_cast<uint16_t>(gPlayState->sceneNum));
    CoopGame::SendEvent(Coop::EventKind::LifeState, w.Take());
    sResendFrames = 0;
}

// copied from Packets/PlayerLifeState.cpp HandlePacket_PlayerLifeState
void OnLifeStateEvent(const Coop::Event& e) {
    if (e.origin == 0 || e.origin == CoopGame::SelfId()) {
        return;
    }
    Coop::ByteReader r(e.data);
    uint8_t state = 0;
    uint16_t sceneNum = 0;
    if (!r.U8(state) || !r.U16(sceneNum) || state > static_cast<uint8_t>(LifeState::GameOver)) {
        return; // broken packet
    }
    sTeam[e.origin] = static_cast<LifeState>(state);
    SPDLOG_INFO("[LifeSync] rx client={} state={} scene={}", e.origin, state, sceneNum);
}

// State changed: log it and tell the others. Sending on a change that's the
// same on the wire (Alive -> Dying) is a harmless resend.
void SetLife(LocalLife next, const char* label) {
    if (sLife == next) {
        return;
    }
    SPDLOG_INFO("[LifeSync] local {}", label);
    sLife = next;
    SendLifeState(WireOf(next));
}

// Work out our state from the game over context this frame (TrueCoop UpdateLocalLife).
void UpdateLocalLife(PlayState* play) {
    s16 gs = play->gameOverCtx.state;

    // Reviving first (fairy). gameOverCtx jumps into the revive range the same
    // frame func_80836448 uses the fairy, never through a DEATH_* state, so a
    // fairy revive can't end up Dying / Downed / spectating.
    if (gs >= GAMEOVER_REVIVE_START) {
        sBlockedFunnel = false;
        SetLife(LocalLife::Reviving, "ALIVE->REVIVING");
        return;
    }

    if (gs == GAMEOVER_INACTIVE) {
        // Not dying. Back to Alive once really alive (revive done, or respawned
        // after the game over). The health check skips the one frame where the
        // hit zeroed health but func_80836448 didn't start the game over yet.
        sBlockedFunnel = false;
        if (sLife != LocalLife::Alive && gSaveContext.health > 0) {
            SetLife(LocalLife::Alive, "->ALIVE");
        }
        return;
    }

    // No fairy death: gs in DEATH_START (1) .. DEATH_MENU (4).
    if (gs >= GAMEOVER_DEATH_DELAY_MENU) {
        // The VB let it go on (everyone else is down too): vanilla game over menu.
        SetLife(LocalLife::GameOver, "DOWNED->GAME_OVER");
        return;
    }

    // DEATH_START or DEATH_WAIT_GROUND: still falling, or lying there.
    if (sBlockedFunnel) {
        SetLife(LocalLife::Downed, "DYING->DOWNED");
    } else {
        SetLife(LocalLife::Dying, "ALIVE->DYING");
    }
}

// Spectate (TrueCoop UpdateSpectate): follow the living teammate. Same scene =
// free camera behind them. Other scene = one position warp to go there, our
// body gets killed again after the loading (OnSceneLoaded below).
void UpdateSpectate(PlayState* play) {
    const Coop::RemotePlayer* peer = FindLivingPeer();
    if (peer == nullptr) {
        // nobody left alive (dead or gone). The VB lets the game over go on next
        // frame, nothing to follow meanwhile.
        return;
    }
    const Coop::PlayerState& them = *peer->state;

    if (them.sceneNum != play->sceneNum) {
        // respawnFlag = 1 in the position warp, so no entrance cutscene (good for
        // a dead body following someone). Once in the same scene the camera
        // picks them up.
        if (!sFollowWarpPending) {
            CoopGame::RequestPositionWarp(them);
            sFollowWarpPending = true;
            SPDLOG_INFO("[LifeSync] spectate follow client={} scene={} (warp)", peer->id, them.sceneNum);
        }
        return;
    }
    sFollowWarpPending = false;

    // Same scene: only once the death cutscene camera gave the main camera
    // back, writing the main camera while another one is active does nothing.
    if (play->activeCamera != CAM_ID_MAIN) {
        return;
    }
    Camera* cam = GET_ACTIVE_CAM(play);
    if (cam == nullptr) {
        return;
    }

    // CAM_SET_FREE0 runs Camera_Unique6 which never writes at/eye, so what we
    // write stays. Asked again every frame so floor / water camera triggers
    // don't take it back while the world runs. (TrueCoop's
    // Camera_ChangeSetting is called Camera_RequestSetting in 9.3.1.)
    Camera_RequestSetting(cam, CAM_SET_FREE0);

    Vec3f p = { them.x, them.y, them.z };
    s16 yaw = them.yaw;
    f32 dist = 140.0f;
    Vec3f targetAt = { p.x, p.y + 40.0f, p.z };
    // behind them: pos - forward * dist, forward = (sin(yaw), cos(yaw))
    Vec3f targetEye = { p.x - dist * Math_SinS(yaw), p.y + 60.0f, p.z - dist * Math_CosS(yaw) };

    // slide toward the target so it doesn't snap every frame
    f32 t = 0.25f;
    cam->at.x += (targetAt.x - cam->at.x) * t;
    cam->at.y += (targetAt.y - cam->at.y) * t;
    cam->at.z += (targetAt.z - cam->at.z) * t;
    cam->eye.x += (targetEye.x - cam->eye.x) * t;
    cam->eye.y += (targetEye.y - cam->eye.y) * t;
    cam->eye.z += (targetEye.z - cam->eye.z) * t;
    cam->eyeNext = cam->eye;
    cam->roll = 0;
    cam->fov = 60.0f;

    static uint32_t sSpectateLog = 0;
    if ((sSpectateLog++ % 90) == 0) {
        SPDLOG_INFO("[LifeSync] spectate follow client={}", peer->id);
    }
}

// The music part, copied from TrueCoop Packets/BgmSync.cpp Tick() (it reads the
// life state there, we do it right here).
void UpdateMusic(PlayState* play) {
    LifeState life = WireOf(sLife);
    if (life == LifeState::Downed && sPrevLife != LifeState::Downed) {
        // Just went Downed (a teammate is up). The vanilla death already put the
        // game over music on (z_player.c func_80836448): bring back the scene
        // music so we hear the world while watching (same as z_play.c does on load).
        Environment_PlaySceneSequence(play);
        gSaveContext.seqId = play->sequenceCtx.seqId;
        gSaveContext.natureAmbienceId = play->sequenceCtx.natureAmbienceId;
        SPDLOG_INFO("[Bgm] spectate restore scene bgm (seqId={})", play->sequenceCtx.seqId);
    } else if (life == LifeState::GameOver && sPrevLife == LifeState::Downed) {
        // We died first and were watching with the scene music, now everyone is
        // down. Vanilla only played the game over music at our death (which we
        // replaced), so put it back, same lines as func_80836448. TrueCoop calls
        // func_800F6AB0(0), that's Audio_StopBgmAndFanfare(0) in 9.3.1.
        Audio_StopBgmAndFanfare(0);
        Audio_PlayFanfare(NA_BGM_GAME_OVER);
        gSaveContext.seqId = (u8)NA_BGM_DISABLED;
        gSaveContext.natureAmbienceId = NATURE_ID_DISABLED;
        SPDLOG_INFO("[Bgm] downed->game-over restore game-over music");
    }
    sPrevLife = life;
}

void ResetLocal() {
    sLife = LocalLife::Alive;
    sBlockedFunnel = false;
    sFollowWarpPending = false;
    sUndimFrames = 0;
    sPrevLife = LifeState::Alive;
    sResendFrames = 0;
    sTeam.clear();
}

// Every frame (TrueCoop Tick). OnGameFrameUpdate runs after the player update,
// so the VB flag of this frame is already set.
void Tick() {
    if (gPlayState == nullptr || !CoopGame::LocalState().inGame) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return;
    }

    LocalLife prev = sLife;
    UpdateLocalLife(gPlayState);

    if (sLife == LocalLife::Downed && prev != LocalLife::Downed) {
        // just went down: undo the darkening for a bit, reset the follow latch
        sUndimFrames = 50;
        sFollowWarpPending = false;
    }

    if (sLife == LocalLife::Downed) {
        UpdateSpectate(gPlayState);
        if (sUndimFrames > 0) {
            Environment_FadeOutGameOverLights(gPlayState);
            sUndimFrames--;
        }
    } else {
        sFollowWarpPending = false;
    }

    UpdateMusic(gPlayState);

    // TrueCoop also sends the life state inside its client state packets (late
    // joiners get it). We don't have that, so resend it every few seconds.
    if (++sResendFrames >= kResendFrames) {
        SendLifeState(WireOf(sLife));
    }
}

// VB 1 (z_player.c func_80843AE8): hold back WAIT_GROUND -> DELAY_MENU while a
// teammate is alive, so the dead player stays on the ground, pauseCtx never
// goes to 8, the world keeps running and we spectate. Once nobody else is up
// we let it go: vanilla game over, everyone respawns together. It's asked
// every frame while lying there, so "letting go" is just this turning false.
void OnAdvanceToGameOverMenu(bool* should) {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive()) {
        return;
    }
    if (PartnerAlive()) {
        *should = false;
        sBlockedFunnel = true;
    }
}

// VB 2 (z_play.c Play_DrawOverlayElements): no game over darkening during a
// solo no-fairy death (teammate alive, gameOverCtx in the DEATH range), so the
// watching player's screen isn't dark. Left alone for a fairy revive and for a
// real everyone-dead game over.
void OnFadeInGameOverLights(bool* should) {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive() || gPlayState == nullptr) {
        return;
    }
    if (PartnerAlive()) {
        s16 gs = gPlayState->gameOverCtx.state;
        if (gs >= GAMEOVER_DEATH_START && gs < GAMEOVER_REVIVE_START) {
            *should = false;
        }
    }
}

// After a follow warp drops us in the teammate's scene, Link spawns standing
// with 0 health. Run the death again so the body stays down (if a fairy got
// picked up meanwhile, health > 0 skips this and the fairy revive plays,
// TrueCoop accepts that). Only while Downed, normal respawns aren't touched.
void OnSceneLoaded(int16_t) {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive() || sLife != LocalLife::Downed || gPlayState == nullptr) {
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr && gSaveContext.health == 0) {
        func_80836448(gPlayState, player, (LinkAnimationHeader*)gPlayerAnim_link_derth_rebirth);
        // The kill puts gameOverCtx back to DEATH_START, and the loading
        // already cleared sBlockedFunnel. Without this the next frame would go
        // Downed -> Dying and send Alive for ~4 s. We're still held on the
        // ground (we're Downed so someone is up), so keep saying Downed.
        sBlockedFunnel = true;
        // the kill also put the game over music back on: we're still watching
        // the others, so scene music again (TrueCoop forgets this one)
        Environment_PlaySceneSequence(gPlayState);
        gSaveContext.seqId = gPlayState->sequenceCtx.seqId;
        gSaveContext.natureAmbienceId = gPlayState->sequenceCtx.natureAmbienceId;
    }
    sFollowWarpPending = false;
}

void RegisterCoopLife() {
    CoopGame::OnEveryFrame("CoopLife", Tick);
    CoopGame::OnEvent(Coop::EventKind::LifeState, "CoopLife", OnLifeStateEvent);

    // TrueCoop resets on disconnect (RegisterCoopLifeSyncHooks). Our frame
    // callbacks stop with the session, so watch for it here.
    COND_HOOK(OnGameFrameUpdate, true, [] {
        std::lock_guard lock(CoopGame::Mutex());
        if (!CoopGame::SessionActive()) {
            ResetLocal();
        }
    });

    COND_VB_SHOULD(VB_ADVANCE_TO_GAME_OVER_MENU, true, { OnAdvanceToGameOverMenu(should); });
    COND_VB_SHOULD(VB_FADE_IN_GAME_OVER_LIGHTS, true, { OnFadeInGameOverLights(should); });
    COND_HOOK(OnTransitionEnd, true, OnSceneLoaded);
}

} // namespace

namespace CoopGame {

LifeState LocalLifeState() {
    std::lock_guard lock(Mutex());
    return WireOf(sLife);
}

LifeState PlayerLifeState(uint32_t playerId) {
    std::lock_guard lock(Mutex());
    if (playerId == SelfId()) {
        return WireOf(sLife);
    }
    return TeammateLife(playerId);
}

} // namespace CoopGame

static RegisterShipInitFunc sCoopLifeInit(RegisterCoopLife);
