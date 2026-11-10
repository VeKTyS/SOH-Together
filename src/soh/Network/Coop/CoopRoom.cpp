// Room settings, copied from Anchor's room state (Packets/UpdateRoomState.cpp,
// Anchor.h RoomState, Menu.cpp AnchorAdminMenu): PvP, who sees who on the map,
// teleport rules, teams.
//
// The host owns the settings (its CVars) and sends them to everyone when they
// change and every few seconds (so people who join later get them too).
// Clients only listen to the host. "Sync items & flags" isn't here: in our
// co-op it's always on.
//
// Plus the host settings of Crests of Courage (menu.cpp group_sharing /
// group_together / group_pvp / group_unfinished, features.cpp
// local_session_flags): Dungeon progress, World objects, Time of day, Death
// link, PvP damage, Hold boss fights. Same texts, same defaults. They ride in
// the same RoomState event, after a version byte.
#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {

// same fields as Anchor's RoomState (minus ownerClientId: the owner is always
// the host here, and syncItemsAndFlags: always on)
struct RoomState {
    uint8_t pvpMode = 1;           // 0 = off, 1 = on (other teams), 2 = on with friendly fire
    uint8_t showLocationsMode = 1; // 0 = none, 1 = team, 2 = all
    uint8_t teleportMode = 1;      // 0 = off, 1 = team, 2 = all
    // Crests of Courage session flags (features.cpp kSess*), CoC's defaults
    bool syncDungeon = true;       // world_dungeon_var: chests, switches, doors
    bool syncWorldObjects = true;  // enemies_breakables_var: pots, grass, blocks
    bool syncTime = true;          // sync_time
    bool deathLink = false;        // death_link
    bool bossWait = false;         // boss_room_wait
    uint16_t pvpDamagePercent = 100; // pvp_damage_percent
};

// Defaults are Anchor's (DefaultIndex(1) in Menu.cpp / PrepRoomState)
constexpr int32_t kDefaultMode = 1;
constexpr int32_t kTeamCount = 8;

// CoC's damage slider: 0 to 400 %, steps of 25 (menu.cpp group_pvp)
constexpr int32_t kDefaultDamage = 100;
constexpr int32_t kMaxDamage = 400;

// The first 3 bytes are Anchor's fields (the old format). Then this version
// byte and the CoC fields. A newer version can add bytes at the end, we read
// what we know and skip the rest.
constexpr uint8_t kRoomStateVersion = 2;
constexpr uint8_t kFlagDungeon = 1 << 0;
constexpr uint8_t kFlagWorldObjects = 1 << 1;
constexpr uint8_t kFlagTime = 1 << 2;
constexpr uint8_t kFlagDeathLink = 1 << 3;
constexpr uint8_t kFlagBossWait = 1 << 4;

// what the host told us last (clients only, the host reads its CVars)
RoomState sHostState;
// to start over from the defaults when we join someone else
uint32_t sStateForSelfId = 0;

// host: when we last sent the settings, and to how many people
std::chrono::steady_clock::time_point sLastSent;
size_t sLastPlayerCount = 0;

// a bad value in the config file would make the combo boxes read past their list
int32_t ReadMode(const char* cvar) {
    return std::clamp(CVarGetInteger(cvar, kDefaultMode), 0, 2);
}

bool ReadFlag(const char* cvar, bool def) {
    return CVarGetInteger(cvar, def ? 1 : 0) != 0;
}

int32_t ReadDamage() {
    return std::clamp(CVarGetInteger(CVAR_COOP("Room.PvpDamage"), kDefaultDamage), 0, kMaxDamage);
}

bool IsHost() {
    return CoopGame::Session().Role() == Coop::SessionRole::Hosting;
}

