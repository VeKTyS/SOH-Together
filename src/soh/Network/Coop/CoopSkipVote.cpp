// Vote to skip a cutscene, from Crests of Courage skipvote.cpp.
//
// CoC: when several players watch the same cutscene, pressing Start is your
// vote, and the skip only happens once everybody watching voted ("1/2 want to
// skip"). Until then your own skip is held back.
//
// OoT has no skip button for players. The only skip in the game is the debug
// one (z_demo.c Cutscene_Command_Terminator: Start + DebugEnabled jumps to the
// cutscene's end, where it was going anyway). So when everybody voted we press
// that button for the game: Start + DebugEnabled for one frame, right before
// the cutscene code reads them (our OnPlayerUpdate runs inside Actor_UpdateAll,
// the cutscene runs after it in Play_Update), and DebugEnabled goes back at the
// end of the frame. Only scripted cutscenes (csCtx) can be skipped this way,
// and only the ones that have that end command; the others just play on.
//
// Data of EventKind::SkipVote, first byte = sub-type:
//   0 (this file): {u8 inCutscene, u8 voted, u16 sceneNum}  CoC MsgSkipVote (stage -> scene)
//   1: boss "continue anyway", see CoopBosses.cpp
#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include <map>
#include <string>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {

constexpr uint8_t kSubVote = 0;

// CoC's numbers (ticks = game frames here)
constexpr uint32_t kStaleTicks = 180; // a watcher we haven't heard from in this long doesn't count
constexpr uint32_t kResendTicks = 60; // while in a cutscene, say it again this often
// the debug skip only works past frame 20 of the cutscene (z_demo.c)
constexpr u16 kMinSkipFrame = 20;
// pressed Start for the game this many frames and the cutscene is still on:
// it has no end command, it can't be skipped
constexpr int kMaxSkipPresses = 10;

// CoC Watcher
struct Watcher {
    bool inCutscene = false;
    bool voted = false;
    int16_t scene = -1;
    uint32_t heardTick = 0;
};
std::map<uint32_t, Watcher> sRemote;
uint32_t sTick = 0;

bool sVoted = false;
bool sSkipping = false;
bool sWasInCutscene = false;
uint32_t sResendTicks = 0;
int sSkipPresses = 0;

// we turned DebugEnabled on for this frame: put it back at the end of it
bool sRestoreDebug = false;

// CoC local_in_cutscene: a scripted cutscene that can still be skipped
// (csCtx in its SKIPPABLE states, played by func_800645A0 when cutsceneIndex >= 0xFFF0)
bool LocalInCutscene() {
    if (gPlayState == nullptr || GET_PLAYER(gPlayState) == nullptr || gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return false;
    }
    u8 state = gPlayState->csCtx.state;
    return (state == CS_STATE_SKIPPABLE_INIT || state == CS_STATE_SKIPPABLE_EXEC) &&
           gSaveContext.cutsceneIndex >= 0xFFF0;
}

int16_t LocalScene() {
    return gPlayState != nullptr ? gPlayState->sceneNum : -1;
}

// CoC count_votes: us + everyone we heard from lately who watches a cutscene in our scene
void CountVotes(int& voted, int& needed) {
    voted = sVoted ? 1 : 0;
    needed = 1;
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        auto it = sRemote.find(p.id);
        if (it == sRemote.end()) {
            continue;
        }
        const Watcher& w = it->second;
        if (!w.inCutscene || w.scene != LocalScene() || sTick - w.heardTick > kStaleTicks) {
            continue;
        }
        ++needed;
        if (w.voted) {
            ++voted;
        }
    }
}

// CoC announce
void Announce() {
    Coop::ByteWriter w;
    w.U8(kSubVote);
    w.U8(LocalInCutscene() ? 1 : 0);
    w.U8(sVoted ? 1 : 0);
    w.U16(static_cast<uint16_t>(LocalScene()));
    CoopGame::SendEvent(Coop::EventKind::SkipVote, w.Take());
}

// CoC say_count
void SayCount() {
    int voted = 0;
    int needed = 0;
    CountVotes(voted, needed);
    CoopGame::Notify(CoopGame::NoticeKind::Other,
                     "Skip\n" + std::to_string(voted) + "/" + std::to_string(needed) + " want to skip");
}

