// Randomizer bits copied from Anchor: check status, discovered entrances, seed check.
//
// - a check changes status in the tracker (collected, skipped, hinted...): the
//   others get the same status (Anchor Packets/SetCheckStatus.cpp)
// - an entrance gets discovered (entrance rando): discovered for everybody
//   (Anchor Packets/EntranceDiscovered.cpp)
// - all check statuses are part of the join snapshot (Anchor UpdateTeamState)
// - both players must play the same seed: SeedHash() for the handshake, and an
//   in-game warning like Anchor's room window ("Seed mismatch!")
//
// The CheckStatus event carries a first byte saying what's inside:
//   0 = check status {u16 rc, u8 status, u8 skipped}
//   1 = seed announce {u64 SeedHash()}

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/randomizer/SeedContext.h"
#include "soh/Enhancements/randomizer/randomizer_check_tracker.h"
#include "soh/Enhancements/randomizer/randomizer_entrance.h"
#include "soh/Notification/Notification.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>

#include <map>
#include <span>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {

constexpr uint8_t kTagCheckStatus = 0;
constexpr uint8_t kTagSeed = 1;

// true while we apply something from the network: the game fires the same
// hooks then, don't send it back (Anchor's isResultOfHandling /
// isHandlingUpdateTeamState / isProcessingIncomingPacket)
bool sApplyingRemote = false;

// seed announce: what we told the others last time, and who was around
bool sSeedAnnounced = false;
uint64_t sAnnouncedHash = 0;
uint64_t sAnnouncedPlayers = 0;
// who we already warned about (player id -> their hash), to say it once
std::map<uint32_t, uint64_t> sSeedWarned;

bool CanSend() {
    return !sApplyingRemote && gPlayState != nullptr && CoopGame::LocalState().inGame && CoopGame::SessionActive();
}

// same rule as flags/items: a client waits for the host's snapshot (it has it all)
bool CanApply() {
    return CoopGame::LocalState().inGame && gPlayState != nullptr &&
           (CoopGame::Session().Role() == Coop::SessionRole::Hosting || CoopGame::FlagsSynced());
}

// Anchor Packets/SetCheckStatus.cpp SendPacket_SetCheckStatus
void SendCheckStatus(RandomizerCheck rc) {
    if (rc < 0 || rc >= RC_MAX) {
        return;
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!CanSend() || !IS_RANDO) {
        return;
    }
    Rando::ItemLocation* loc = Rando::Context::GetInstance()->GetItemLocation(rc);
    Coop::ByteWriter w;
    w.U8(kTagCheckStatus);
    w.U16(static_cast<uint16_t>(rc));
    w.U8(static_cast<uint8_t>(loc->GetCheckStatus()));
    w.U8(loc->GetIsSkipped() ? 1 : 0);
    CoopGame::SendEvent(Coop::EventKind::CheckStatus, w.Take());
}

// Anchor HandlePacket_SetCheckStatus
void ApplyCheckStatus(Coop::ByteReader& r) {
    uint16_t rawRc = 0;
    uint8_t rawStatus = 0;
    uint8_t skipped = 0;
    if (!r.U16(rawRc) || !r.U8(rawStatus) || !r.U8(skipped) || r.Remaining() != 0 || rawRc >= RC_MAX ||
        rawStatus > RCSHOW_SAVED || skipped > 1) {
        return;
    }
    if (!CanApply() || !IS_RANDO) {
        return;
    }
    RandomizerCheck rc = static_cast<RandomizerCheck>(rawRc);
    RandomizerCheckStatus status = static_cast<RandomizerCheckStatus>(rawStatus);
    Rando::ItemLocation* loc = Rando::Context::GetInstance()->GetItemLocation(rc);

    sApplyingRemote = true;
    if (loc->GetCheckStatus() != status) {
        loc->SetCheckStatus(status);
    }
    if (loc->GetIsSkipped() != (skipped != 0)) {
        loc->SetIsSkipped(skipped != 0);
    }
    CheckTracker::RecalculateAllAreaTotals();
    CheckTracker::RecalculateAvailableChecks();
    sApplyingRemote = false;
}