RoomState Current() {
    if (IsHost()) {
        RoomState s;
        s.pvpMode = static_cast<uint8_t>(ReadMode(CVAR_COOP("Room.PvpMode")));
        s.showLocationsMode = static_cast<uint8_t>(ReadMode(CVAR_COOP("Room.ShowLocationsMode")));
        s.teleportMode = static_cast<uint8_t>(ReadMode(CVAR_COOP("Room.TeleportMode")));
        s.syncDungeon = ReadFlag(CVAR_COOP("Room.SyncDungeon"), true);
        s.syncWorldObjects = ReadFlag(CVAR_COOP("Room.SyncWorldObjects"), true);
        s.syncTime = ReadFlag(CVAR_COOP("Room.SyncTime"), true);
        s.deathLink = ReadFlag(CVAR_COOP("Room.DeathLink"), false);
        s.bossWait = ReadFlag(CVAR_COOP("Room.BossWait"), false);
        s.pvpDamagePercent = static_cast<uint16_t>(ReadDamage());
        return s;
    }
    if (!CoopGame::SessionActive() || CoopGame::SelfId() != sStateForSelfId) {
        return RoomState{}; // not heard from this host yet
    }
    return sHostState;
}

// like Anchor's SendPacket_UpdateRoomState / PrepRoomState
void SendRoomState() {
    RoomState s = Current();
    Coop::ByteWriter w;
    w.U8(s.pvpMode);
    w.U8(s.showLocationsMode);
    w.U8(s.teleportMode);
    // CoC's part (features.cpp send_session_settings + pvp.cpp send_state)
    w.U8(kRoomStateVersion);
    uint8_t flags = 0;
    flags |= s.syncDungeon ? kFlagDungeon : 0;
    flags |= s.syncWorldObjects ? kFlagWorldObjects : 0;
    flags |= s.syncTime ? kFlagTime : 0;
    flags |= s.deathLink ? kFlagDeathLink : 0;
    flags |= s.bossWait ? kFlagBossWait : 0;
    w.U8(flags);
    w.U16(s.pvpDamagePercent);
    CoopGame::SendEvent(Coop::EventKind::RoomState, w.Take());
    sLastSent = std::chrono::steady_clock::now();
}

// like Anchor's HandlePacket_UpdateRoomState, but only the host gets a say
void OnRoomState(const Coop::Event& e) {
    if (e.origin != Coop::kHostPlayerId || IsHost()) {
        return;
    }
    Coop::ByteReader r(e.data);
    RoomState s;
    if (!r.U8(s.pvpMode) || !r.U8(s.showLocationsMode) || !r.U8(s.teleportMode)) {
        return;
    }
    if (s.pvpMode > 2 || s.showLocationsMode > 2 || s.teleportMode > 2) {
        return;
    }
    // only 3 bytes = the old format, the CoC settings keep their defaults
    if (r.Remaining() > 0) {
        uint8_t version = 0;
        uint8_t flags = 0;
        uint16_t damage = 0;
        if (!r.U8(version) || version < kRoomStateVersion || !r.U8(flags) || !r.U16(damage)) {
            return; // broken packet
        }
        if (damage > kMaxDamage) {
            return;
        }
        s.syncDungeon = (flags & kFlagDungeon) != 0;
        s.syncWorldObjects = (flags & kFlagWorldObjects) != 0;
        s.syncTime = (flags & kFlagTime) != 0;
        s.deathLink = (flags & kFlagDeathLink) != 0;
        s.bossWait = (flags & kFlagBossWait) != 0;
        s.pvpDamagePercent = damage;
        // a newer version may have more after this: not for us
    }
    sHostState = s;
    sStateForSelfId = CoopGame::SelfId();
}

void OnFrame() {
    if (!IsHost()) {
        return;
    }
    // someone joined: send right away, else every 2 seconds for the ones we missed
    size_t players = CoopGame::Session().Players().size();
    if (players != sLastPlayerCount || std::chrono::steady_clock::now() - sLastSent > std::chrono::seconds(2)) {
        sLastPlayerCount = players;
        if (players > 0) {
            SendRoomState();
        }
    }
}

// Anchor uses UIWidgets::CVarCombobox; we read the value ourselves first so a
// broken config value can't index past the list
bool ModeCombo(const char* label, const char* cvar, const std::vector<const char*>& items, const char* tooltip) {
    int32_t value = ReadMode(cvar);
    if (UIWidgets::Combobox<int32_t>(label, &value, items,
                                     UIWidgets::ComboboxOptions()
                                         .DefaultIndex(kDefaultMode)
                                         .LabelPosition(UIWidgets::LabelPositions::Near)
                                         .Color(THEME_COLOR)
                                         .Tooltip(tooltip))) {
        CVarSetInteger(cvar, value);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        return true;
    }
    return false;
}