// Press the debug skip for the game this frame (see the top of the file)
void PressSkip() {
    CutsceneContext& cs = gPlayState->csCtx;
    if (cs.state != CS_STATE_SKIPPABLE_EXEC || cs.frames < kMinSkipFrame) {
        return; // too early, the game would ignore it: wait
    }
    if (++sSkipPresses > kMaxSkipPresses) {
        if (sSkipPresses == kMaxSkipPresses + 1) {
            SPDLOG_INFO("[Coop][Skip] this cutscene has no end to jump to, it plays on");
            CoopGame::Notify(CoopGame::NoticeKind::Other, "Skip\nThis cutscene can't be skipped.");
        }
        return;
    }
    gPlayState->state.input[0].press.button |= BTN_START;
    if (CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0) == 0) {
        CVarSetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 1);
        sRestoreDebug = true;
    }
}

// CoC begin_skip
void BeginSkip() {
    sSkipping = true;
    sSkipPresses = 0;
    SPDLOG_INFO("[Coop][Skip] all voted, skipping");
}

// CoC on_skipper, run from Link's update (before the cutscene code reads the buttons)
void HandleSkipInput() {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive() || !LocalInCutscene()) {
        return;
    }
    Input& input = gPlayState->state.input[0];
    bool start = CHECK_BTN_ALL(input.press.button, BTN_START);
    // Start is a vote now, never a skip on its own (a player with the debug
    // mode on would otherwise skip alone)
    input.press.button &= ~BTN_START;

    if (sSkipping) {
        PressSkip();
        return;
    }
    int voted = 0;
    int needed = 0;
    CountVotes(voted, needed);
    if (sVoted && voted >= needed) {
        BeginSkip();
        PressSkip();
        return;
    }
    if (start && !sVoted) {
        sVoted = true;
        Announce();
        CountVotes(voted, needed);
        SPDLOG_INFO("[Coop][Skip] local vote ({}/{})", voted, needed);
        if (voted >= needed) {
            BeginSkip();
            PressSkip();
            return;
        }
        SayCount();
    }
}

// CoC skipvote_on_message
void OnSkipVoteEvent(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint8_t sub = 0;
    uint8_t inCutscene = 0;
    uint8_t voted = 0;
    uint16_t scene = 0;
    if (!r.U8(sub) || sub != kSubVote) {
        return; // another sub-type (CoopBosses.cpp)
    }
    if (!r.U8(inCutscene) || !r.U8(voted) || !r.U16(scene) || r.Remaining() != 0 || inCutscene > 1 || voted > 1 ||
        e.origin == 0 || e.origin == CoopGame::SelfId()) {
        return;
    }
    Watcher& w = sRemote[e.origin];
    bool newVote = voted != 0 && !w.voted;
    w.inCutscene = inCutscene != 0;
    w.voted = voted != 0 && w.inCutscene;
    w.scene = static_cast<int16_t>(scene);
    w.heardTick = sTick;
    if (newVote && LocalInCutscene() && w.scene == LocalScene()) {
        SayCount();
    }
}

void ResetAll() {
    sRemote.clear();
    sVoted = false;
    sSkipping = false;
    sWasInCutscene = false;
    sResendTicks = 0;
    sSkipPresses = 0;
}

// CoC skipvote_update (session on)
void OnFrame() {
    ++sTick;
    bool now = LocalInCutscene();
    if (now != sWasInCutscene) {
        sWasInCutscene = now;
        if (!now) {
            sVoted = false;
            sSkipping = false;
            sSkipPresses = 0;
        }
        Announce();
        sResendTicks = 0;
        return;
    }
    if (now && ++sResendTicks >= kResendTicks) {
        sResendTicks = 0;
        Announce();
    }
}

void Register() {
    CoopGame::OnEveryFrame("CoopSkipVote", OnFrame);
    CoopGame::OnEvent(Coop::EventKind::SkipVote, "CoopSkipVote", OnSkipVoteEvent);
    COND_HOOK(OnPlayerUpdate, true, HandleSkipInput);
    // end of every frame: DebugEnabled back to off, and forget everything once
    // the session is gone (CoC skipvote_update's "not connected" part)
    COND_HOOK(OnGameFrameUpdate, true, [] {
        std::lock_guard lock(CoopGame::Mutex());
        if (sRestoreDebug) {
            CVarSetInteger(CVAR_DEVELOPER_TOOLS("DebugEnabled"), 0);
            sRestoreDebug = false;
        }
        if (!CoopGame::SessionActive()) {
            ResetAll();
        }
    });
}

} // namespace

static RegisterShipInitFunc sInit(Register);
