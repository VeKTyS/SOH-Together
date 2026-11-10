// Shared items: what one player gets, everybody gets.
//
// - an item received (chest, NPC, heart piece...) is sent to the others, who
//   get it the same way (Item_Give / Randomizer_Item_Give), with a small
//   "X found Y" notification
// - dungeon keys/map/compass: we send the dungeon's current values, because
//   keys also go down when someone opens a door
// - ice traps are shared too: everybody gets frozen (Anchor does that)
// - rupees are NOT items here: one shared wallet, every +/- is sent as a delta
//   (TrueCoop RupeeChange), so a picked up rupee only counts once
// - magic beans count is kept the same for everyone (Anchor UpdateBeansCount)
// - when a client loads their save, they get the host's inventory, hearts,
//   magic, rupees... together with the host's flags (see CoopFlags.cpp), merged
//   the way Anchor's UPDATE_TEAM_STATE does it
//
// Built the same way as Anchor's GIVE_ITEM / UPDATE_DUNGEON_ITEMS packets,
// they already found the special cases (heart pieces, BGS, great fairies).

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/item-tables/ItemTableManager.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Notification/Notification.h"
#include "soh/SohGui/ImGuiUtils.h"
#include "soh/util.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <span>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// libultra's r4300.h has a U32(x) macro (pulled in by the headers above), it breaks ByteWriter::U32
#undef U32

