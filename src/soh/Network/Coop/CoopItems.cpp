// Shared items: what one player gets, everybody gets.
//
// - an item received (chest, NPC, heart piece...) is sent to the others, who
//   get it the same way (Item_Give / Randomizer_Item_Give), with a small
//   "X found Y" notification
// - dungeon keys/map/compass: we send the dungeon's current values, because
//   keys also go down when someone opens a door
// - when a client loads their save, they get the host's inventory together
//   with the host's flags (see CoopFlags.cpp), so both start from the same place
//
// Built the same way as Anchor's GIVE_ITEM / UPDATE_DUNGEON_ITEMS packets,
// they already found the special cases (heart pieces, BGS, great fairies).

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/item-tables/ItemTableManager.h"
#include "soh/Enhancements/randomizer/static_data.h"
#include "soh/Notification/Notification.h"
#include "soh/SohGui/ImGuiUtils.h"
#include "soh/util.h"

#include <spdlog/spdlog.h>

#include <cstring>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {

// true while we give an item because of the network, so it doesn't bounce back
bool sApplyingRemote = false;

bool CanSend() {
    Coop::SessionRole role = CoopGame::Session().Role();
    return !sApplyingRemote && CoopGame::LocalState().inGame &&
           (role == Coop::SessionRole::Hosting || role == Coop::SessionRole::Connected);
}

void SendItem(uint16_t modId, int16_t getItemId) {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CanSend()) {
        return;
    }
    CoopGame::Session().SendItemGiven({ modId, getItemId });
    SPDLOG_INFO("[Coop] Item sent: mod {} id {:#x}", modId, getItemId);
}

void SendDungeon(uint16_t mapIndex) {
    if (mapIndex >= ARRAY_COUNT(gSaveContext.inventory.dungeonKeys)) {
        return;
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!CanSend()) {
        return;
    }
    CoopGame::Session().SendDungeonItems({ mapIndex, gSaveContext.inventory.dungeonItems[mapIndex],
                                           static_cast<int8_t>(gSaveContext.inventory.dungeonKeys[mapIndex]) });
}

void OnItemReceive(GetItemEntry item) {
    // keys/map/compass: send the whole dungeon state instead
    if (item.modIndex == MOD_NONE && item.itemId >= ITEM_KEY_BOSS && item.itemId <= ITEM_KEY_SMALL) {
        SendDungeon(gSaveContext.mapIndex);
        return;
    }
    // ice traps stay with whoever got trapped
    if (item.modIndex == MOD_RANDOMIZER && item.getItemId == RG_ICE_TRAP) {
        return;
    }
    // the master sword in the last Ganon fight is part of the fight, not an item
    if (item.modIndex == MOD_RANDOMIZER && item.getItemId == RG_MASTER_SWORD && gPlayState != nullptr &&
        gPlayState->sceneNum == SCENE_GANON_BOSS) {
        return;
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

    sApplyingRemote = true;
    if (entry.modIndex == MOD_NONE) {
        if (entry.getItemId == GI_SWORD_BGS) {
            gSaveContext.bgsFlag = true;
        }
        Item_Give(gPlayState, static_cast<u8>(entry.itemId));
    } else if (entry.modIndex == MOD_RANDOMIZER) {
        Randomizer_Item_Give(gPlayState, entry);
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

// What a client takes from the host when joining. Raw copy of these fields:
// both games are the exact same build (checked by the handshake).
struct TeamSave {
    Inventory inventory;
    s16 healthCapacity;
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
    bool hosting = session.Role() == Coop::SessionRole::Hosting;
    if (!LocalState().inGame || gPlayState == nullptr || (!hosting && !FlagsSynced())) {
        return;
    }
    // the host relays without saying who it came from: with one other player we know, else "Teammate"
    std::string from = session.Players().size() == 1 ? session.Players()[0].name : std::string("Teammate");
    for (const Coop::ItemGiven& item : items) {
        GiveItem(from, item);
    }
    for (const Coop::DungeonItems& d : dungeons) {
        ApplyDungeon(d);
    }
}

std::vector<uint8_t> BuildSaveBlob() {
    TeamSave t;
    memset(&t, 0, sizeof(t));
    t.inventory = gSaveContext.inventory;
    t.healthCapacity = gSaveContext.healthCapacity;
    t.magicLevel = gSaveContext.magicLevel;
    t.isMagicAcquired = gSaveContext.isMagicAcquired;
    t.isDoubleMagicAcquired = gSaveContext.isDoubleMagicAcquired;
    t.isDoubleDefenseAcquired = gSaveContext.isDoubleDefenseAcquired;
    t.bgsFlag = gSaveContext.bgsFlag;
    std::vector<uint8_t> out(sizeof(t));
    memcpy(out.data(), &t, sizeof(t));
    return out;
}

bool ApplySaveBlob(const std::vector<uint8_t>& blob) {
    if (blob.size() != sizeof(TeamSave)) {
        return false;
    }
    TeamSave t;
    memcpy(&t, blob.data(), sizeof(t));
    gSaveContext.inventory = t.inventory;
    gSaveContext.healthCapacity = t.healthCapacity;
    gSaveContext.health = t.healthCapacity; // start full
    gSaveContext.magicLevel = t.magicLevel;
    gSaveContext.isMagicAcquired = t.isMagicAcquired;
    gSaveContext.isDoubleMagicAcquired = t.isDoubleMagicAcquired;
    gSaveContext.isDoubleDefenseAcquired = t.isDoubleDefenseAcquired;
    gSaveContext.bgsFlag = t.bgsFlag;
    return true;
}

void RegisterItemHooks() {
    COND_HOOK(OnItemReceive, true, OnItemReceive);
    COND_HOOK(OnDungeonKeyUsed, true, [](uint16_t mapIndex) { SendDungeon(mapIndex); });
    COND_HOOK(OnFlagSet, true, OnFlagSet);
}

} // namespace CoopGame
