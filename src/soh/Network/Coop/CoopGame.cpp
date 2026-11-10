// This is where the co-op code meets the actual game:
// - adds a "Co-op" page in the Network menu (host / join / who's connected)
// - every game frame, reads where Link is and sends it to the others
// Nothing in SoH had to be edited for this, it registers itself on startup.

#include "soh/Network/Coop/BuildId.h"
#include "soh/Network/Coop/Session.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/util.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <mutex>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
extern PlayState* gPlayState;
}

namespace SohGui {
extern std::shared_ptr<SohMenu> mSohMenu;
} // namespace SohGui

#define CVAR_COOP(var) CVAR_REMOTE("Coop." var)

namespace {

// The menu and the game loop both touch the session. They probably run on the
// same thread, but a lock costs nothing here so let's not bet on it.
std::mutex gMutex;

// Last state we sent, kept so the menu can show how far the others are.
Coop::PlayerState gLocalState;

// Addresses shown to the host. Looking them up is slow-ish, so only once when hosting starts.
std::vector<std::string> gHostAddresses;

Coop::Session& GetSession() {
    static Coop::Session session([](const std::string& line) { SPDLOG_INFO("[Coop] {}", line); });
    return session;
}

// Two players can only play together if they run the exact same build.
// SOHT_BUILD_ID is our git commit (set by CMake), gGitCommitHash is SoH's.
uint64_t LocalBuildHash() {
    static const uint64_t hash = Coop::BuildHash(std::string(SOHT_BUILD_ID) + "/soh:" + gGitCommitHash);
    return hash;
}

Coop::Hello MakeHello() {
    Coop::Hello hello;
    hello.buildHash = LocalBuildHash();
    hello.seedHash = 0; // no randomizer support yet
    hello.playerName = CVarGetString(CVAR_COOP("Name"), "");
    return hello;
}

Coop::PlayerState ReadLocalState() {
    static uint32_t frame = 0;
    Coop::PlayerState state;
    state.frame = ++frame;

    // gPlayState is NULL on the title screen and file select
    if (gPlayState == nullptr) {
        return state;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return state;
    }
    state.inGame = true;
    state.sceneNum = gPlayState->sceneNum;
    state.roomNum = gPlayState->roomCtx.curRoom.num;
    state.linkAge = static_cast<uint8_t>(gSaveContext.linkAge);
    state.x = player->actor.world.pos.x;
    state.y = player->actor.world.pos.y;
    state.z = player->actor.world.pos.z;
    state.yaw = player->actor.shape.rot.y;
    return state;
}

void OnFrame() {
    std::lock_guard lock(gMutex);
    gLocalState = ReadLocalState();
    GetSession().Tick(gLocalState);
}

void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

// ---- menu ------------------------------------------------------------------

std::string Where(const Coop::PlayerState& s) {
    if (!s.inGame) {
        return "on the title screen / file select";
    }
    // scene numbers come from the network, don't trust them blindly
    std::string scene =
        s.sceneNum >= 0 && s.sceneNum < SCENE_ID_MAX ? SohUtils::GetSceneName(s.sceneNum) : "unknown place";
    return scene + ", room " + std::to_string(s.roomNum) + (s.linkAge == LINK_AGE_CHILD ? ", child" : ", adult");
}

void DrawPlayer(const Coop::RemotePlayer& p) {
    ImGui::Text("%s  -  %u ms", p.name.c_str(), p.pingMs);
    if (!p.state) {
        ImGui::TextDisabled("  waiting for their first update...");
        return;
    }
    ImGui::Text("  %s", Where(*p.state).c_str());

    // handy to see if things move when they walk around
    if (p.state->inGame && gLocalState.inGame && p.state->sceneNum == gLocalState.sceneNum) {
        float dx = p.state->x - gLocalState.x;
        float dy = p.state->y - gLocalState.y;
        float dz = p.state->z - gLocalState.z;
        ImGui::Text("  same area as you, %.0f units away", std::sqrt(dx * dx + dy * dy + dz * dz));
    }

    // no news for a while = probably lagging or losing packets
    auto silence = std::chrono::steady_clock::now() - p.lastStateAt;
    if (silence > std::chrono::seconds(2)) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0, 1), "  no update for %lld s",
                           (long long)std::chrono::duration_cast<std::chrono::seconds>(silence).count());
    }
}