namespace {

// true while we give an item because of the network, so it doesn't bounce back
bool sApplyingRemote = false;

// Ice traps we got from teammates and haven't triggered yet. When they go off,
// the game fires OnItemReceive(ice trap) again: that one isn't ours to send.
// Copied from Anchor Packets/GiveItem.cpp (incomingIceTrapsFromAnchor).
uint8_t sIncomingIceTraps = 0;

// --- shared wallet + beans, polled every frame ---
// 9.3.1 has no OnRupeeChange hook (TrueCoop added one). But Rupees_ChangeBy
// (z_parameter.c), where every rupee change goes through, also bumps the
// "rupees collected" / "rupees spent" stats. So the change of those two
// counters since last frame = exactly the sum of the Rupees_ChangeBy calls,
// same thing TrueCoop's hook sees.
bool sWatching = false; // baselines below are valid
uint32_t sRupeesCollected = 0;
uint32_t sRupeesSpent = 0;
s8 sBeans = 0;
s8 sBeansBought = 0;
// frame of our last RupeeChange, for TrueCoop's drift correction
uint32_t sLastLocalRupeeSendFrame = 0;

// TrueCoop Packets/RupeeChange.cpp: frames of local silence before we let a
// teammate's total overwrite ours (~0.67s at 60fps)
constexpr uint32_t kRupeeSnapQuietFrames = 40;

bool CanSend() {
    Coop::SessionRole role = CoopGame::Session().Role();
    return !sApplyingRemote && CoopGame::LocalState().inGame && gPlayState != nullptr &&
           (role == Coop::SessionRole::Hosting || role == Coop::SessionRole::Connected);
}

// Same rule as items/flags: a client ignores everything until it has the
// host's snapshot (it's all in there anyway).
bool CanApply() {
    return CoopGame::LocalState().inGame && gPlayState != nullptr &&
           (CoopGame::Session().Role() == Coop::SessionRole::Hosting || CoopGame::FlagsSynced());
}

// TrueCoop HookHandlers.cpp / Packets/GiveItem.cpp: rupees go through the
// wallet (RupeeChange), never through item sharing, or they'd count twice.
bool IsRupeeItem(const GetItemEntry& e) {
    return (e.modIndex == MOD_NONE && e.itemId >= ITEM_RUPEE_GREEN && e.itemId <= ITEM_RUPEE_GOLD) ||
           (e.modIndex == MOD_RANDOMIZER && e.getItemId >= RG_GREEN_RUPEE && e.getItemId <= RG_HUGE_RUPEE);
}

// "What we have right now is the starting point": changes before this aren't
// ours to send (or came from a teammate).
void Rebaseline() {
    sRupeesCollected = gSaveContext.ship.stats.count[COUNT_RUPEES_COLLECTED];
    sRupeesSpent = gSaveContext.ship.stats.count[COUNT_RUPEES_SPENT];
    sBeans = AMMO(ITEM_BEAN);
    sBeansBought = BEANS_BOUGHT;
    sWatching = true;
}

void SendItem(uint16_t modId, int16_t getItemId) {
    if (sApplyingRemote) {
        return; // Item_Give fires OnItemReceive too: that's the item we just got from the network
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!CanSend()) {
        return;
    }
    // Anchor Packets/GiveItem.cpp: an ice trap we got from a teammate just went off, don't send it back
    if (modId == MOD_RANDOMIZER && getItemId == RG_ICE_TRAP && sIncomingIceTraps > 0) {
        sIncomingIceTraps--;
        return;
    }
    // Anchor: the master sword in the last Ganon fight is part of the fight, not an item
    if (modId == MOD_RANDOMIZER && getItemId == RG_MASTER_SWORD && gPlayState->sceneNum == SCENE_GANON_BOSS) {
        return;
    }
    CoopGame::Session().SendItemGiven({ modId, getItemId });
    SPDLOG_INFO("[Coop] Item sent: mod {} id {:#x}", modId, getItemId);
}

void SendDungeon(uint16_t mapIndex) {
    if (mapIndex >= ARRAY_COUNT(gSaveContext.inventory.dungeonKeys)) {
        return;
    }
    if (sApplyingRemote) {
        return;
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!CanSend()) {
        return;
    }
    CoopGame::Session().SendDungeonItems({ mapIndex, gSaveContext.inventory.dungeonItems[mapIndex],
                                           static_cast<int8_t>(gSaveContext.inventory.dungeonKeys[mapIndex]) });
}

// Anchor HookHandlers.cpp OnItemReceive + TrueCoop's rupee filter
void OnItemReceive(GetItemEntry item) {
    // keys/map/compass: send the whole dungeon state instead
    if (item.modIndex == MOD_NONE && item.itemId >= ITEM_KEY_BOSS && item.itemId <= ITEM_KEY_SMALL) {
        SendDungeon(gSaveContext.mapIndex);
        return;
    }
    if (IsRupeeItem(item)) {
        return; // the wallet sends it (RupeeChange)
    }
    SendItem(item.tableId, item.getItemId);
}

// Outside the randomizer, great fairy upgrades don't go through OnItemReceive,
// only their flag does: turn the flag into the item for the others.
void OnFlagSet(int16_t type, int16_t flag) {
    if (IS_RANDO || type != FLAG_RANDOMIZER_INF) {
        return;
    }
    switch (flag) {
        case RAND_INF_DMT_GREAT_FAIRY_REWARD:
            SendItem(MOD_RANDOMIZER, RG_MAGIC_SINGLE);
            break;
        case RAND_INF_DMC_GREAT_FAIRY_REWARD:
            SendItem(MOD_RANDOMIZER, RG_MAGIC_DOUBLE);
            break;
        case RAND_INF_OGC_GREAT_FAIRY_REWARD:
            SendItem(MOD_RANDOMIZER, RG_DOUBLE_DEFENSE);
            break;
        default:
            break;
    }
}

// Copied from Anchor Packets/GiveItem.cpp HandlePacket_GiveItem
void GiveItem(const std::string& from, const Coop::ItemGiven& in) {
    GetItemEntry entry;
    if (in.modId == MOD_NONE) {
        if (in.getItemId <= GI_NONE || in.getItemId >= GI_MAX) {
            return;
        }
        entry = ItemTableManager::Instance->RetrieveItemEntry(MOD_NONE, in.getItemId);
    } else if (in.modId == MOD_RANDOMIZER) {
        if (in.getItemId <= RG_NONE || in.getItemId >= RG_MAX) {
            return;
        }
        entry = Rando::StaticData::RetrieveItem(static_cast<RandomizerGet>(in.getItemId)).GetGIEntry_Copy();
    } else {
        return; // some other mod's table, we don't know it
    }

    // TrueCoop: a rupee here would count twice (the wallet already sent it)
    if (IsRupeeItem(entry)) {
        return;
    }

    sApplyingRemote = true;
    if (entry.modIndex == MOD_NONE) {
        if (entry.getItemId == GI_SWORD_BGS) {
            gSaveContext.bgsFlag = true;
        }
        Item_Give(gPlayState, static_cast<u8>(entry.itemId));
    } else if (entry.modIndex == MOD_RANDOMIZER) {
        if (entry.getItemId == RG_ICE_TRAP) {
            // the game freezes us at the next safe moment, like a trap from a chest
            gSaveContext.ship.pendingIceTrapCount++;
            if (sIncomingIceTraps < UINT8_MAX) {
                sIncomingIceTraps++;
            }
        } else {
            Randomizer_Item_Give(gPlayState, entry);
        }
    }
    sApplyingRemote = false;

    // heart container / piece: full heal, like when you get it yourself
    if (entry.gid == GID_HEART_CONTAINER || entry.gid == GID_HEART_PIECE) {
        gSaveContext.healthAccumulator = 0x140;
    }
    // 4 heart pieces make a heart (normally the message box code does that)
    s32 pieces = (s32)(gSaveContext.inventory.questItems & 0xF0000000) >> (QUEST_HEART_PIECE + 4);
    if (pieces >= 4) {
        gSaveContext.inventory.questItems &= ~0xF0000000;
        gSaveContext.inventory.questItems += (pieces % 4) << (QUEST_HEART_PIECE + 4);
        gSaveContext.healthCapacity += 0x10 * (pieces / 4);
        gSaveContext.health += 0x10 * (pieces / 4);
    }

    if (entry.getItemCategory != ITEM_CATEGORY_JUNK) {
        if (entry.modIndex == MOD_NONE) {
            Notification::Emit({
                .itemIcon = GetTextureForItemId(entry.itemId),
                .prefix = from,
                .message = "found",
                .suffix = SohUtils::GetItemName(entry.itemId),
            });
        } else {
            Notification::Emit({
                .prefix = from,
                .message = "found",
                .suffix = Rando::StaticData::RetrieveItem((RandomizerGet)entry.getItemId).GetName().english,
            });
        }
    }
    SPDLOG_INFO("[Coop] Item from {}: mod {} id {:#x}", from, in.modId, in.getItemId);
}

void ApplyDungeon(const Coop::DungeonItems& d) {
    if (d.mapIndex >= ARRAY_COUNT(gSaveContext.inventory.dungeonItems) ||
        d.mapIndex >= ARRAY_COUNT(gSaveContext.inventory.dungeonKeys)) {
        return;
    }
    gSaveContext.inventory.dungeonItems[d.mapIndex] = d.items;
    gSaveContext.inventory.dungeonKeys[d.mapIndex] = d.keys;
}

// --- shared wallet (TrueCoop Packets/RupeeChange.cpp) ---

// TrueCoop SendPacket_RupeeChange: the delta, plus our total so the others can fix drift
void SendRupeeChange(s32 delta) {
    if (delta == 0) {
        return;
    }
    delta = std::clamp<s32>(delta, INT16_MIN, INT16_MAX);
    s32 total = gSaveContext.rupees + gSaveContext.rupeeAccumulator;
    Coop::ByteWriter w;
    w.U16(static_cast<uint16_t>(static_cast<int16_t>(delta)));
    w.U32(static_cast<uint32_t>(total));
    sLastLocalRupeeSendFrame = gPlayState->gameplayFrames;
    SPDLOG_INFO("[Coop] Wallet tx delta={} total={}", delta, total);
    CoopGame::SendEvent(Coop::EventKind::RupeeChange, w.Take());
}

// TrueCoop HandlePacket_RupeeChange
void OnRupeeChange(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint16_t rawDelta = 0;
    uint32_t rawTotal = 0;
    if (!r.U16(rawDelta) || !r.U32(rawTotal) || r.Remaining() != 0) {
        return;
    }
    s32 delta = static_cast<int16_t>(rawDelta);
    s32 total = static_cast<int32_t>(rawTotal);
    if (total < 0 || total > INT16_MAX || !CanApply()) {
        return;
    }

    // snapshot the accumulator BEFORE applying: a snap is only safe when our
    // own wallet was idle (nothing pending to drain)
    s16 preAccumulator = gSaveContext.rupeeAccumulator;

    // keep the wallet between 0 and its size (the game would also stop there
    // while draining the accumulator, this just says it up front)
    s32 myTotal = gSaveContext.rupees + gSaveContext.rupeeAccumulator;
    s32 cap = CUR_CAPACITY(UPG_WALLET);
    if (delta > 0) {
        delta = std::min(delta, std::max<s32>(0, cap - myTotal));
    } else {
        delta = std::max(delta, -std::max<s32>(0, myTotal));
    }
    if (delta != 0) {
        Rupees_ChangeBy(static_cast<s16>(delta));
    }
    // that change was the teammate's, not ours: don't send it back
    if (sWatching) {
        sRupeesCollected = gSaveContext.ship.stats.count[COUNT_RUPEES_COLLECTED];
        sRupeesSpent = gSaveContext.ship.stats.count[COUNT_RUPEES_SPENT];
    }

    myTotal = gSaveContext.rupees + gSaveContext.rupeeAccumulator;
    s32 drift = total - myTotal;
    SPDLOG_INFO("[Coop] Wallet rx delta={} total={} (drift={})", delta, total, drift);

    // Drift correction: only when (a) there is drift, (b) we've been quiet
    // long enough that no in-flight local delta explains it, and (c) our
    // wallet was idle before. Nudging the accumulator lets the HUD tick to it.
    if (drift != 0 && preAccumulator == 0 &&
        (gPlayState->gameplayFrames - sLastLocalRupeeSendFrame) >= kRupeeSnapQuietFrames) {
        SPDLOG_INFO("[Coop] Wallet drift {} vs {} (snapping {})", myTotal, total, drift);
        s32 acc = std::clamp<s32>(gSaveContext.rupeeAccumulator + drift, INT16_MIN, INT16_MAX);
        gSaveContext.rupeeAccumulator = static_cast<s16>(acc);
    }
}

// --- magic beans (Anchor Packets/UpdateBeansCount.cpp) ---

void SendBeans() {
    Coop::ByteWriter w;
    w.U8(static_cast<uint8_t>(AMMO(ITEM_BEAN)));
    w.U8(static_cast<uint8_t>(BEANS_BOUGHT));
    CoopGame::SendEvent(Coop::EventKind::BeansCount, w.Take());
}

void OnBeansCount(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint8_t amount = 0;
    uint8_t bought = 0;
    if (!r.U8(amount) || !r.U8(bought) || r.Remaining() != 0 || amount > INT8_MAX || bought > INT8_MAX ||
        !CanApply()) {
        return;
    }
    AMMO(ITEM_BEAN) = static_cast<s8>(amount);
    BEANS_BOUGHT = static_cast<s8>(bought);
    if (sWatching) {
        sBeans = AMMO(ITEM_BEAN);
        sBeansBought = BEANS_BOUGHT;
    }
}

// Every frame: did OUR game change the wallet or the beans since last time?
void SendLocalChanges() {
    if (!sWatching) {
        Rebaseline();
        return;
    }
    uint32_t collected = gSaveContext.ship.stats.count[COUNT_RUPEES_COLLECTED];
    uint32_t spent = gSaveContext.ship.stats.count[COUNT_RUPEES_SPENT];
    s32 delta = static_cast<s32>(collected - sRupeesCollected) - static_cast<s32>(spent - sRupeesSpent);
    sRupeesCollected = collected;
    sRupeesSpent = spent;
    SendRupeeChange(delta);

    if (AMMO(ITEM_BEAN) != sBeans || BEANS_BOUGHT != sBeansBought) {
        sBeans = AMMO(ITEM_BEAN);
        sBeansBought = BEANS_BOUGHT;
        SendBeans();
    }
}

// What a client takes from the host when joining. Raw copy of these fields:
// both games are the exact same build (checked by the handshake). Same field
// list as Anchor's JsonConversions.hpp (SaveContext to_json), plus the rupees
// (TrueCoop). The randomizer check statuses follow it (CoopRando.cpp).
struct TeamSave {
    Inventory inventory;
    s32 gsFlags[ARRAY_COUNT(gSaveContext.gsFlags)];
    ShipQuestSaveContextData quest; // triforce pieces, bombchu bag level, silver rupees...
    uint64_t firstInput;
    uint64_t fileCreatedAt;
    s16 healthCapacity;
    s16 magicCapacity;
    s16 rupees;
    u16 swordHealth;
    s8 magicLevel;
    u8 isMagicAcquired;
    u8 isDoubleMagicAcquired;
    u8 isDoubleDefenseAcquired;
    u8 bgsFlag;
};

} // namespace

