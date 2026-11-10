// On-screen notifications, queued and stacked (Crests of Courage notify.cpp)
// + "X joined" / "X left" (Crests of Courage features.cpp, features_on_roster_changed)
//
// CoC draws them with the game's 2D font on Twilight Princess' 608x448 screen. We draw
// the same boxes with ImGui over the "Main Game" window, in the same 448-tall units, so
// all their numbers (sizes, margins, "distance from the edge") still mean the same thing.
#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Notification/Notification.h"
#include "soh/ShipInit.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/GuiWindow.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {

using Clock = std::chrono::steady_clock;
using CoopGame::NoticeKind;

constexpr int kKinds = 5; // NoticeKind::Item .. NoticeKind::Other

struct Note {
    NoticeKind kind = NoticeKind::Other;
    std::string title;
    std::string body;
    Clock::time_point born;
    Clock::time_point shownAt{}; // set the first frame it's on screen (waiting ones don't age)
    uint32_t lifeMs = 5000;
};
// Notify() can come from any thread, so the queue has its own little lock
std::mutex sNotesMutex;
std::deque<Note> sNotes;
constexpr size_t kNotesKept = 64;
// "Use SoH's notifications instead": SoH's Emit isn't thread safe, so we hand them over from the UI frame
std::vector<Note> sSohPending;

constexpr float kSlideMs = 220.0f;
constexpr float kFadeOutMs = 350.0f;

// settings, same names and defaults as CoC's notify_* config vars
const char* KindCVar(NoticeKind kind) {
    switch (kind) {
        case NoticeKind::Item:
            return CVAR_COOP("Notify.Items");
        case NoticeKind::Players:
            return CVAR_COOP("Notify.Players");
        case NoticeKind::Randomizer:
            return CVAR_COOP("Notify.Rando");
        case NoticeKind::Teleport:
            return CVAR_COOP("Notify.Teleport");
        default:
            return CVAR_COOP("Notify.Other");
    }
}
#define NOTIFY_ON CVAR_COOP("Notify.On")
#define NOTIFY_SIDE CVAR_COOP("Notify.Side")
#define NOTIFY_EDGE CVAR_COOP("Notify.Edge")
#define NOTIFY_FROM_SIDE CVAR_COOP("Notify.FromSide")
#define NOTIFY_FROM_EDGE CVAR_COOP("Notify.FromEdge")
#define NOTIFY_SECONDS CVAR_COOP("Notify.Seconds")
#define NOTIFY_BUSY CVAR_COOP("Notify.BusyTenths")
#define NOTIFY_HURRY CVAR_COOP("Notify.HurryAt")
#define NOTIFY_SIZE CVAR_COOP("Notify.Size")
#define NOTIFY_MAX CVAR_COOP("Notify.Max")
#define NOTIFY_MORE CVAR_COOP("Notify.MoreLine")
#define NOTIFY_SLIDE CVAR_COOP("Notify.Slide")
#define NOTIFY_USE_SOH CVAR_COOP("Notify.UseSoh")

int CVarInt(const char* name, int fallback, int lo, int hi) {
    return std::clamp(static_cast<int>(CVarGetInteger(name, fallback)), lo, hi);
}

// copied from CoC notify.cpp kind_color
ImU32 KindColor(NoticeKind kind, float alpha) {
    int a = static_cast<int>(255.0f * alpha);
    switch (kind) {
        case NoticeKind::Item:
            return IM_COL32(255, 200, 70, a);
        case NoticeKind::Players:
            return IM_COL32(110, 220, 120, a);
        case NoticeKind::Randomizer:
            return IM_COL32(190, 130, 255, a);
        case NoticeKind::Teleport:
            return IM_COL32(90, 180, 255, a);
        default:
            return IM_COL32(200, 200, 200, a);
    }
}

ImVec4 KindColorVec(NoticeKind kind) {
    return ImGui::ColorConvertU32ToFloat4(KindColor(kind, 1.0f));
}

// Where the game picture is on the window, and how many pixels one CoC unit is
// (CoC works on a 448-tall screen).
struct Screen {
    float x = 0, y = 0; // top left of the game picture, in pixels
    float u = 1;        // pixels per CoC unit
    float w = 608;      // width in CoC units
    float h = 448;      // height in CoC units
    ImGuiID viewport = 0;
};

