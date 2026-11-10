// Flags = the little on/off bits the game uses to remember stuff: chest
// opened, door unlocked, room cleared, rupee picked up, story events...
//
// - When a client loads their save, they ask the host for all its flags and
//   take them (so they end up in the host's world).
// - After that, every flag that goes up OR down is sent to the others.
//
// Some flags are left alone on purpose (see ShouldSync). Things NOT handled yet:
// inventory (so the item from a chest someone else opened isn't yours), and
// actors already on screen don't always notice a flag changed until you re-enter
// the room (a chest opened by your friend stays closed for you until then).

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"

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

using RawAction = GameInteractor::RawAction;

// Client side: did we get the host's flags since we loaded our save?
bool sSnapshotApplied = false;
std::chrono::steady_clock::time_point sLastRequest;
// Host side: people who asked for our flags while we weren't in game yet.
std::vector<Coop::PeerId> sPendingRequests;

// Which flags we share. Copied the exceptions from Anchor, they found these the hard way.
bool ShouldSync(int16_t scene, int16_t type, int16_t flag) {
    switch (type) {
        case FLAG_SCENE_SWITCH:
        case FLAG_SCENE_TREASURE:
        case FLAG_SCENE_CLEAR:
        case FLAG_SCENE_COLLECTIBLE:
            break;
        case FLAG_EVENT_CHECK_INF:
        case FLAG_ITEM_GET_INF:
        case FLAG_INF_TABLE:
        case FLAG_RANDOMIZER_INF:
            return true;
        default:
            // EVENT_INF is temporary stuff (minigames, timers), GS tokens also
            // need the skulltula count to be shared: later.
            return false;
    }
    // collectible flag 0 means "no flag" (hearts/rupees from grass and pots),
    // but the game still fires the hook for it on every pickup. Never share it,
    // or picking up one heart would delete every loose drop at your friend's.
    if (type == FLAG_SCENE_COLLECTIBLE && flag == 0) {
        return false;
    }
    if (type == FLAG_SCENE_SWITCH) {
        // water level: everyone keeps their own, or you'd drown your friend
        if (scene == SCENE_WATER_TEMPLE && flag >= 0x1C && flag <= 0x1E) {
            return false;
        }
        if (scene == SCENE_FOREST_TEMPLE && flag == 0x1B) { // elevator
            return false;
        }
        if (scene == SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR && flag == 0x36) { // escape timer
            return false;
        }
        if ((scene == SCENE_GREAT_FAIRYS_FOUNTAIN_MAGIC || scene == SCENE_GREAT_FAIRYS_FOUNTAIN_SPELLS) &&
            flag == 0x38) { // great fairy cutscene
            return false;
        }
    }
    return true;
}

// Flags from the network are used as array indexes by the game, check them first.
bool IsValid(const Coop::FlagChange& c) {
    if (c.flag < 0) {
        return false;
    }
    if (c.sceneNum == -1) {
        size_t index = c.flag >> 4;
        switch (c.flagType) {
            case FLAG_EVENT_CHECK_INF:
                return index < ARRAY_COUNT(gSaveContext.eventChkInf);
            case FLAG_ITEM_GET_INF:
                return index < ARRAY_COUNT(gSaveContext.itemGetInf);
            case FLAG_INF_TABLE:
                return index < ARRAY_COUNT(gSaveContext.infTable);
            case FLAG_RANDOMIZER_INF:
                return index < ARRAY_COUNT(gSaveContext.ship.randomizerInf);
            default:
                return false;
        }
    }
    if (c.sceneNum < 0 || c.sceneNum >= SCENE_ID_MAX) {
        return false;
    }
    switch (c.flagType) {
        case FLAG_SCENE_SWITCH:
        case FLAG_SCENE_COLLECTIBLE:
            return c.flag < 0x40; // 0x20+ are the "temporary" ones
        case FLAG_SCENE_TREASURE:
        case FLAG_SCENE_CLEAR:
            return c.flag < 0x20;
        default:
            return false;
    }
}

// Two kinds of items on the floor (EnItem00):
// - placed in the level (heart piece, small key, fixed rupees): shared, if
//   one player takes it it's gone for everybody
// - dropped by a pot / bush / crate / enemy: everyone gets their own, nobody
//   can steal the other one's heart
// The game tells them apart with unk_15A: dropped items have a despawn timer
// (>= 0), placed ones sit at -1/-2. The pickup code calls Flags_SetCollectible
// before touching that timer, so it's still right when our hook runs.
bool IsDroppedItem(const EnItem00* item) {
    return item->unk_15A >= 0;
}

template <typename Fn> void ForEachItemWithFlag(int16_t flag, Fn&& fn) {
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_MISC].head; a != nullptr; a = a->next) {
        if (a->id == ACTOR_EN_ITEM00 && ((EnItem00*)a)->collectibleFlag == flag) {
            fn((EnItem00*)a);
        }
    }
}