namespace CoopGame {

void UpdateItems() {
    Coop::Session& session = Session();
    std::vector<Coop::ItemGiven> items = session.TakeItemsGiven();
    std::vector<Coop::DungeonItems> dungeons = session.TakeDungeonItems();
    // flags-style: before the host's snapshot, it's all in the snapshot anyway
    if (!CanApply()) {
        sWatching = false; // start again from what we have once we're back
        return;
    }
    // our own wallet / beans changes first, before the teammates' stuff lands
    SendLocalChanges();

    // the host relays without saying who it came from: with one other player we know, else "Teammate"
    std::string from = session.Players().size() == 1 ? session.Players()[0].name : std::string("Teammate");
    for (const Coop::ItemGiven& item : items) {
        GiveItem(from, item);
    }
    for (const Coop::DungeonItems& d : dungeons) {
        ApplyDungeon(d);
    }
    // a teammate's item can move the rupees / beans (a bean bought, ...):
    // that's theirs, it doesn't go back out
    if (!items.empty()) {
        Rebaseline();
    }
}

std::vector<uint8_t> BuildSaveBlob() {
    TeamSave t;
    memset(&t, 0, sizeof(t));
    t.inventory = gSaveContext.inventory;
    memcpy(t.gsFlags, gSaveContext.gsFlags, sizeof(t.gsFlags));
    t.quest = gSaveContext.ship.quest;
    t.firstInput = gSaveContext.ship.stats.firstInput;
    t.fileCreatedAt = gSaveContext.ship.stats.fileCreatedAt;
    t.healthCapacity = gSaveContext.healthCapacity;
    t.magicCapacity = gSaveContext.magicCapacity;
    // TrueCoop: rupees still draining count too
    t.rupees = static_cast<s16>(gSaveContext.rupees + gSaveContext.rupeeAccumulator);
    t.swordHealth = gSaveContext.swordHealth;
    t.magicLevel = gSaveContext.magicLevel;
    t.isMagicAcquired = gSaveContext.isMagicAcquired;
    t.isDoubleMagicAcquired = gSaveContext.isDoubleMagicAcquired;
    t.isDoubleDefenseAcquired = gSaveContext.isDoubleDefenseAcquired;
    t.bgsFlag = gSaveContext.bgsFlag;
    std::vector<uint8_t> out(sizeof(t));
    memcpy(out.data(), &t, sizeof(t));
    std::vector<uint8_t> rando = BuildRandoSnapshot(); // empty outside the randomizer
    out.insert(out.end(), rando.begin(), rando.end());
    return out;
}

// Copied from Anchor Packets/UpdateTeamState.cpp HandlePacket_UpdateTeamState
// (the items / hearts / magic part, the flags are in CoopFlags.cpp), plus
// TrueCoop's rupees. Checks everything first, then writes.
bool ApplySaveBlob(const std::vector<uint8_t>& blob) {
    if (blob.size() < sizeof(TeamSave)) {
        return false;
    }
    std::span<const uint8_t> rando(blob.data() + sizeof(TeamSave), blob.size() - sizeof(TeamSave));
    if (!rando.empty() && !RandoSnapshotValid(rando)) {
        return false;
    }
    TeamSave t;
    memcpy(&t, blob.data(), sizeof(t));
    // a vanilla save and a randomizer save can't be merged (the seed check should stop that earlier)
    if (t.quest.id != gSaveContext.ship.quest.id) {
        SPDLOG_WARN("[Coop] Host's save is a different kind of game (quest {} vs ours {}), not taken", t.quest.id,
                    gSaveContext.ship.quest.id);
        return false;
    }

    gSaveContext.healthCapacity = t.healthCapacity;
    // Anchor leaves health alone, but we can't have more than our max
    if (gSaveContext.health > gSaveContext.healthCapacity) {
        gSaveContext.health = gSaveContext.healthCapacity;
    }
    gSaveContext.magicLevel = t.magicLevel;
    gSaveContext.magicCapacity = t.magicCapacity;
    gSaveContext.magic = static_cast<s8>(t.magicCapacity);
    gSaveContext.isMagicAcquired = t.isMagicAcquired;
    gSaveContext.isDoubleMagicAcquired = t.isDoubleMagicAcquired;
    gSaveContext.isDoubleDefenseAcquired = t.isDoubleDefenseAcquired;
    gSaveContext.bgsFlag = t.bgsFlag;
    gSaveContext.swordHealth = t.swordHealth;
    gSaveContext.ship.quest = t.quest;

    for (size_t i = 0; i < ARRAY_COUNT(gSaveContext.gsFlags); i++) {
        gSaveContext.gsFlags[i] |= t.gsFlags[i];
    }

    gSaveContext.ship.stats.firstInput = t.firstInput;
    gSaveContext.ship.stats.fileCreatedAt = t.fileCreatedAt;

    // Restore bottle contents (unless it's ruto's letter)
    for (int i = 0; i < 4; i++) {
        if (gSaveContext.inventory.items[SLOT_BOTTLE_1 + i] != ITEM_NONE &&
            gSaveContext.inventory.items[SLOT_BOTTLE_1 + i] != ITEM_LETTER_RUTO) {
            t.inventory.items[SLOT_BOTTLE_1 + i] = gSaveContext.inventory.items[SLOT_BOTTLE_1 + i];
        }
    }

    // Restore ammo if it's non-zero, unless it's beans
    for (int i = 0; i < ARRAY_COUNT(gSaveContext.inventory.ammo); i++) {
        if (gSaveContext.inventory.ammo[i] != 0 && i != SLOT(ITEM_BEAN) && i != SLOT(ITEM_BEAN + 1)) {
            t.inventory.ammo[i] = gSaveContext.inventory.ammo[i];
        }
    }

    gSaveContext.inventory = t.inventory;

    // TrueCoop shared wallet: the running session's wallet wins. After the
    // inventory so the cap uses the wallet we just got. Direct write, no
    // Rupees_ChangeBy, so the stats (and our wallet watcher) don't see it.
    s16 walletCap = static_cast<s16>(CUR_CAPACITY(UPG_WALLET));
    gSaveContext.rupeeAccumulator = 0;
    gSaveContext.rupees = std::clamp<s16>(t.rupees, 0, walletCap);

    if (!rando.empty()) {
        ApplyRandoSnapshot(rando);
    }

    // what we just took is the host's, not something to send back
    Rebaseline();
    return true;
}

void RegisterItemHooks() {
    COND_HOOK(OnItemReceive, true, OnItemReceive);
    COND_HOOK(OnDungeonKeyUsed, true, [](uint16_t mapIndex) { SendDungeon(mapIndex); });
    COND_HOOK(OnFlagSet, true, OnFlagSet);
    OnEvent(Coop::EventKind::RupeeChange, "CoopItems.Wallet", OnRupeeChange);
    OnEvent(Coop::EventKind::BeansCount, "CoopItems.Beans", OnBeansCount);
}

} // namespace CoopGame
