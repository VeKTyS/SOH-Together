// Room settings, copied from Anchor's room state (Packets/UpdateRoomState.cpp,
// Anchor.h RoomState, Menu.cpp AnchorAdminMenu): PvP, who sees who on the map,
// teleport rules, teams.
//
// The host owns the settings (its CVars) and sends them to everyone when they
// change and every few seconds (so people who join later get them too).
// Clients only listen to the host. "Sync items & flags" isn't here: in our
// co-op it's always on.
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
};

// Defaults are Anchor's (DefaultIndex(1) in Menu.cpp / PrepRoomState)
constexpr int32_t kDefaultMode = 1;
constexpr int32_t kTeamCount = 8;

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

bool IsHost() {
    return CoopGame::Session().Role() == Coop::SessionRole::Hosting;
}

RoomState Current() {
    if (IsHost()) {
        RoomState s;
        s.pvpMode = static_cast<uint8_t>(ReadMode(CVAR_COOP("Room.PvpMode")));
        s.showLocationsMode = static_cast<uint8_t>(ReadMode(CVAR_COOP("Room.ShowLocationsMode")));
        s.teleportMode = static_cast<uint8_t>(ReadMode(CVAR_COOP("Room.TeleportMode")));
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
    if (!r.U8(s.pvpMode) || !r.U8(s.showLocationsMode) || !r.U8(s.teleportMode) || r.Remaining() != 0) {
        return;
    }
    if (s.pvpMode > 2 || s.showLocationsMode > 2 || s.teleportMode > 2) {
        return;
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