// We just picked up something with this flag: was it someone's personal drop?
bool PickedUpDroppedItem(int16_t scene, int16_t flag) {
    if (gPlayState == nullptr || scene != gPlayState->sceneNum) {
        return false;
    }
    bool dropped = false;
    bool placed = false;
    ForEachItemWithFlag(flag, [&](EnItem00* item) { (IsDroppedItem(item) ? dropped : placed) = true; });
    return dropped && !placed;
}

// Someone picked up a placed item that's lying in front of us: make it vanish.
// Dropped ones stay, those are ours.
void RemoveCollectible(int16_t flag) {
    ForEachItemWithFlag(flag, [](EnItem00* item) {
        if (!IsDroppedItem(item)) {
            Actor_Kill(&item->actor);
        }
    });
}

void ApplyChange(const Coop::FlagChange& c) {
    if (!IsValid(c) || !ShouldSync(c.sceneNum, c.flagType, c.flag)) {
        SPDLOG_WARN("[Coop] Ignored bad flag from network: scene {} type {} flag {:#x}", c.sceneNum, c.flagType,
                    c.flag);
        return;
    }
    // RawAction writes the bits directly, it does NOT fire the flag hooks, so
    // this won't bounce back to the network.
    if (c.sceneNum == -1) {
        c.set ? RawAction::SetFlag(c.flagType, c.flag) : RawAction::UnsetFlag(c.flagType, c.flag);
    } else {
        c.set ? RawAction::SetSceneFlag(c.sceneNum, c.flagType, c.flag)
              : RawAction::UnsetSceneFlag(c.sceneNum, c.flagType, c.flag);
        if (c.set && c.flagType == FLAG_SCENE_COLLECTIBLE && c.sceneNum == gPlayState->sceneNum) {
            RemoveCollectible(c.flag);
        }
    }
    SPDLOG_INFO("[Coop] Flag from network: scene {} type {} flag {:#x} -> {}", c.sceneNum, c.flagType, c.flag,
                c.set ? "on" : "off");
}

Coop::FlagSnapshot BuildSnapshot() {
    Coop::FlagSnapshot s;
    s.scenes.resize(ARRAY_COUNT(gSaveContext.sceneFlags));
    for (size_t i = 0; i < s.scenes.size(); i++) {
        const SavedSceneFlags& f = gSaveContext.sceneFlags[i];
        s.scenes[i] = { f.chest, f.swch, f.clear, f.collect, f.unk, f.rooms, f.floors };
    }
    // careful: the scene we're standing in keeps its flags in a working copy
    // and only writes them back to the save when we leave, so read that one
    if (gPlayState != nullptr && gPlayState->sceneNum >= 0 && (size_t)gPlayState->sceneNum < s.scenes.size()) {
        auto& cur = s.scenes[gPlayState->sceneNum];
        cur[0] = gPlayState->actorCtx.flags.chest;
        cur[1] = gPlayState->actorCtx.flags.swch;
        cur[2] = gPlayState->actorCtx.flags.clear;
        cur[3] = gPlayState->actorCtx.flags.collect;
    }
    s.eventChkInf.assign(std::begin(gSaveContext.eventChkInf), std::end(gSaveContext.eventChkInf));
    s.itemGetInf.assign(std::begin(gSaveContext.itemGetInf), std::end(gSaveContext.itemGetInf));
    s.infTable.assign(std::begin(gSaveContext.infTable), std::end(gSaveContext.infTable));
    s.randomizerInf.assign(std::begin(gSaveContext.ship.randomizerInf), std::end(gSaveContext.ship.randomizerInf));
    return s;
}

template <typename T, size_t N> bool SameSize(const std::vector<uint16_t>& v, const T (&)[N]) {
    return v.size() == N;
}

bool ApplySnapshot(const Coop::FlagSnapshot& s) {
    if (s.scenes.size() != ARRAY_COUNT(gSaveContext.sceneFlags) || !SameSize(s.eventChkInf, gSaveContext.eventChkInf) ||
        !SameSize(s.itemGetInf, gSaveContext.itemGetInf) || !SameSize(s.infTable, gSaveContext.infTable) ||
        !SameSize(s.randomizerInf, gSaveContext.ship.randomizerInf)) {
        SPDLOG_WARN("[Coop] Host's flags don't fit our save layout, ignored");
        return false;
    }
    for (size_t i = 0; i < s.scenes.size(); i++) {
        SavedSceneFlags& f = gSaveContext.sceneFlags[i];
        const auto& in = s.scenes[i];
        f.chest = in[0];
        f.swch = in[1];
        f.clear = in[2];
        f.collect = in[3];
        f.unk = in[4];
        f.rooms = in[5];
        f.floors = in[6];
    }
    if (gPlayState != nullptr && gPlayState->sceneNum >= 0 && (size_t)gPlayState->sceneNum < s.scenes.size()) {
        const auto& cur = s.scenes[gPlayState->sceneNum];
        gPlayState->actorCtx.flags.chest = cur[0];
        gPlayState->actorCtx.flags.swch = cur[1];
        gPlayState->actorCtx.flags.clear = cur[2];
        gPlayState->actorCtx.flags.collect = cur[3];
    }
    std::copy(s.eventChkInf.begin(), s.eventChkInf.end(), gSaveContext.eventChkInf);
    std::copy(s.itemGetInf.begin(), s.itemGetInf.end(), gSaveContext.itemGetInf);
    std::copy(s.infTable.begin(), s.infTable.end(), gSaveContext.infTable);
    std::copy(s.randomizerInf.begin(), s.randomizerInf.end(), gSaveContext.ship.randomizerInf);
    return true;
}