// Anchor Packets/EntranceDiscovered.cpp
void SendEntranceDiscovered(u16 entranceIndex) {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CanSend()) {
        return;
    }
    Coop::ByteWriter w;
    w.U16(entranceIndex);
    CoopGame::SendEvent(Coop::EventKind::EntranceDiscovered, w.Take());
}

void OnEntranceDiscovered(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint16_t entranceIndex = 0;
    if (!r.U16(entranceIndex) || r.Remaining() != 0 || !CanApply()) {
        return;
    }
    // Entrance_SetEntranceDiscovered ignores indexes past the rando range itself.
    // "reversed" = 1 like Anchor: the sender also sends the reverse one if any.
    sApplyingRemote = true;
    Entrance_SetEntranceDiscovered(entranceIndex, 1);
    sApplyingRemote = false;
}

// Something that changes when someone joins / leaves / enters the game, so we
// say our seed again to the newcomers.
uint64_t PlayersSignature() {
    uint64_t sig = 0;
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        bool inGame = p.state && p.state->inGame;
        sig = sig * 31 + p.id * 2 + (inGame ? 1 : 0);
    }
    return sig;
}

void AnnounceSeed() {
    if (!CoopGame::LocalState().inGame || gPlayState == nullptr) {
        sSeedAnnounced = false;
        return;
    }
    uint64_t hash = CoopGame::SeedHash();
    uint64_t players = PlayersSignature();
    if (sSeedAnnounced && hash == sAnnouncedHash && players == sAnnouncedPlayers) {
        return;
    }
    Coop::ByteWriter w;
    w.U8(kTagSeed);
    w.U64(hash);
    CoopGame::SendEvent(Coop::EventKind::CheckStatus, w.Take());
    sSeedAnnounced = true;
    sAnnouncedHash = hash;
    sAnnouncedPlayers = players;
}

// Anchor AnchorRoomWindow.cpp: "Seed mismatch! Continuing will break things!"
void CheckSeed(uint32_t origin, Coop::ByteReader& r) {
    uint64_t theirs = 0;
    if (!r.U64(theirs) || r.Remaining() != 0 || !CoopGame::LocalState().inGame) {
        return;
    }
    uint64_t mine = CoopGame::SeedHash();
    if (theirs == mine) {
        sSeedWarned.erase(origin);
        return;
    }
    auto it = sSeedWarned.find(origin);
    if (it != sSeedWarned.end() && it->second == theirs) {
        return; // already said
    }
    sSeedWarned[origin] = theirs;
    std::string name = CoopGame::PlayerName(origin);
    SPDLOG_WARN("[Coop] Seed mismatch with {} (ours {:#x}, theirs {:#x})", name, mine, theirs);
    Notification::Emit({
        .prefix = name,
        .message = "is on another seed!",
        .suffix = "Continuing will break things",
    });
}

void OnCheckStatusEvent(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint8_t tag = 0;
    if (!r.U8(tag)) {
        return;
    }
    if (tag == kTagCheckStatus) {
        ApplyCheckStatus(r);
    } else if (tag == kTagSeed) {
        CheckSeed(e.origin, r);
    }
}

void Register() {
    // Anchor HookHandlers.cpp (it skips them while applying the team state)
    COND_HOOK(OnRandoSetCheckStatus, true,
              [](RandomizerCheck rc, RandomizerCheckStatus status) { SendCheckStatus(rc); });
    COND_HOOK(OnRandoSetIsSkipped, true, [](RandomizerCheck rc, bool isSkipped) { SendCheckStatus(rc); });
    COND_HOOK(OnRandoEntranceDiscovered, true,
              [](u16 entranceIndex, u8 isReversedEntrance) { SendEntranceDiscovered(entranceIndex); });
    CoopGame::OnEvent(Coop::EventKind::CheckStatus, "CoopRando.CheckStatus", OnCheckStatusEvent);
    CoopGame::OnEvent(Coop::EventKind::EntranceDiscovered, "CoopRando.Entrance", OnEntranceDiscovered);
    CoopGame::OnEveryFrame("CoopRando.Seed", AnnounceSeed);
}

} // namespace