Screen GameScreen() {
    Screen s;
    ImVec2 pos, size;
    ImGuiWindow* game = ImGui::FindWindowByName("Main Game");
    if (game != nullptr && game->Size.y > 1.0f) {
        pos = game->Pos;
        size = game->Size;
        s.viewport = game->ViewportId;
    } else {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        pos = vp->Pos;
        size = vp->Size;
        s.viewport = vp->ID;
    }
    s.x = pos.x;
    s.y = pos.y;
    s.u = std::max(size.y, 1.0f) / 448.0f;
    s.w = size.x / s.u;
    return s;
}

float TextWidth(const std::string& text, float cell, const Screen& s) {
    if (text.empty()) {
        return 0.0f;
    }
    return ImGui::GetFont()->CalcTextSizeA(cell * s.u, FLT_MAX, 0.0f, text.c_str()).x / s.u;
}

// CoC draw_shadowed: a dark copy a bit down-right, then the text. (x, y) = top left, CoC units.
void DrawShadowed(ImDrawList* dl, const Screen& s, float x, float y, float cell, const std::string& text, ImU32 color) {
    float shadow = cell * 0.07f;
    int alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, cell * s.u, ImVec2(s.x + (x + shadow) * s.u, s.y + (y + shadow) * s.u),
                IM_COL32(0, 0, 0, static_cast<int>(alpha * 0.8f)), text.c_str());
    dl->AddText(font, cell * s.u, ImVec2(s.x + x * s.u, s.y + y * s.u), color, text.c_str());
}

void FillBox(ImDrawList* dl, const Screen& s, float x0, float y0, float x1, float y1, ImU32 color) {
    dl->AddRectFilled(ImVec2(s.x + x0 * s.u, s.y + y0 * s.u), ImVec2(s.x + x1 * s.u, s.y + y1 * s.u), color);
}