// Our own game changed a flag.
void OnLocalFlag(int16_t scene, int16_t type, int16_t flag, bool set) {
    std::lock_guard lock(CoopGame::Mutex());
    Coop::Session& session = CoopGame::Session();
    if (session.Role() != Coop::SessionRole::Hosting && session.Role() != Coop::SessionRole::Connected) {
        return;
    }
    if (!ShouldSync(scene, type, flag)) {
        return;
    }
    if (type == FLAG_SCENE_COLLECTIBLE && set && PickedUpDroppedItem(scene, flag)) {
        SPDLOG_INFO("[Coop] Picked up our own drop (flag {:#x}), not shared", flag);
        return;
    }
    session.SendFlagChange({ scene, static_cast<uint8_t>(type), flag, set });
    SPDLOG_INFO("[Coop] Flag sent: scene {} type {} flag {:#x} -> {}", scene, type, flag, set ? "on" : "off");
}

} // namespace

namespace CoopGame {

bool FlagsSynced() {
    return sSnapshotApplied;
}

void UpdateFlags() {
    Coop::Session& session = Session();
    bool inGame = LocalState().inGame;
    bool hosting = session.Role() == Coop::SessionRole::Hosting;
    bool client = session.Role() == Coop::SessionRole::Connected;

    if (!inGame || (!hosting && !client)) {
        // flags only mean something once a save is loaded; we'll ask again then
        sSnapshotApplied = false;
        session.TakeFlagChanges();
        session.TakeFlagSnapshot();
        if (hosting) {
            for (Coop::PeerId p : session.TakeSnapshotRequests()) {
                sPendingRequests.push_back(p);
            }
        } else {
            sPendingRequests.clear();
        }
        return;
    }

    if (hosting) {
        for (Coop::PeerId p : session.TakeSnapshotRequests()) {
            sPendingRequests.push_back(p);
        }
        if (!sPendingRequests.empty()) {
            Coop::FlagSnapshot snapshot = BuildSnapshot();
            for (Coop::PeerId p : sPendingRequests) {
                session.SendFlagSnapshot(p, snapshot);
            }
            SPDLOG_INFO("[Coop] Sent our flags to {} player(s)", sPendingRequests.size());
            sPendingRequests.clear();
        }
    }

    if (client && !sSnapshotApplied) {
        if (std::optional<Coop::FlagSnapshot> snapshot = session.TakeFlagSnapshot()) {
            sSnapshotApplied = ApplySnapshot(*snapshot);
            if (sSnapshotApplied) {
                SPDLOG_INFO("[Coop] Got the host's flags, we're in their world now");
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (!sSnapshotApplied && now - sLastRequest > std::chrono::seconds(2)) {
            session.RequestFlagSnapshot(); // ask again every 2 s until the host (in game) answers
            sLastRequest = now;
        }
    }

    // changes that arrived before the host's snapshot are already in it (same
    // ordered channel), so a client can safely drop them
    std::vector<Coop::FlagChange> changes = session.TakeFlagChanges();
    if (hosting || sSnapshotApplied) {
        for (const Coop::FlagChange& c : changes) {
            ApplyChange(c);
        }
    }
}

void RegisterFlagHooks() {
    COND_HOOK(OnFlagSet, true, [](int16_t type, int16_t flag) { OnLocalFlag(-1, type, flag, true); });
    COND_HOOK(OnFlagUnset, true, [](int16_t type, int16_t flag) { OnLocalFlag(-1, type, flag, false); });
    COND_HOOK(OnSceneFlagSet, true,
              [](int16_t scene, int16_t type, int16_t flag) { OnLocalFlag(scene, type, flag, true); });
    COND_HOOK(OnSceneFlagUnset, true,
              [](int16_t scene, int16_t type, int16_t flag) { OnLocalFlag(scene, type, flag, false); });
}

} // namespace CoopGame