// CoC's add_toggle: a checkbox on a CVar with its help text
bool FlagCheckbox(const char* label, const char* cvar, bool def, const char* tooltip) {
    bool value = ReadFlag(cvar, def);
    if (UIWidgets::Checkbox(label, &value, UIWidgets::CheckboxOptions().Color(THEME_COLOR).Tooltip(tooltip))) {
        CVarSetInteger(cvar, value ? 1 : 0);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        return true;
    }
    return false;
}

// The CoC host settings (menu.cpp build_game). Host: can change them. Client:
// sees the host's values, grayed out. Folded by default, most people never
// need to touch them.
void DrawGameRules() {
    if (!ImGui::TreeNode("Game rules")) {
        return;
    }
    bool host = IsHost();
    RoomState s = Current();
    if (!host) {
        ImGui::TextDisabled("You are connected, so the host's settings are the ones in use.");
    }
    bool changed = false;

    ImGui::BeginDisabled(!host);
    ImGui::SeparatorText("Share");
    if (host) {
        changed |= FlagCheckbox("Dungeon progress", CVAR_COOP("Room.SyncDungeon"), true,
                                "Chests, switches and doors. Open one and it is open for everybody.");
        changed |= FlagCheckbox("World objects", CVAR_COOP("Room.SyncWorldObjects"), true,
                                "Pots, grass, blocks and the like.");
        changed |= FlagCheckbox("Time of day", CVAR_COOP("Room.SyncTime"), true,
                                "One clock for everybody, so it is not day for you and night for them.");
    } else {
        // read-only copies of the host's values
        ImGui::Checkbox("Dungeon progress", &s.syncDungeon);
        ImGui::Checkbox("World objects", &s.syncWorldObjects);
        ImGui::Checkbox("Time of day", &s.syncTime);
    }

    ImGui::SeparatorText("Playing together");
    if (host) {
        changed |= FlagCheckbox("Death link", CVAR_COOP("Room.DeathLink"), false, "One of you dies, everybody dies.");
    } else {
        ImGui::Checkbox("Death link", &s.deathLink);
    }

    ImGui::SeparatorText("PvP");
    // CoC grays the damage out while PvP is off (add_number(..., pvp_enabled_var()))
    ImGui::BeginDisabled(s.pvpMode == 0);
    int32_t damage = s.pvpDamagePercent;
    if (UIWidgets::SliderInt("Damage", &damage,
                             UIWidgets::IntSliderOptions()
                                 .Min(0)
                                 .Max(kMaxDamage)
                                 .Step(25)
                                 .DefaultValue(kDefaultDamage)
                                 .Format("%d%%")
                                 .Color(THEME_COLOR)
                                 .Tooltip("How much of each hit lands, for everybody.")) &&
        host) {
        CVarSetInteger(CVAR_COOP("Room.PvpDamage"), std::clamp(damage, 0, kMaxDamage));
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // our own setting, never the host's (CoC: "Your own setting.")
    FlagCheckbox("Lock on to players", CVAR_COOP("LockOnPlayers"), true,
                 "Z-target other players while PvP is on. Your own setting.");

    ImGui::BeginDisabled(!host);
    ImGui::SeparatorText("Unfinished: these can break your game");
    if (host) {
        changed |= FlagCheckbox("Hold boss fights until everyone's there", CVAR_COOP("Room.BossWait"), false,
                                "The fight waits at the door until the whole party has walked in. Can leave you "
                                "stuck at the door if someone never arrives.");
    } else {
        ImGui::Checkbox("Hold boss fights until everyone's there", &s.bossWait);
    }
    ImGui::EndDisabled();

    if (changed && CoopGame::SessionActive()) {
        SendRoomState();
    }
    ImGui::TreePop();
}

// labels from Anchor's Menu.cpp, a bit more spelled out
const std::vector<const char*> kPvpModes = { "Off", "On (other teams)", "On + Friendly Fire" };
const std::vector<const char*> kShowLocationsModes = { "None", "Team Only", "All" };
const std::vector<const char*> kTeleportModes = { "None", "Team Only", "All" };
const std::vector<const char*> kTeams = { "Team 1", "Team 2", "Team 3", "Team 4",
                                          "Team 5", "Team 6", "Team 7", "Team 8" };

void Register() {
    CoopGame::OnEvent(Coop::EventKind::RoomState, "CoopRoom", OnRoomState);
    CoopGame::OnEveryFrame("CoopRoom", OnFrame);
}

} // namespace