// A see-through window over the game picture we can draw on. Mouse goes through it.
bool BeginOverlay(const char* name, const Screen& s) {
    ImGui::SetNextWindowPos(ImVec2(s.x, s.y));
    ImGui::SetNextWindowSize(ImVec2(s.w * s.u, s.h * s.u));
    if (s.viewport != 0) {
        ImGui::SetNextWindowViewport(s.viewport);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    bool open = ImGui::Begin(name, nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(2);
    return open;
}

// Text("title\nbody") -> title / body, like CoC's coop_notify(kind, title, body)
void SplitText(const std::string& text, std::string& title, std::string& body) {
    size_t nl = text.find('\n');
    title = text.substr(0, nl);
    body = nl == std::string::npos ? "" : text.substr(nl + 1);
}

// copied from CoC notify.cpp coop_notify
void Queue(NoticeKind kind, const std::string& title, const std::string& body) {
    if (title.empty() && body.empty()) {
        return;
    }
    if (!CVarGetInteger(NOTIFY_ON, 1)) {
        return;
    }
    if (!CVarGetInteger(KindCVar(kind), 1)) {
        return;
    }
    uint32_t ms = static_cast<uint32_t>(CVarInt(NOTIFY_SECONDS, 5, 1, 30) * 1000);

    std::lock_guard lock(sNotesMutex);
    if (CVarGetInteger(NOTIFY_USE_SOH, 0)) {
        Note n;
        n.kind = kind;
        n.title = title;
        n.body = body;
        n.lifeMs = ms;
        if (sSohPending.size() < kNotesKept) {
            sSohPending.push_back(std::move(n));
        }
        return;
    }
    // the same one is already waiting or showing: just give it its time back
    for (Note& waiting : sNotes) {
        if (waiting.title != title || waiting.body != body) {
            continue;
        }
        waiting.lifeMs = ms;
        if (waiting.shownAt != Clock::time_point{}) {
            waiting.shownAt = Clock::now();
        }
        return;
    }
    Note n;
    n.kind = kind;
    n.title = title;
    n.body = body;
    n.born = Clock::now();
    n.lifeMs = ms;
    sNotes.push_back(std::move(n));
    while (sNotes.size() > kNotesKept) {
        sNotes.pop_front();
    }
}

// copied from CoC notify.cpp notify_tick (minus the preview / debug bits): drop the ones whose time is up
void Tick() {
    Clock::time_point now = Clock::now();
    sNotes.erase(std::remove_if(sNotes.begin(), sNotes.end(),
                                [now](const Note& n) {
                                    return n.shownAt != Clock::time_point{} &&
                                           now - n.shownAt > std::chrono::milliseconds(n.lifeMs);
                                }),
                 sNotes.end());
}

// SoH's own pop-ups: title in the kind's color, then the body
void FlushToSoh() {
    for (Note& n : sSohPending) {
        Notification::Options options;
        options.prefix = n.title;
        options.prefixColor = KindColorVec(n.kind);
        options.message = n.body;
        options.messageColor = ImVec4(0.78f, 0.82f, 0.86f, 1.0f);
        options.remainingTime = n.lifeMs / 1000.0f;
        Notification::Emit(options);
    }
    sSohPending.clear();
}

// copied from CoC notify.cpp NotifyDlst::draw, J2D boxes -> ImGui draw list
void DrawNotes() {
    if (sNotes.empty()) {
        return;
    }
    Screen s = GameScreen();
    const float minX = 0.0f, minY = 0.0f, width = s.w, height = s.h;

    const float scale = static_cast<float>(CVarInt(NOTIFY_SIZE, 80, 40, 200)) / 100.0f;
    const float titleCell = 17.0f * scale;
    const float bodyCell = 14.0f * scale;
    const float pad = 8.0f * scale;
    const float stripe = 4.0f * scale;
    const float gap = 6.0f * scale;
    const float margin = 16.0f;
    const float minW = 210.0f * scale;
    const int column = CVarInt(NOTIFY_SIDE, 2, 0, 2);
    const bool top = CVarGetInteger(NOTIFY_EDGE, 1) == 0;
    const float fromSide = static_cast<float>(CVarInt(NOTIFY_FROM_SIDE, 0, 0, 400));
    const float fromEdge = static_cast<float>(CVarInt(NOTIFY_FROM_EDGE, 50, 0, 400));

    const float burstMs = static_cast<float>(CVarInt(NOTIFY_BUSY, 25, 5, 100)) * 100.0f;
    const int hurryAt = CVarInt(NOTIFY_HURRY, 1, 1, 30);
    const bool slides = CVarGetInteger(NOTIFY_SLIDE, 1) != 0;

    const int maxShown = CVarInt(NOTIFY_MAX, 4, 1, 8);
    const int waiting = std::max(0, static_cast<int>(sNotes.size()) - maxShown);
    const Clock::time_point now = Clock::now();

    struct Shown {
        Note* note;
        float alpha;
        float slide;
        float boxW;
        float boxH;
    };
    Shown vis[8];
    int count = 0;

    for (Note& n : sNotes) {
        if (count >= maxShown) {
            break;
        }
        if (n.shownAt == Clock::time_point{}) {
            n.shownAt = now;
        }
        float age = static_cast<float>(std::chrono::duration_cast<std::chrono::milliseconds>(now - n.shownAt).count());
        // lots waiting behind: each one only stays "busy" long
        if (waiting >= hurryAt && static_cast<float>(n.lifeMs) > burstMs) {
            n.lifeMs = static_cast<uint32_t>(std::max(burstMs, age + kFadeOutMs));
        }
        const float life = static_cast<float>(n.lifeMs);
        if (age >= life) {
            continue;
        }
        Shown& v = vis[count++];
        v.note = &n;
        v.alpha = 1.0f;
        if (life - age < kFadeOutMs) {
            v.alpha = (life - age) / kFadeOutMs;
        }
        v.slide = 0.0f;
        if (age < kSlideMs && slides) {
            const float t = age / kSlideMs;
            v.slide = (1.0f - t) * (1.0f - t);
            v.alpha = std::min(v.alpha, 0.35f + 0.65f * t);
        }
        const float titleW = TextWidth(n.title, titleCell, s);
        const float bodyW = TextWidth(n.body, bodyCell, s);
        v.boxW = std::max(minW, std::max(titleW, bodyW) + pad * 2.0f + stripe);
        v.boxH = pad * 2.0f + titleCell + (n.body.empty() ? 0.0f : bodyCell + 3.0f);
    }
    if (count == 0) {
        return;
    }

    // widest first so the stack looks tidy (narrowest first when it grows up from the bottom middle)
    const bool narrowFirst = column == 1 && !top;
    std::stable_sort(vis, vis + count, [narrowFirst](const Shown& l, const Shown& r) {
        return narrowFirst ? l.boxW < r.boxW : l.boxW > r.boxW;
    });

    if (!BeginOverlay("##CoopNotifyOverlay", s)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float totalH = 0.0f;
    for (int i = 0; i < count; ++i) {
        totalH += vis[i].boxH + (i > 0 ? gap : 0.0f);
    }
    const float stackTop = top ? minY + margin + fromEdge : minY + height - margin - fromEdge - totalH;
    float y = stackTop;
    for (int i = 0; i < count; ++i) {
        const Shown& v = vis[i];
        const Note& n = *v.note;
        float x0 = minX + margin + fromSide;
        if (column == 1) {
            x0 = minX + (width - v.boxW) * 0.5f;
        }
        if (column == 2) {
            x0 = minX + width - margin - fromSide - v.boxW;
        }
        float y0 = y;
        // slide in from the side they're on (or from the edge in the middle)
        if (column == 0) {
            x0 -= v.slide * (v.boxW + margin);
        }
        if (column == 2) {
            x0 += v.slide * (v.boxW + margin);
        }
        if (column == 1) {
            y0 += (top ? -1.0f : 1.0f) * v.slide * (v.boxH + margin);
        }

        FillBox(dl, s, x0, y0, x0 + v.boxW, y0 + v.boxH, IM_COL32(12, 14, 20, static_cast<int>(205.0f * v.alpha)));
        FillBox(dl, s, x0, y0, x0 + stripe, y0 + v.boxH, KindColor(n.kind, v.alpha));

        const int a = static_cast<int>(255.0f * v.alpha);
        const float tx = x0 + stripe + pad;
        const float ty = y0 + pad;
        DrawShadowed(dl, s, tx, ty, titleCell, n.title, IM_COL32(255, 255, 255, a));
        if (!n.body.empty()) {
            DrawShadowed(dl, s, tx, ty + titleCell + 3.0f, bodyCell, n.body, IM_COL32(200, 208, 220, a));
        }
        y += v.boxH + gap;
    }

    // "+3 more" under the stack (above it when it grows from the bottom)
    if (waiting > 0 && CVarGetInteger(NOTIFY_MORE, 1)) {
        char more[32];
        std::snprintf(more, sizeof(more), "+%d more", waiting);
        const float w = TextWidth(more, bodyCell, s);
        float mx = minX + margin + fromSide;
        if (column == 1) {
            mx = minX + (width - w) * 0.5f;
        }
        if (column == 2) {
            mx = minX + width - margin - fromSide - w;
        }
        // CoC gives the baseline, we need the top of the text
        const float my = (top ? y + bodyCell : stackTop - 4.0f) - bodyCell * 0.85f;
        DrawShadowed(dl, s, mx, my, bodyCell, more, IM_COL32(225, 230, 240, 235));
    }
    ImGui::End();
}

// ---- players joining and leaving (CoC features.cpp) ----

std::map<uint32_t, std::string> sKnownPlayers;
bool sWasActive = false;

std::string NameOf(const Coop::RemotePlayer& p) {
    return p.name.empty() ? "Player " + std::to_string(p.id) : p.name;
}

// Compares who's here with last time. CoC: announce_peer_once ("X joined"),
// features_on_roster_changed ("X left") and features_on_disconnected.
// Called every game frame in a session and every UI frame (to see the session end).
void TrackRoster() {
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive()) {
        if (sWasActive) {
            if (sKnownPlayers.size() == 1) {
                CoopGame::Notify(NoticeKind::Players, sKnownPlayers.begin()->second + " left\nDisconnected.");
            } else if (sKnownPlayers.size() > 1) {
                CoopGame::Notify(NoticeKind::Players, "Disconnected");
            }
        }
        sKnownPlayers.clear();
        sWasActive = false;
        return;
    }
    std::map<uint32_t, std::string> now;
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (p.id != 0) {
            now[p.id] = NameOf(p);
        }
    }
    for (const auto& [id, name] : now) {
        if (!sKnownPlayers.contains(id)) {
            CoopGame::Notify(NoticeKind::Players, name + " joined");
        }
    }
    for (const auto& [id, name] : sKnownPlayers) {
        if (!now.contains(id)) {
            CoopGame::Notify(NoticeKind::Players, name + " left");
        }
    }
    sKnownPlayers = std::move(now);
    sWasActive = true;
}

// Drawn every UI frame, even on the title screen or with no session (like CoC's 2D pass hook).
class NotifyOverlay final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }
    void Draw() override {
        TrackRoster();
        std::lock_guard lock(sNotesMutex);
        FlushToSoh();
        Tick();
        DrawNotes();
    }
};