namespace CoopGame {

// Anchor compares Rando::Context::GetSeed() (UpdateClientState "seed"). That's
// only the hash of the seed text though, so we also mix in the 5 hash icons:
// they come from seed + settings (playthrough.cpp), and are saved in the file.
uint64_t SeedHash() {
    if (!IS_RANDO) {
        return 0;
    }
    auto ctx = Rando::Context::GetInstance();
    uint64_t h = 14695981039346656037ULL; // FNV-1a, same result on every PC
    auto mix = [&h](uint8_t b) {
        h ^= b;
        h *= 1099511628211ULL;
    };
    uint32_t seed = ctx->GetSeed();
    for (int i = 0; i < 4; i++) {
        mix(static_cast<uint8_t>(seed >> (8 * i)));
    }
    for (uint8_t icon : ctx->hashIconIndexes) {
        mix(icon);
    }
    return h == 0 ? 1 : h; // 0 means vanilla
}

// Join snapshot, randomizer part (used by CoopItems.cpp BuildSaveBlob /
// ApplySaveBlob). Copied from Anchor UpdateTeamState: status + skipped of every check.
std::vector<uint8_t> BuildRandoSnapshot() {
    if (!IS_RANDO) {
        return {};
    }
    auto ctx = Rando::Context::GetInstance();
    Coop::ByteWriter w;
    w.U16(static_cast<uint16_t>(RC_MAX));
    for (int i = 0; i < RC_MAX; i++) {
        Rando::ItemLocation* loc = ctx->GetItemLocation(i);
        w.U8(static_cast<uint8_t>(loc->GetCheckStatus()));
        w.U8(loc->GetIsSkipped() ? 1 : 0);
    }
    return w.Take();
}

bool RandoSnapshotValid(std::span<const uint8_t> data) {
    Coop::ByteReader r(data);
    uint16_t count = 0;
    if (!r.U16(count) || count != RC_MAX || r.Remaining() != static_cast<size_t>(count) * 2) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        uint8_t status = 0;
        uint8_t skipped = 0;
        r.U8(status);
        r.U8(skipped);
        if (status > RCSHOW_SAVED || skipped > 1) {
            return false;
        }
    }
    return true;
}

void ApplyRandoSnapshot(std::span<const uint8_t> data) {
    if (!IS_RANDO || !RandoSnapshotValid(data)) {
        return;
    }
    auto ctx = Rando::Context::GetInstance();
    Coop::ByteReader r(data);
    uint16_t count = 0;
    r.U16(count);
    sApplyingRemote = true;
    for (int i = 0; i < count; i++) {
        uint8_t status = 0;
        uint8_t skipped = 0;
        r.U8(status);
        r.U8(skipped);
        Rando::ItemLocation* loc = ctx->GetItemLocation(i);
        // Anchor sets them all, we skip the ones already right (same result,
        // without firing thousands of hooks)
        if (loc->GetCheckStatus() != static_cast<RandomizerCheckStatus>(status)) {
            loc->SetCheckStatus(static_cast<RandomizerCheckStatus>(status));
        }
        if (loc->GetIsSkipped() != (skipped != 0)) {
            loc->SetIsSkipped(skipped != 0);
        }
    }
    // not in Anchor's team state, but the tracker should show the new totals
    CheckTracker::RecalculateAllAreaTotals();
    CheckTracker::RecalculateAvailableChecks();
    sApplyingRemote = false;
}

} // namespace CoopGame

static RegisterShipInitFunc sInit(Register);