namespace CoopGame {

// --- read by CoopPuppets.cpp / CoopMinimap.cpp (Anchor's roomState fields) ---
uint8_t RoomPvpMode() {
    return Current().pvpMode;
}

uint8_t RoomShowLocationsMode() {
    return Current().showLocationsMode;
}

uint8_t OwnTeam() {
    return static_cast<uint8_t>(std::clamp(CVarGetInteger(CVAR_COOP("Team"), 0), 0, kTeamCount - 1));
}

void DrawRoomSettings() {
    ImGui::SeparatorText("Room");

    // everyone picks their team (Anchor's TeamId box in Menu.cpp)
    int32_t team = OwnTeam();
    if (UIWidgets::Combobox<int32_t>("Your team", &team, kTeams,
                                     UIWidgets::ComboboxOptions()
                                         .LabelPosition(UIWidgets::LabelPositions::Near)
                                         .Color(THEME_COLOR)
                                         .Tooltip("Same team = no PvP between you, and you see each other "
                                                  "on the map when the room says \"Team Only\"."))) {
        CVarSetInteger(CVAR_COOP("Team"), team);
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
    }

    if (!IsHost()) {
        // clients just see what the host picked
        RoomState s = Current();
        ImGui::Text("PvP: %s", kPvpModes[s.pvpMode]);
        ImGui::Text("Show players on the map: %s", kShowLocationsModes[s.showLocationsMode]);
        ImGui::Text("Teleport to: %s", kTeleportModes[s.teleportMode]);
        DrawGameRules();
        return;
    }

    // host: copied from Anchor's AnchorAdminMenu, each change is sent right away
    bool changed = false;
    changed |= ModeCombo("PvP", CVAR_COOP("Room.PvpMode"), kPvpModes,
                         "Off: nobody can hurt anybody.\n"
                         "On: you can hurt players of the other teams.\n"
                         "On + Friendly Fire: you can hurt everyone.");
    changed |= ModeCombo("Show players on the map", CVAR_COOP("Room.ShowLocationsMode"), kShowLocationsModes,
                         "Who shows up on the minimap.");
    changed |= ModeCombo("Teleport to", CVAR_COOP("Room.TeleportMode"), kTeleportModes,
                         "Who you can teleport to.");
    if (changed && SessionActive()) {
        SendRoomState();
    }
    DrawGameRules();
}

// --- the CoC host settings, read by the feature files ---
// Host: its own CVars. Client: what the host sent (defaults until then).
bool RoomSyncDungeon() {
    return Current().syncDungeon;
}

bool RoomSyncWorldObjects() {
    return Current().syncWorldObjects;
}

bool RoomSyncTime() {
    return Current().syncTime;
}

bool RoomDeathLink() {
    return Current().deathLink;
}

bool RoomBossWait() {
    return Current().bossWait;
}

uint16_t RoomPvpDamagePercent() {
    return Current().pvpDamagePercent;
}

// copied from Anchor's CanTeleportTo (Packets/RequestTeleport.cpp), the room
// part only: the scene / age checks are already in CoopGame.cpp
const char* RoomTeleportBlocker(const Coop::RemotePlayer& them) {
    RoomState s = Current();
    // Teleporting is disabled
    if (s.teleportMode == 0) {
        return "Teleport is off in this room";
    }
    // Teleporting to team only, but the client is not on your team
    if (s.teleportMode == 1 && them.state && them.state->team != OwnTeam()) {
        return "Teleport is team only in this room, and they're not on your team";
    }
    return nullptr;
}

} // namespace CoopGame

static RegisterShipInitFunc sInit(Register);
