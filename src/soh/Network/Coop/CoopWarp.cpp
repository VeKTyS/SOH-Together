// Warps queued until it's safe to go (TrueCoop's CoopWarp.cpp/.h, copied).
// One warp waits at a time (the newest replaces the older one). It only fires
// when nothing is in the way: no cutscene, no loading zone already going, not
// paused / in the game over menu. If that never happens in 30 s we drop it
// instead of warping into some weird state later.
// These skip the Teleport button's rules (TeleportBlocker) on purpose, like
// TrueCoop: the co-op code asks for them, not the player.
#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {

enum class PendingWarpKind { None, Entrance, Position };

struct PendingWarp {
    PendingWarpKind kind = PendingWarpKind::None;
    s16 entranceIndex = 0;
    s32 cutsceneIndex = -1; // entrance warp only (s32: the 0xFFF0+ scene cutscenes don't fit a s16)
    s16 room = 0;           // position warp only
    PosRot dest = {};       // position warp only
    std::chrono::steady_clock::time_point queuedAt;
};

PendingWarp sPending;

// copied from TrueCoop: drop a warp that never found a good moment
constexpr auto WARP_EXPIRY = std::chrono::seconds(30);

// Plain entrance warp, copied from TrueCoop ExecuteEntranceWarp. respawnFlag is
// left alone (0) so the entrance cutscene still plays when we arrive
// (Cutscene_HandleEntranceTriggers only runs when respawnFlag <= 0).
void ExecuteEntranceWarp(const PendingWarp& w) {
    if (w.cutsceneIndex >= 0) {
        gSaveContext.cutsceneIndex = w.cutsceneIndex;
    }
    gPlayState->nextEntranceIndex = w.entranceIndex;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.nextTransitionType = TRANS_TYPE_INSTANT;

    SPDLOG_INFO("[CoopWarp] warp entrance={} cs={}", w.entranceIndex, w.cutsceneIndex);
}

// Exact spot warp, copied from TrueCoop ExecutePositionWarp (itself Anchor's
// TeleportTo.cpp, same as CoopGame.cpp TeleportTo): we pretend we fell in a
// hole and respawn where they stand. respawnFlag = 1 means no entrance
// cutscene on arrival. Falling in a hole costs health, skip that once.
void ExecutePositionWarp(const PendingWarp& w) {
    gPlayState->nextEntranceIndex = w.entranceIndex;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = w.entranceIndex;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = (u8)w.room;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = w.dest.pos;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = w.dest.rot.y;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0xDFF;
    gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    gSaveContext.respawnFlag = 1;

    static HOOK_ID hookId = 0;
    hookId = REGISTER_VB_SHOULD(VB_INFLICT_VOID_DAMAGE, {
        *should = false;
        GameInteractor::Instance->UnregisterGameHookForID<GameInteractor::OnVanillaBehavior>(hookId);
    });

    SPDLOG_INFO("[CoopWarp] warp entrance={} cs={}", w.entranceIndex, -1);
}

// Copied from TrueCoop DrainPendingWarp: fire the waiting warp only at a safe
// moment. Never during a cutscene (Player_InCsMode), a loading zone already
// going (transitionTrigger), or the pause / game over menu (pauseCtx.state).
void DrainPendingWarp() {
    if (sPending.kind == PendingWarpKind::None || gPlayState == nullptr) {
        return;
    }

    if (std::chrono::steady_clock::now() - sPending.queuedAt > WARP_EXPIRY) {
        SPDLOG_INFO("[CoopWarp] dropping stale warp entrance={}", sPending.entranceIndex);
        sPending.kind = PendingWarpKind::None;
        return;
    }

    // Player_InCsMode reads our Link, there isn't one for a moment while a scene loads
    if (GET_PLAYER(gPlayState) == nullptr) {
        return;
    }
    if (Player_InCsMode(gPlayState) || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF ||
        gPlayState->pauseCtx.state != 0) {
        return;
    }

    // one shot: clear before going (the warp itself sets transitionTrigger)
    PendingWarp w = sPending;
    sPending.kind = PendingWarpKind::None;

    if (w.kind == PendingWarpKind::Entrance) {
        ExecuteEntranceWarp(w);
    } else {
        ExecutePositionWarp(w);
    }
}

// entrance numbers can come from the network, don't trust them
bool ValidEntrance(int32_t entranceIndex) {
    return entranceIndex >= 0 && entranceIndex < ENTR_MAX;
}

void RegisterCoopWarp() {
    // the queue is emptied every frame while a session is on
    CoopGame::OnEveryFrame("CoopWarp", DrainPendingWarp);

    // TrueCoop drops the waiting warp on disconnect (RegisterCoopWarpHooks). Our
    // OnEveryFrame callbacks stop when the session stops, so watch for it here.
    COND_HOOK(OnGameFrameUpdate, true, [] {
        std::lock_guard lock(CoopGame::Mutex());
        if (!CoopGame::SessionActive()) {
            sPending.kind = PendingWarpKind::None;
        }
    });
}

} // namespace

namespace CoopGame {

// copied from TrueCoop RequestEntranceWarp
void RequestEntranceWarp(int16_t entranceIndex, int32_t cutsceneIndex) {
    std::lock_guard lock(Mutex());
    if (!ValidEntrance(entranceIndex)) {
        SPDLOG_INFO("[CoopWarp] ignoring warp to bad entrance {}", entranceIndex);
        return;
    }
    sPending.kind = PendingWarpKind::Entrance;
    sPending.entranceIndex = entranceIndex;
    sPending.cutsceneIndex = cutsceneIndex;
    sPending.queuedAt = std::chrono::steady_clock::now();
}

// copied from TrueCoop RequestPositionWarp, fed from their PlayerState
void RequestPositionWarp(const Coop::PlayerState& them) {
    std::lock_guard lock(Mutex());
    if (!ValidEntrance(them.entranceIndex)) {
        SPDLOG_INFO("[CoopWarp] ignoring warp to bad entrance {}", them.entranceIndex);
        return;
    }
    sPending.kind = PendingWarpKind::Position;
    sPending.entranceIndex = static_cast<s16>(them.entranceIndex);
    sPending.room = them.roomNum;
    sPending.dest = {};
    sPending.dest.pos = { them.x, them.y, them.z };
    sPending.dest.rot.y = them.yaw;
    sPending.queuedAt = std::chrono::steady_clock::now();
}

} // namespace CoopGame

static RegisterShipInitFunc sCoopWarpInit(RegisterCoopWarp);