void AddOverlay() {
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    if (gui->GetGuiWindow("CoopNotifyOverlay") == nullptr) {
        gui->AddGuiWindow(std::make_shared<NotifyOverlay>("", true, "CoopNotifyOverlay"));
    }
}

// CoC notify.cpp notify_debug_test, with Ocarina of Time things
void SendTestNotes() {
    static const struct {
        NoticeKind kind;
        const char* text;
    } kSamples[] = {
        { NoticeKind::Players, "Saria joined" },
        { NoticeKind::Item, "Malon found the Fairy Slingshot\nYou got it too." },
        { NoticeKind::Randomizer, "Got the host's randomizer seed" },
        { NoticeKind::Teleport, "Teleporting\nHeading to Malon." },
        { NoticeKind::Other, "Malon\ndied, so did you." },
        { NoticeKind::Item, "Saria bottled a Red Potion\nYou got one too." },
        { NoticeKind::Item, "Malon found a Piece of Heart\nYou got it too." },
        { NoticeKind::Players, "Darunia joined" },
        { NoticeKind::Item, "Saria found the Boomerang\nYou got it too." },
        { NoticeKind::Item, "Malon found a Gold Skulltula Token\nYou got it too." },
    };
    for (int round = 0; round < 2; ++round) {
        for (const auto& sample : kSamples) {
            CoopGame::Notify(sample.kind, sample.text);
        }
    }
}