void DrawOff(Coop::Session& session) {
    std::string name = CVarGetString(CVAR_COOP("Name"), "");
    ImGui::Text("Your name");
    if (UIWidgets::InputString("##CoopName", &name, UIWidgets::InputOptions().Color(THEME_COLOR))) {
        CVarSetString(CVAR_COOP("Name"), name.c_str());
        SaveCVars();
    }
    if (name.empty()) {
        ImGui::TextDisabled("Pick a name first, your friend will see it.");
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(name.empty());
    uint16_t port = static_cast<uint16_t>(CVarGetInteger(CVAR_COOP("Port"), Coop::kDefaultPort));
    if (UIWidgets::Button("Host a game", UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
        if (session.Host(port, MakeHello())) {
            gHostAddresses = Coop::LocalIPv4Addresses();
        }
    }

    ImGui::SeparatorText("Or join a friend");
    std::string address = CVarGetString(CVAR_COOP("JoinAddress"), "");
    ImGui::Text("Their address (like 192.168.1.20)");
    if (UIWidgets::InputString("##CoopAddress", &address, UIWidgets::InputOptions().Color(THEME_COLOR))) {
        CVarSetString(CVAR_COOP("JoinAddress"), address.c_str());
        SaveCVars();
    }
    std::string host;
    uint16_t joinPort = 0;
    bool addressOk = Coop::ParseAddress(address, host, joinPort);
    if (!address.empty() && !addressOk) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "That address doesn't look right.");
    }
    ImGui::BeginDisabled(!addressOk);
    if (UIWidgets::Button("Join", UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
        session.Join(host, joinPort, MakeHello());
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // why the last attempt failed, right where you'll try again
    if (!session.LastError().empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", session.LastError().c_str());
    }
}

void DrawHosting(Coop::Session& session) {
    ImGui::Text("You're hosting. Your friend joins with:");
    if (gHostAddresses.empty()) {
        ImGui::TextDisabled("  couldn't find your local address (check ipconfig)");
    }
    for (const std::string& ip : gHostAddresses) {
        std::string full = ip + ":" + std::to_string(session.Port());
        ImGui::BulletText("%s", full.c_str());
        ImGui::SameLine();
        ImGui::PushID(full.c_str());
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(full.c_str());
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("Not on the same network? They need your public IP and UDP port %u", session.Port());
    ImGui::TextDisabled("open on your router (or use Tailscale).");

    ImGui::SeparatorText("Players");
    if (session.Players().empty()) {
        ImGui::TextDisabled("Nobody yet...");
    }
    for (const Coop::RemotePlayer& p : session.Players()) {
        DrawPlayer(p);
    }
    ImGui::Spacing();
    if (UIWidgets::Button("Stop hosting", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Red))) {
        session.Stop();
    }
}

void DrawClient(Coop::Session& session) {
    if (session.Role() == Coop::SessionRole::Connecting) {
        ImGui::Text("Connecting...");
        ImGui::TextDisabled("If this takes more than a few seconds, check the address and the host's firewall.");
    } else {
        ImGui::Text("Connected!");
        for (const Coop::RemotePlayer& p : session.Players()) {
            DrawPlayer(p);
        }
    }
    ImGui::Spacing();
    if (UIWidgets::Button(session.Role() == Coop::SessionRole::Connecting ? "Cancel" : "Leave",
                          UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Red))) {
        session.Stop();
    }
}

void DrawCoopMenu(WidgetInfo& info) {
    std::lock_guard lock(gMutex);
    Coop::Session& session = GetSession();
    switch (session.Role()) {
        case Coop::SessionRole::Off:
            DrawOff(session);
            break;
        case Coop::SessionRole::Hosting:
            DrawHosting(session);
            break;
        case Coop::SessionRole::Connecting:
        case Coop::SessionRole::Connected:
            DrawClient(session);
            break;
    }
}

void RegisterCoopMenu() {
    WidgetPath path = { "Network", "Co-op", SECTION_COLUMN_1 };
    SohGui::mSohMenu->AddSidebarEntry("Network", path.sidebarName, 1);
    SohGui::mSohMenu->AddWidget(path, "CoopMain", WIDGET_CUSTOM).CustomFunction(DrawCoopMenu).HideInSearch(true);
}

void RegisterCoopFrameHook() {
    SPDLOG_INFO("[Coop] build {}", SOHT_BUILD_ID);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrame);
}

} // namespace

static RegisterMenuInitFunc sCoopMenuInit(RegisterCoopMenu);
static RegisterShipInitFunc sCoopInit(RegisterCoopFrameHook);
