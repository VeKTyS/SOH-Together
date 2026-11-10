// Flags = the little on/off bits the game uses to remember stuff: chest
// opened, door unlocked, room cleared, rupee picked up, story events...
//
// - When a client loads their save, they ask the host for its whole save state
//   (flags + inventory, Anchor's "team state") and merge it in, so they end up
//   in the host's world. The "Resync" button asks for it again.
// - After that, every flag that goes up OR down is sent to the others.
// - Gold skulltula flags don't always fire a hook, so we watch them every frame.
//
// Some flags are left alone on purpose (see ShouldSync). Actors already on
// screen don't always notice a flag changed until you re-enter the room,
// CoopWorld.cpp pokes the common ones.

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Notification/Notification.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "soh/Enhancements/randomizer/ShuffleTradeItems.h"
extern PlayState* gPlayState;
}

// libultra's r4300.h has a U32(x) macro (pulled in by the headers above), it breaks ByteWriter::U32
#undef U32

namespace {

using RawAction = GameInteractor::RawAction;

// Client side: did we get the host's flags since we loaded our save?
bool sSnapshotApplied = false;
std::chrono::steady_clock::time_point sLastRequest;
// Host side: people who asked for our flags while we weren't in game yet.
std::vector<Coop::PeerId> sPendingRequests;

// Gold skulltula flags as we last saw / sent them
bool sGsWatched = false;
s32 sLastGsFlags[ARRAY_COUNT(gSaveContext.gsFlags)];

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
        case FLAG_EVENT_INF: // Anchor sends every OnFlagSet, these included
            return true;
        default:
            // GS tokens: watched every frame instead (see WatchGsFlags)
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
            case FLAG_EVENT_INF:
                return index < ARRAY_COUNT(gSaveContext.eventInf);
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

// Copied from Anchor Packets/SetFlag.cpp: if King Zora moved, and the player
// has Ruto's Letter, convert it to an empty bottle
void AfterGlobalFlagSet(int16_t type, int16_t flag) {
    if (type == FLAG_EVENT_CHECK_INF && flag == EVENTCHKINF_KING_ZORA_MOVED &&
        Inventory_HasSpecificBottle(ITEM_LETTER_RUTO)) {
        Inventory_ReplaceItem(gPlayState, ITEM_LETTER_RUTO, ITEM_BOTTLE);
    }
}

// Copied from Anchor Packets/UnsetFlag.cpp: an adult trade item flag went down
// (someone traded it away), swap it for the next one in the chain
void AfterGlobalFlagUnset(int16_t type, int16_t flag) {
    if (type != FLAG_RANDOMIZER_INF ||
        !(flag >= RAND_INF_ADULT_TRADES_HAS_POCKET_EGG && flag <= RAND_INF_ADULT_TRADES_HAS_CLAIM_CHECK)) {
        return;
    }
    u16 itemToReplace = ITEM_POCKET_EGG;
    switch (flag) {
        case RAND_INF_ADULT_TRADES_HAS_POCKET_EGG:
            itemToReplace = ITEM_POCKET_EGG;
            break;
        case RAND_INF_ADULT_TRADES_HAS_POCKET_CUCCO:
            itemToReplace = ITEM_POCKET_CUCCO;
            break;
        case RAND_INF_ADULT_TRADES_HAS_COJIRO:
            itemToReplace = ITEM_COJIRO;
            break;
        case RAND_INF_ADULT_TRADES_HAS_ODD_MUSHROOM:
            itemToReplace = ITEM_ODD_MUSHROOM;
            break;
        case RAND_INF_ADULT_TRADES_HAS_ODD_POTION:
            itemToReplace = ITEM_ODD_POTION;
            break;
        case RAND_INF_ADULT_TRADES_HAS_SAW:
            itemToReplace = ITEM_SAW;
            break;
        case RAND_INF_ADULT_TRADES_HAS_SWORD_BROKEN:
            itemToReplace = ITEM_SWORD_BROKEN;
            break;
        case RAND_INF_ADULT_TRADES_HAS_PRESCRIPTION:
            itemToReplace = ITEM_PRESCRIPTION;
            break;
        case RAND_INF_ADULT_TRADES_HAS_FROG:
            itemToReplace = ITEM_FROG;
            break;
        case RAND_INF_ADULT_TRADES_HAS_EYEDROPS:
            itemToReplace = ITEM_EYEDROPS;
            break;
        case RAND_INF_ADULT_TRADES_HAS_CLAIM_CHECK:
            itemToReplace = ITEM_CLAIM_CHECK;
            break;
    }
    Inventory_ReplaceItem(gPlayState, itemToReplace, Randomizer_GetNextAdultTradeItem());
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
        if (!c.set && c.flagType == FLAG_RANDOMIZER_INF && !IS_RANDO) {
            return; // RawAction::UnsetFlag asserts on that outside the randomizer
        }
        c.set ? RawAction::SetFlag(c.flagType, c.flag) : RawAction::UnsetFlag(c.flagType, c.flag);
        if (c.set) {
            AfterGlobalFlagSet(c.flagType, c.flag);
        } else {
            AfterGlobalFlagUnset(c.flagType, c.flag);
        }
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
    s.saveBlob = CoopGame::BuildSaveBlob(); // inventory, hearts, magic (CoopItems.cpp)
    return s;
}

template <typename T, size_t N> bool SameSize(const std::vector<uint16_t>& v, const T (&)[N]) {
    return v.size() == N;
}

bool ApplySnapshot(const Coop::FlagSnapshot& s) {
    if (s.scenes.size() != ARRAY_COUNT(gSaveContext.sceneFlags) || !SameSize(s.eventChkInf, gSaveContext.eventChkInf) ||
        !SameSize(s.itemGetInf, gSaveContext.itemGetInf) || !SameSize(s.infTable, gSaveContext.infTable) ||
        !SameSize(s.randomizerInf, gSaveContext.ship.randomizerInf) || !CoopGame::ApplySaveBlob(s.saveBlob)) {
        SPDLOG_WARN("[Coop] Host's flags don't fit our save layout, ignored");
        return false;
    }
    // ApplySaveBlob (CoopItems.cpp) did the hearts / magic / inventory / rupees /
    // gold skulltulas / rando checks part. The flags part is below.
    // Copied from Anchor Packets/UpdateTeamState.cpp HandlePacket_UpdateTeamState.
    for (size_t i = 0; i < s.scenes.size(); i++) {
        SavedSceneFlags& f = gSaveContext.sceneFlags[i];
        const auto& in = s.scenes[i];
        bool current = gPlayState != nullptr && gPlayState->sceneNum == static_cast<s16>(i);
        // Anchor reads our bits from the save copy, but the scene we stand in
        // keeps its live switches in actorCtx (the save copy can be old)
        u32 localSwch = current ? gPlayState->actorCtx.flags.swch : f.swch;
        u32 swch = in[1];
        u32 keepMask = 0;
        if (i == SCENE_WATER_TEMPLE) {
            // Keep water temple water level flags
            keepMask = (1 << 0x1C) | (1 << 0x1D) | (1 << 0x1E);
        } else if (i == SCENE_FOREST_TEMPLE) {
            // Keep forest temple elevator flag
            keepMask = (1 << 0x1B);
        } else if (i == SCENE_GANONS_TOWER_COLLAPSE_EXTERIOR) {
            // Keep collapse timer flag
            keepMask = (1 << 0x17);
        }
        swch = (swch & ~keepMask) | (localSwch & keepMask);

        f.chest = in[0];
        f.swch = swch;
        f.clear = in[2];
        f.collect = in[3];
        // Anchor's json doesn't carry these 3 so it ends up zeroing them (wipes
        // the visited rooms on the map). We merge them instead: map knowledge adds up.
        f.unk |= in[4];
        f.rooms |= in[5];
        f.floors |= in[6];
        if (current) {
            gPlayState->actorCtx.flags.chest = f.chest;
            gPlayState->actorCtx.flags.swch = f.swch;
            gPlayState->actorCtx.flags.clear = f.clear;
            gPlayState->actorCtx.flags.collect = f.collect;
        }
    }

    // Story flags are merged (|=), not overwritten: what we did stays done
    for (size_t i = 0; i < s.eventChkInf.size(); i++) {
        gSaveContext.eventChkInf[i] |= s.eventChkInf[i];
    }
    for (size_t i = 0; i < s.itemGetInf.size(); i++) {
        gSaveContext.itemGetInf[i] |= s.itemGetInf[i];
    }
    // Skip last row of infTable, don't want to sync swordless flag
    for (size_t i = 0; i + 1 < s.infTable.size(); i++) {
        gSaveContext.infTable[i] |= s.infTable[i];
    }
    for (size_t i = 0; i < s.randomizerInf.size(); i++) {
        gSaveContext.ship.randomizerInf[i] |= s.randomizerInf[i];
    }

    // Ensure ganon barrier state matches trials
    if (gSaveContext.eventChkInf[10] & 0x2000 && gSaveContext.eventChkInf[11] & 0xFC00) {
        gSaveContext.eventChkInf[12] |= 0x8;
    }

    // the skulltula flags we just got are the host's, don't send them back
    memcpy(sLastGsFlags, gSaveContext.gsFlags, sizeof(sLastGsFlags));
    sGsWatched = true;

    Notification::Emit({
        .message = "Save updated from team",
    });
    return true;
}

// --- gold skulltulas ---
// EnSi fires OnFlagSet(FLAG_GS_TOKEN) in vanilla, but the randomizer gives
// tokens other ways. So like the wallet: compare with last frame, send the
// whole array when it changed, the others OR it in (Anchor's SET_FLAG does
// SET_GS_FLAGS, same thing).
void WatchGsFlags() {
    if (!sGsWatched) {
        memcpy(sLastGsFlags, gSaveContext.gsFlags, sizeof(sLastGsFlags));
        sGsWatched = true;
        return;
    }
    if (memcmp(sLastGsFlags, gSaveContext.gsFlags, sizeof(sLastGsFlags)) == 0) {
        return;
    }
    memcpy(sLastGsFlags, gSaveContext.gsFlags, sizeof(sLastGsFlags));
    Coop::ByteWriter w;
    for (s32 v : sLastGsFlags) {
        w.U32(static_cast<uint32_t>(v));
    }
    CoopGame::SendEvent(Coop::EventKind::GsFlags, w.Take());
    SPDLOG_INFO("[Coop] Gold skulltula flags sent");
}

void OnGsFlags(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint32_t in[ARRAY_COUNT(gSaveContext.gsFlags)];
    for (uint32_t& v : in) {
        if (!r.U32(v)) {
            return;
        }
    }
    bool hosting = CoopGame::Session().Role() == Coop::SessionRole::Hosting;
    if (r.Remaining() != 0 || !CoopGame::LocalState().inGame || (!hosting && !sSnapshotApplied)) {
        return; // before the snapshot: it's in there already
    }
    for (size_t i = 0; i < ARRAY_COUNT(gSaveContext.gsFlags); i++) {
        gSaveContext.gsFlags[i] |= static_cast<s32>(in[i]);
        if (sGsWatched) {
            sLastGsFlags[i] |= static_cast<s32>(in[i]); // theirs, not to send back
        }
    }
}

// Host: send our whole state to whoever asked. Runs at the end of the frame
// (OnEveryFrame), after this frame's flags / items / events from the others
// are applied: they were already relayed before this snapshot, so the client
// drops them and must find them inside.
void SendPendingSnapshots() {
    Coop::Session& session = CoopGame::Session();
    if (session.Role() != Coop::SessionRole::Hosting || !CoopGame::LocalState().inGame ||
        sPendingRequests.empty()) {
        return;
    }
    Coop::FlagSnapshot snapshot = BuildSnapshot();
    for (Coop::PeerId p : sPendingRequests) {
        session.SendFlagSnapshot(p, snapshot);
    }
    SPDLOG_INFO("[Coop] Sent our flags to {} player(s)", sPendingRequests.size());
    sPendingRequests.clear();
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

// Anchor's "Request Team State" button: ask the host for its whole state again
// and merge it in, like when we joined. Only a client can ask (the host's
// world is the reference).
void RequestResync() {
    std::lock_guard lock(Mutex());
    Coop::Session& session = Session();
    if (session.Role() != Coop::SessionRole::Connected) {
        return;
    }
    session.TakeFlagSnapshot(); // an old answer still waiting would be stale
    sSnapshotApplied = false;   // UpdateFlags takes the next one (and asks again every 2 s)
    sGsWatched = false;
    sLastRequest = std::chrono::steady_clock::now();
    session.RequestFlagSnapshot();
    SPDLOG_INFO("[Coop] Asked the host for its save again");
}

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
        sGsWatched = false;
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
        // answered at the end of the frame, see SendPendingSnapshots
        for (Coop::PeerId p : session.TakeSnapshotRequests()) {
            sPendingRequests.push_back(p);
        }
    }