// ---- menu (CoC menu.cpp group_messages) ----

int sSavedFromEdge = -1; // CoC s_savedFromEdge: the edge distance before switching to Middle

void DrawMenu() {
    ImGui::SeparatorText("Notifications");
    UIWidgets::CVarCheckbox("Show notifications", NOTIFY_ON,
                            UIWidgets::CheckboxOptions()
                                .DefaultValue(true)
                                .Tooltip("Turn this off to hide all of them.")
                                .Color(THEME_COLOR));
    ImGui::BeginDisabled(!CVarGetInteger(NOTIFY_ON, 1));
    UIWidgets::CVarCheckbox("Use SoH's notifications instead", NOTIFY_USE_SOH,
                            UIWidgets::CheckboxOptions()
                                .Tooltip("SoH's own pop-ups. They follow SoH's notification settings, "
                                         "not the ones below.")
                                .Color(THEME_COLOR));
    if (UIWidgets::Button("Test notifications", UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
        SendTestNotes();
    }

    if (ImGui::TreeNode("What to show")) {
        UIWidgets::CVarCheckbox(
            "Items they find", KindCVar(NoticeKind::Item),
            UIWidgets::CheckboxOptions()
                .DefaultValue(true)
                .Tooltip("When somebody finds something that matters: a key, an item, a heart piece.")
                .Color(THEME_COLOR));
        UIWidgets::CVarCheckbox("Players joining and leaving", KindCVar(NoticeKind::Players),
                                UIWidgets::CheckboxOptions().DefaultValue(true).Color(THEME_COLOR));
        UIWidgets::CVarCheckbox(
            "Randomizer", KindCVar(NoticeKind::Randomizer),
            UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip("The host's seed arriving, and so on.").Color(
                THEME_COLOR));
        UIWidgets::CVarCheckbox("Teleports", KindCVar(NoticeKind::Teleport),
                                UIWidgets::CheckboxOptions().DefaultValue(true).Color(THEME_COLOR));
        UIWidgets::CVarCheckbox("Everything else", KindCVar(NoticeKind::Other),
                                UIWidgets::CheckboxOptions()
                                    .DefaultValue(true)
                                    .Tooltip("Death link, version mismatches and the rest.")
                                    .Color(THEME_COLOR));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Timing")) {
        UIWidgets::CVarSliderInt("Stay up for", NOTIFY_SECONDS,
                                 UIWidgets::IntSliderOptions()
                                     .Min(1)
                                     .Max(30)
                                     .DefaultValue(5)
                                     .Format("%d s")
                                     .Tooltip("How long each one stays when nothing else is waiting.")
                                     .Color(THEME_COLOR));
        UIWidgets::CVarSliderInt("At once", NOTIFY_MAX,
                                 UIWidgets::IntSliderOptions()
                                     .Min(1)
                                     .Max(8)
                                     .DefaultValue(4)
                                     .Tooltip("How many can be on screen together. The rest wait their turn.")
                                     .Color(THEME_COLOR));
        UIWidgets::CVarSliderInt("When busy, each stays", NOTIFY_BUSY,
                                 UIWidgets::IntSliderOptions()
                                     .Min(5)
                                     .Max(100)
                                     .DefaultValue(25)
                                     .Format("%d /10 s")
                                     .Tooltip("With more waiting behind them, each one only stays this long, so a "
                                              "pile of them comes through in quick bursts instead of taking a "
                                              "minute.")
                                     .Color(THEME_COLOR));
        UIWidgets::CVarSliderInt("Hurry once this many wait", NOTIFY_HURRY,
                                 UIWidgets::IntSliderOptions()
                                     .Min(1)
                                     .Max(30)
                                     .DefaultValue(1)
                                     .Tooltip("How big the queue has to get before they speed up.")
                                     .Color(THEME_COLOR));
        UIWidgets::CVarCheckbox("Show how many are waiting", NOTIFY_MORE,
                                UIWidgets::CheckboxOptions()
                                    .DefaultValue(true)
                                    .Tooltip("A \"+3 more\" line under the stack.")
                                    .Color(THEME_COLOR));
        UIWidgets::CVarCheckbox(
            "Slide in", NOTIFY_SLIDE,
            UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip("Off, they just fade in.").Color(THEME_COLOR));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Position and size")) {
        static const char* kSides[] = { "Left", "Middle", "Right" };
        static const char* kEdges[] = { "Top", "Bottom" };
        int sideBefore = CVarInt(NOTIFY_SIDE, 2, 0, 2);
        if (UIWidgets::CVarCombobox("Side", NOTIFY_SIDE, kSides,
                                    UIWidgets::ComboboxOptions().DefaultIndex(2).Color(THEME_COLOR))) {
            // CoC: the middle sits right at the edge, and gets its old distance back when you leave it
            int side = CVarInt(NOTIFY_SIDE, 2, 0, 2);
            if (side == 1 && sideBefore != 1) {
                sSavedFromEdge = CVarInt(NOTIFY_FROM_EDGE, 50, 0, 400);
                CVarSetInteger(NOTIFY_FROM_EDGE, 0);
            } else if (side != 1 && sideBefore == 1) {
                CVarSetInteger(NOTIFY_FROM_EDGE, sSavedFromEdge >= 0 ? sSavedFromEdge : 50);
                sSavedFromEdge = -1;
            }
            Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
        UIWidgets::CVarCombobox("Edge", NOTIFY_EDGE, kEdges,
                                UIWidgets::ComboboxOptions()
                                    .DefaultIndex(1)
                                    .Tooltip("Which edge the stack starts from. It grows away from it.")
                                    .Color(THEME_COLOR));
        UIWidgets::CVarSliderInt("Distance from the side", NOTIFY_FROM_SIDE,
                                 UIWidgets::IntSliderOptions()
                                     .Min(0)
                                     .Max(400)
                                     .Step(2)
                                     .DefaultValue(0)
                                     .Tooltip("Moves them in from the side they are on. No effect in the middle.")
                                     .Color(THEME_COLOR));
        UIWidgets::CVarSliderInt("Distance from the edge", NOTIFY_FROM_EDGE,
                                 UIWidgets::IntSliderOptions()
                                     .Min(0)
                                     .Max(400)
                                     .Step(2)
                                     .DefaultValue(50)
                                     .Tooltip("Moves them in from the top or bottom.")
                                     .Color(THEME_COLOR));
        UIWidgets::CVarSliderInt(
            "Size", NOTIFY_SIZE,
            UIWidgets::IntSliderOptions().Min(40).Max(200).Step(5).DefaultValue(80).Format("%d%%").Color(THEME_COLOR));
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
}

void Register() {
    CoopGame::OnEveryFrame("Notify.Roster", TrackRoster);
    CoopGame::AddMenuSection("Notifications", CoopGame::MenuWhere::Always, DrawMenu);
}

} // namespace

namespace CoopGame {

// text = "title" or "title\nbody" (CoC notifications have a title and a smaller line under it)
void Notify(NoticeKind kind, const std::string& text) {
    SPDLOG_INFO("[Coop] {}", text);
    std::string title, body;
    SplitText(text, title, body);
    Queue(kind, title, body);
}

} // namespace CoopGame

static RegisterShipInitFunc sNotifyInit(Register);
static RegisterMenuInitFunc sNotifyOverlayInit(AddOverlay);