    if (client) {
        // extra answers (we ask every 2 s) are dropped once we have one
        std::optional<Coop::FlagSnapshot> snapshot = session.TakeFlagSnapshot();
        if (!sSnapshotApplied) {
            if (snapshot) {
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
    }

    // changes that arrived before the host's snapshot are already in it (same
    // ordered channel), so a client can safely drop them
    std::vector<Coop::FlagChange> changes = session.TakeFlagChanges();
    if (hosting || sSnapshotApplied) {
        for (const Coop::FlagChange& c : changes) {
            ApplyChange(c);
        }
        WatchGsFlags();
    } else {
        sGsWatched = false;
    }
}

void RegisterFlagHooks() {
    COND_HOOK(OnFlagSet, true, [](int16_t type, int16_t flag) { OnLocalFlag(-1, type, flag, true); });
    COND_HOOK(OnFlagUnset, true, [](int16_t type, int16_t flag) { OnLocalFlag(-1, type, flag, false); });
    COND_HOOK(OnSceneFlagSet, true,
              [](int16_t scene, int16_t type, int16_t flag) { OnLocalFlag(scene, type, flag, true); });
    COND_HOOK(OnSceneFlagUnset, true,
              [](int16_t scene, int16_t type, int16_t flag) { OnLocalFlag(scene, type, flag, false); });
    OnEvent(Coop::EventKind::GsFlags, "CoopFlags.GsFlags", OnGsFlags);
    OnEveryFrame("CoopFlags.Snapshots", SendPendingSnapshots);
}

} // namespace CoopGame
