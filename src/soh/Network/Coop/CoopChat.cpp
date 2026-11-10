// Text chat, Network tab, blocking players (Crests of Courage chat.cpp, chat_keys.cpp, menu.cpp)
//
// CoC draws the chat with the game's 2D font on Twilight Princess' 608x448 screen and reads
// the keyboard itself (GetAsyncKeyState + a window hook so the game doesn't see the keys).
// Here ImGui does both: the lines are drawn over the "Main Game" window in the same 448-tall
// units (so CoC's sizes still work), and the typing box is a real ImGui text field. While it
// has the keyboard, SoH already keeps keys away from the game (ControlDeck::KeyboardGameInputBlocked),
// and we block the controller too, like CoC zeroing the pad while you type.
#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/SohMenu.h"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/GuiWindow.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <deque>
#include <string>
#include <string_view>
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

#define CHAT_SHOW CVAR_COOP("Chat.Show")
#define CHAT_SIZE CVAR_COOP("Chat.Size")
#define CHAT_FADE CVAR_COOP("Chat.FadeSeconds")
#define CHAT_BLOCKED CVAR_COOP("Chat.Blocked")
#define SHOW_NETWORK_TAB CVAR_COOP("ShowNetworkTab")

// ---- the lines (CoC chat.cpp) ----

struct Line {
    uint32_t id = 0;   // who said it, 0 = a note from us ("Slow down a little.")
    bool own = false;  // we said it
    std::string name;  // empty for notes
    std::string text;
    Color_RGB8 color{ 255, 214, 120 }; // name color (CoC's gold until we know theirs)
    Clock::time_point at;
};
std::deque<Line> sLines;
constexpr size_t kLinesKept = 100;
constexpr size_t kChatMax = 100;          // characters, same as CoC
constexpr size_t kChatMaxBytes = 4 * 100; // UTF-8: 4 bytes per character at most

bool sToldHow = false;

void Push(Line line) {
    sLines.push_back(std::move(line));
    while (sLines.size() > kLinesKept) {
        sLines.pop_front();
    }
}

// CoC chat_note: a grey line with no name
void ChatNote(const std::string& text) {
    Push(Line{ 0, false, "", text, Color_RGB8{ 190, 190, 190 }, Clock::now() });
}

const char* HowText() {
    return "Press T in game to chat.";
}

// Keeps at most maxChars characters. Broken UTF-8, control characters and the invisible
// "flip the text direction" ones become spaces (CoC turns anything it can't print into ' ').
std::string CleanText(std::string_view in, size_t maxChars) {
    static const uint32_t kMinForLength[5] = { 0, 0, 0x80, 0x800, 0x10000 }; // shorter = "overlong", not allowed
    std::string out;
    size_t chars = 0;
    size_t i = 0;
    while (i < in.size() && chars < maxChars) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        uint32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        }
        bool ok = len != 0 && i + len <= in.size();
        for (size_t k = 1; ok && k < len; k++) {
            unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
            } else {
                cp = (cp << 6) | (cc & 0x3F);
            }
        }
        if (ok && (cp < kMinForLength[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
            ok = false;
        }
        if (!ok) {
            out.push_back(' ');
            i++; // skip just this byte and try again from the next one
            chars++;
            continue;
        }
        bool printable = cp >= 0x20 && cp != 0x7F && !(cp >= 0x80 && cp < 0xA0) && cp != 0x200E && cp != 0x200F &&
                         !(cp >= 0x202A && cp <= 0x202E) && !(cp >= 0x2066 && cp <= 0x2069) && cp != 0xFEFF;
        if (printable) {
            out.append(in.substr(i, len));
        } else {
            out.push_back(' ');
        }
        i += len;
        chars++;
    }
    return out;
}

// CoC send(): no spaces at both ends
std::string Trim(std::string text) {
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    size_t start = text.find_first_not_of(' ');
    return start == std::string::npos ? "" : text.substr(start);
}

std::string OwnName() {
    std::string name = CVarGetString(CVAR_COOP("Name"), "");
    return name.empty() ? "You" : name;
}

std::string NameOf(const Coop::RemotePlayer& p) {
    return p.name.empty() ? "Player " + std::to_string(p.id) : p.name;
}

void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

// ---- blocking (CoC global.cpp global_set_blocked / global_blocked) ----
// CoC remembers a stable player tag. We only have names, so we block by name
// (saved, so it still works next session when their id changed).

std::vector<std::string> sBlocked;
bool sBlockedLoaded = false;

std::vector<std::string>& Blocked() {
    if (!sBlockedLoaded) {
        sBlockedLoaded = true;
        std::string saved = CVarGetString(CHAT_BLOCKED, "");
        size_t at = 0;
        while (at < saved.size()) {
            size_t end = saved.find('\n', at);
            if (end == std::string::npos) {
                end = saved.size();
            }
            if (end > at) {
                sBlocked.push_back(saved.substr(at, end - at));
            }
            at = end + 1;
        }
    }
    return sBlocked;
}

bool NameBlocked(const std::string& name) {
    const std::vector<std::string>& list = Blocked();
    return !name.empty() && std::find(list.begin(), list.end(), name) != list.end();
}

void SaveBlocked() {
    std::string joined;
    for (const std::string& name : Blocked()) {
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += name;
    }
    CVarSetString(CHAT_BLOCKED, joined.c_str());
    SaveCVars();
}

// CoC chat_forget_player: their lines go away right away
void ForgetLines(const std::string& name) {
    sLines.erase(std::remove_if(sLines.begin(), sLines.end(),
                                [&](const Line& l) { return !l.own && !l.name.empty() && l.name == name; }),
                 sLines.end());
}

void SetBlocked(const std::string& name, bool on) {
    if (name.empty()) {
        return;
    }
    std::vector<std::string>& list = Blocked();
    if (on) {
        if (!NameBlocked(name)) {
            // names come from the network: no line breaks in the saved list
            if (name.find('\n') != std::string::npos) {
                return;
            }
            list.push_back(name);
        }
        SaveBlocked();
        ForgetLines(name);
        ChatNote("Blocked " + name + ". Undo it in Network > Co-op > Block players.");
        SPDLOG_INFO("[Coop] Blocked {}", name);
        return;
    }
    list.erase(std::remove(list.begin(), list.end(), name), list.end());
    SaveBlocked();
    SPDLOG_INFO("[Coop] Unblocked {}", name);
}

// ---- sending / receiving (CoC chat.cpp send, chat_on_lobby_message) ----

void Send(const std::string& typed) {
    std::string text = Trim(CleanText(typed, kChatMax));
    if (text.empty()) {
        return;
    }
    if (!CoopGame::SessionActive()) {
        ChatNote("Join a co-op session first.");
        return;
    }
    static Clock::time_point sLast{};
    if (Clock::now() - sLast < std::chrono::seconds(2)) {
        ChatNote("Slow down a little.");
        return;
    }
    sLast = Clock::now();
    CoopGame::SendEvent(Coop::EventKind::Chat, std::vector<uint8_t>(text.begin(), text.end()));
    Push(Line{ CoopGame::SelfId(), true, OwnName(), text, CoopGame::OwnColor(), Clock::now() });
}

// Event data = the text, raw UTF-8 (CoC sends the raw text too). Session locked.
void OnChat(const Coop::Event& e) {
    if (e.data.empty() || e.data.size() > kChatMaxBytes || e.origin == 0 || e.origin == CoopGame::SelfId()) {
        return;
    }
    std::string_view raw(reinterpret_cast<const char*>(e.data.data()), e.data.size());
    std::string text = Trim(CleanText(raw, kChatMax));
    if (text.empty()) {
        return;
    }
    const Coop::RemotePlayer* p = CoopGame::Session().FindById(e.origin);
    std::string name = p != nullptr ? NameOf(*p) : "Player " + std::to_string(e.origin);
    if (NameBlocked(name)) {
        return;
    }
    Line line{ e.origin, false, name, text, Color_RGB8{ 255, 214, 120 }, Clock::now() };
    if (p != nullptr && p->state) {
        line.color = CoopGame::PlayerColor(*p);
    }
    Push(std::move(line));
}

// ---- typing (CoC chat.cpp start_typing / stop_typing / type_update) ----

bool sTyping = false;
char sTyped[kChatMaxBytes + 1] = {};
int sScroll = 0;
bool sHoldingInput = false; // controller still blocked until Enter / Esc are let go
constexpr int32_t kChatInputBlockId = 95237950; // like Mapper.cpp's MAPPER_WINDOW_GAME_INPUT_BLOCK_ID

std::shared_ptr<Ship::ControlDeck> Deck() {
    return Ship::Context::GetRawInstance()->GetControlDeck();
}

void StartTyping() {
    sTyping = true;
    sTyped[0] = '\0';
    sScroll = 0;
    sHoldingInput = false;
    // CoC zeroes the pad while you type, so Link doesn't walk around
    Deck()->BlockGameInput(kChatInputBlockId);
}

void StopTyping() {
    sTyping = false;
    sTyped[0] = '\0';
    sScroll = 0;
    // let go of the controller only once Enter / Esc are up, or the game would
    // see Enter as a fresh Start press and pause (same idea as Mapper.cpp's timer)
    sHoldingInput = true;
}

void ReleaseInputWhenKeysUp() {
    if (sHoldingInput && !ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) &&
        !ImGui::IsKeyDown(ImGuiKey_Escape)) {
        sHoldingInput = false;
        Deck()->UnblockGameInput(kChatInputBlockId);
    }
}

// ImGui counts bytes, CoC counts characters: cut after kChatMax characters
int LimitChars(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit) {
        return 0;
    }
    int chars = 0;
    for (int i = 0; i < data->BufTextLen; i++) {
        if ((static_cast<unsigned char>(data->Buf[i]) & 0xC0) != 0x80 && ++chars > static_cast<int>(kChatMax)) {
            data->DeleteChars(i, data->BufTextLen - i);
            break;
        }
    }
    return 0;
}

// ---- drawing (CoC chat.cpp chat_draw) ----

// Where the game picture is and how many pixels one CoC unit is (CoC's screen is 448 tall).
struct Screen {
    float x = 0, y = 0;
    float u = 1;
    float w = 608, h = 448;
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

// CoC shadowed(); y is the baseline like JUTFont, ImGui wants the top
void Shadowed(ImDrawList* dl, const Screen& s, float x, float y, float cell, const std::string& text, ImU32 color) {
    float top = y - cell * 0.85f;
    float shadow = cell * 0.07f;
    int alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    ImFont* font = ImGui::GetFont();
    dl->AddText(font, cell * s.u, ImVec2(s.x + (x + shadow) * s.u, s.y + (top + shadow) * s.u),
                IM_COL32(0, 0, 0, static_cast<int>(alpha * 0.8f)), text.c_str());
    dl->AddText(font, cell * s.u, ImVec2(s.x + x * s.u, s.y + top * s.u), color, text.c_str());
}

// back up to the start of a UTF-8 character so we never cut one in half
size_t CharStart(const std::string& text, size_t n) {
    while (n > 1 && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) {
        --n;
    }
    return n;
}

// copied from CoC chat.cpp wrap(): split on spaces, cut words that are too long by themselves
std::vector<std::string> Wrap(const std::string& text, float cell, float width, const Screen& s) {
    std::vector<std::string> out;
    std::string line;
    const auto fits = [&](const std::string& t) { return TextWidth(t, cell, s) <= width; };
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find(' ', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string word = text.substr(at, end - at);
        at = end + 1;
        const std::string joined = line.empty() ? word : line + " " + word;
        if (fits(joined)) {
            line = joined;
            continue;
        }
        if (!line.empty()) {
            out.push_back(line);
        }
        line.clear();
        while (!word.empty() && !fits(word)) {
            size_t n = word.size();
            while (n > 1 && !fits(word.substr(0, n))) {
                n = CharStart(word, n - 1);
            }
            n = std::max<size_t>(CharStart(word, n), 1);
            out.push_back(word.substr(0, n));
            word.erase(0, n);
        }
        line = word;
    }
    if (!line.empty()) {
        out.push_back(line);
    }
    return out;
}

ImU32 ColorOf(const Color_RGB8& c, int alpha) {
    return IM_COL32(c.r, c.g, c.b, alpha);
}

// The typing box, bottom middle (CoC: dark panel with a gold stripe). It's a real
// ImGui window so the text field can have the keyboard.
void DrawTypingBox(const Screen& s, float scale) {
    const float cell = 13.0f * scale;
    const float pad = 6.0f * scale;
    const float stripe = 3.0f * scale;
    const float panelW = std::min(s.w * 0.46f, 440.0f * scale);
    const float x0 = (s.w - panelW) * 0.5f;
    const float bottom = s.h - 18.0f;
    const float boxH = cell + pad * 2.0f;
    const float top = bottom - boxH;

    ImGui::SetNextWindowPos(ImVec2(s.x + x0 * s.u, s.y + top * s.u));
    ImGui::SetNextWindowSize(ImVec2(panelW * s.u, boxH * s.u));
    if (s.viewport != 0) {
        ImGui::SetNextWindowViewport(s.viewport);
    }
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2((stripe + pad) * s.u, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 14, 20, 225));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(150, 156, 168, 255));
    bool open = ImGui::Begin("##CoopChatTyping", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddRectFilled(wp, ImVec2(wp.x + stripe * s.u, wp.y + boxH * s.u), IM_COL32(255, 214, 120, 255));

        ImGui::SetWindowFontScale(cell * s.u / std::max(ImGui::GetFont()->FontSize, 1.0f));
        ImGui::SetCursorPosY((boxH * s.u - ImGui::GetFontSize()) * 0.5f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        // keep the keyboard in the box until Enter or Esc (even if they click the game)
        if (!ImGui::IsAnyItemActive()) {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::InputTextWithHint("##CoopChatInput",
                                     "Say something...  (Enter to send, Esc to close, PgUp to scroll)", sTyped,
                                     sizeof(sTyped),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit,
                                     LimitChars)) {
            Send(sTyped);
            StopTyping();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(4);
}

// The last lines, bottom left, fading out (CoC chat_draw)
void DrawLines(const Screen& s, float scale) {
    if (sLines.empty()) {
        return;
    }
    const float cell = 13.0f * scale;
    const float lineGap = cell * 1.25f;
    const float above = cell * 0.9f;
    const float below = cell * 0.3f;
    const float width = s.w;
    const float height = s.h;
    const float lineW = std::min(width * 0.36f, 300.0f * scale);
    // CoC dodges TP's hearts (top left) and minimap / item cross. In OoT the bottom left has
    // the rupees and small keys (y 190-222 of 240), so we stop above those.
    const float left = 14.0f;
    float highest = height * 0.25f + 4.0f + above;
    const float floor = height * (185.0f / 240.0f);
    const float bottomY = floor - 6.0f - below;
    highest = std::min(highest, bottomY);
    float y = bottomY;

    const float fadeSecs = static_cast<float>(std::clamp(CVarGetInteger(CHAT_FADE, 10), 2, 120));
    const int most = sTyping ? 10 : 5;
    int shown = 0;
    const Clock::time_point now = Clock::now();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto first = sLines.rbegin();
    if (sTyping && sScroll > 0) {
        first += static_cast<ptrdiff_t>(std::min<size_t>(static_cast<size_t>(sScroll), sLines.size()));
        Shadowed(dl, s, left, y, cell, "Older messages. PgDn for newer.", IM_COL32(150, 156, 168, 255));
        y -= lineGap;
    }
    for (auto it = first; it != sLines.rend() && shown < most; ++it) {
        float alpha = 1.0f;
        if (!sTyping) {
            const float age = std::chrono::duration<float>(now - it->at).count();
            if (age > fadeSecs + 1.0f) {
                continue;
            }
            if (age > fadeSecs) {
                alpha = 1.0f - (age - fadeSecs);
            }
        }
        const int a = static_cast<int>(255.0f * std::clamp(alpha, 0.0f, 1.0f));
        const std::string label = it->name.empty() ? "" : it->name + ": ";
        std::vector<std::string> rows = Wrap(label + it->text, cell, lineW, s);

        // the newest one may use a wider column if it doesn't fit
        const int fitRows = 1 + static_cast<int>(std::max(0.0f, y - highest) / lineGap);
        if (shown == 0 && static_cast<int>(rows.size()) > fitRows) {
            std::vector<std::string> wide = Wrap(label + it->text, cell, std::min(width * 0.6f, 520.0f * scale), s);
            if (wide.size() < rows.size()) {
                rows = std::move(wide);
            }
        }
        if (shown > 0 && (y < highest || static_cast<int>(rows.size()) > fitRows)) {
            break;
        }
        for (auto row = rows.rbegin(); row != rows.rend(); ++row) {
            const bool firstRow = row + 1 == rows.rend();
            if (firstRow && !label.empty() && row->rfind(label, 0) == 0) {
                // name in the player's color, then the text in white
                Shadowed(dl, s, left, y, cell, label, ColorOf(it->color, a));
                const float nameW = TextWidth(label, cell, s);
                Shadowed(dl, s, left + nameW, y, cell, row->substr(label.size()), IM_COL32(255, 255, 255, a));
            } else {
                Shadowed(dl, s, left, y, cell, *row,
                         it->name.empty() ? IM_COL32(190, 190, 190, a) : IM_COL32(255, 255, 255, a));
            }
            y -= lineGap;
        }
        ++shown;
    }
}

// CoC chat_live() + "Link exists"
bool InGame() {
    return CoopGame::SessionActive() && gPlayState != nullptr && GET_PLAYER(gPlayState) != nullptr;
}

bool MenuUp() {
    return Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetMenuOrMenubarVisible();
}

// CoC chat_update + chat_draw, every UI frame
void ChatFrame() {
    std::lock_guard lock(CoopGame::Mutex());
    ReleaseInputWhenKeysUp();

    if (!InGame()) {
        if (sTyping) {
            StopTyping();
        }
    } else if (sTyping) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            StopTyping();
            // SoH opens its menu on Esc before we get here: that Esc was for us, close it again
            auto menu = Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetMenu();
            if (menu != nullptr && menu->IsVisible()) {
                menu->Hide();
            }
        } else if (MenuUp()) {
            StopTyping();
        } else {
            int scroll = 0;
            scroll += ImGui::IsKeyPressed(ImGuiKey_PageUp, false) ? 4 : 0;
            scroll -= ImGui::IsKeyPressed(ImGuiKey_PageDown, false) ? 4 : 0;
            scroll += ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) ? 1 : 0;
            scroll -= ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) ? 1 : 0;
            sScroll = std::clamp(sScroll + scroll, 0, std::max(0, static_cast<int>(sLines.size()) - 1));
        }
    } else if (!MenuUp() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_T, false)) {
        StartTyping();
    }

    if (!CoopGame::SessionActive() || !CVarGetInteger(CHAT_SHOW, 1)) {
        if (sTyping) {
            const float scale = static_cast<float>(std::clamp(CVarGetInteger(CHAT_SIZE, 100), 50, 200)) / 100.0f;
            DrawTypingBox(GameScreen(), scale);
        }
        return;
    }
    if (sLines.empty() && !sTyping) {
        return;
    }
    const Screen s = GameScreen();
    const float scale = static_cast<float>(std::clamp(CVarGetInteger(CHAT_SIZE, 100), 50, 200)) / 100.0f;

    // see-through window over the game picture to draw the lines on
    ImGui::SetNextWindowPos(ImVec2(s.x, s.y));
    ImGui::SetNextWindowSize(ImVec2(s.w * s.u, s.h * s.u));
    if (s.viewport != 0) {
        ImGui::SetNextWindowViewport(s.viewport);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##CoopChatOverlay", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoMove)) {
        DrawLines(s, scale);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    if (sTyping) {
        DrawTypingBox(s, scale);
    }
}

class ChatOverlay final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }
    void Draw() override {
        ChatFrame();
    }
};

void AddOverlay() {
    auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
    if (gui->GetGuiWindow("CoopChatOverlay") == nullptr) {
        gui->AddGuiWindow(std::make_shared<ChatOverlay>("", true, "CoopChatOverlay"));
    }
}

// ---- network numbers (CoC menu.cpp sample_rates / net_text / net_peers_text) ----

struct NetRates {
    Clock::time_point sampledAt{};
    uint64_t lastSent = 0, lastReceived = 0;
    double upKBs = 0.0, downKBs = 0.0;
    // the counters start over with each session (or wrap): keep what came before
    uint64_t sentBefore = 0, receivedBefore = 0;
};
NetRates sRates;

void SampleRates() {
    Coop::Session& session = CoopGame::Session();
    const uint64_t sent = session.BytesSent();
    const uint64_t received = session.BytesReceived();
    if (sent < sRates.lastSent) {
        sRates.sentBefore += sRates.lastSent;
        sRates.lastSent = 0;
    }
    if (received < sRates.lastReceived) {
        sRates.receivedBefore += sRates.lastReceived;
        sRates.lastReceived = 0;
    }
    const Clock::time_point now = Clock::now();
    if (sRates.sampledAt == Clock::time_point{}) {
        sRates.sampledAt = now;
        sRates.lastSent = sent;
        sRates.lastReceived = received;
        return;
    }
    const double secs = std::chrono::duration<double>(now - sRates.sampledAt).count();
    if (secs < 1.0) {
        return;
    }
    sRates.upKBs = static_cast<double>(sent - sRates.lastSent) / 1024.0 / secs;
    sRates.downKBs = static_cast<double>(received - sRates.lastReceived) / 1024.0 / secs;
    sRates.sampledAt = now;
    sRates.lastSent = sent;
    sRates.lastReceived = received;
}

std::string FormatSize(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024ull) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    }
    return buf;
}

// ---- menu ----

char sMenuDraft[kChatMaxBytes + 1] = {};

// CoC menu.cpp group_global_chat + the chat window (message box and the messages)
void DrawChatMenu() {
    ImGui::SeparatorText("Chat");
    if (CoopGame::SessionActive()) {
        ImGui::TextDisabled("%s", HowText());
        // the last few messages, oldest first
        size_t from = sLines.size() > 5 ? sLines.size() - 5 : 0;
        for (size_t i = from; i < sLines.size(); i++) {
            const Line& l = sLines[i];
            if (l.name.empty()) {
                ImGui::TextDisabled("%s", l.text.c_str());
                continue;
            }
            ImGui::TextColored(ImVec4(l.color.r / 255.0f, l.color.g / 255.0f, l.color.b / 255.0f, 1.0f), "%s:",
                               l.name.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", l.text.c_str());
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##CoopChatMenuInput", "Message (Enter to send)", sMenuDraft, sizeof(sMenuDraft),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit,
                                     LimitChars)) {
            Send(sMenuDraft);
            sMenuDraft[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1); // stay in the box for the next one
        }
    } else {
        ImGui::TextDisabled("Join a co-op session to chat. In game, press T.");
    }
    if (ImGui::TreeNode("Chat options")) {
        UIWidgets::CVarCheckbox("Show chat on screen", CHAT_SHOW,
                                UIWidgets::CheckboxOptions().DefaultValue(true).Color(THEME_COLOR));
        UIWidgets::CVarSliderInt(
            "Chat size", CHAT_SIZE,
            UIWidgets::IntSliderOptions().Min(50).Max(200).Step(10).DefaultValue(100).Format("%d%%").Color(
                THEME_COLOR));
        UIWidgets::CVarSliderInt(
            "Fade after", CHAT_FADE,
            UIWidgets::IntSliderOptions().Min(2).Max(120).DefaultValue(10).Format("%d s").Color(THEME_COLOR));
        ImGui::TreePop();
    }
}

// CoC menu.cpp "Players" (push_block_list / on_block_pressed / "Unblock everyone")
void DrawBlockMenu() {
    ImGui::SeparatorText("Block players");
    ImGui::TextDisabled("Press someone to block them. Press again to unblock.");

    // who's here, who talked lately, who's blocked (each name once)
    std::vector<std::string> rows;
    const auto add = [&](const std::string& name) {
        if (!name.empty() && std::find(rows.begin(), rows.end(), name) == rows.end()) {
            rows.push_back(name);
        }
    };
    if (CoopGame::SessionActive()) {
        for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
            add(NameOf(p));
        }
    }
    for (auto it = sLines.rbegin(); it != sLines.rend(); ++it) {
        if (!it->own && it->id != 0) {
            add(it->name);
        }
    }
    for (const std::string& name : Blocked()) {
        add(name);
    }
    if (rows.empty()) {
        ImGui::TextDisabled("Nobody to show yet.");
    }
    std::string toggle;
    for (const std::string& name : rows) {
        bool blocked = NameBlocked(name);
        ImGui::PushID(name.c_str());
        if (ImGui::Selectable((name + (blocked ? "  (blocked)" : "")).c_str(), blocked)) {
            toggle = name;
        }
        ImGui::PopID();
    }
    if (!toggle.empty()) {
        SetBlocked(toggle, !NameBlocked(toggle));
    }

    ImGui::BeginDisabled(Blocked().empty());
    if (UIWidgets::Button("Unblock everyone", UIWidgets::ButtonOptions()
                                                  .Color(THEME_COLOR)
                                                  .Tooltip("Everyone you blocked can be seen and heard again."))) {
        Blocked().clear();
        SaveBlocked();
        SPDLOG_INFO("[Coop] Unblocked everyone");
    }
    ImGui::EndDisabled();
}

// CoC menu.cpp tab_network (net_text on the left, net_peers_text on the right)
void DrawNetworkMenu() {
    ImGui::SeparatorText("Network");
    UIWidgets::CVarCheckbox("Show Network tab", SHOW_NETWORK_TAB,
                            UIWidgets::CheckboxOptions().Tooltip("Ping and traffic for each player.").Color(
                                THEME_COLOR));
    if (!CVarGetInteger(SHOW_NETWORK_TAB, 0)) {
        return;
    }
    Coop::Session& session = CoopGame::Session();
    const int players = static_cast<int>(session.Players().size()) + 1;
    std::string who = session.Role() == Coop::SessionRole::Hosting
                          ? std::string("Hosting")
                          : "Joined as player " + std::to_string(CoopGame::SelfId());
    ImGui::Text("%s, %d %s", who.c_str(), players, players == 1 ? "player" : "players");
    ImGui::Text("Upload: %.1f KB/s", sRates.upKBs);
    ImGui::Text("Download: %.1f KB/s", sRates.downKBs);
    ImGui::Text("Since launch: %s up, %s down", FormatSize(sRates.sentBefore + session.BytesSent()).c_str(),
                FormatSize(sRates.receivedBefore + session.BytesReceived()).c_str());

    ImGui::Spacing();
    if (session.Players().empty()) {
        ImGui::TextDisabled("Nobody else is here yet.");
    }
    for (const Coop::RemotePlayer& p : session.Players()) {
        ImGui::Text("%s", NameOf(p).c_str());
        if (p.relayed) {
            // we only talk to the host, it passes their stuff on
            ImGui::TextDisabled("  Through the host");
        } else {
            ImGui::TextDisabled("  Ping: %u ms, direct connection (UDP)", session.RoundTripMs(p));
        }
    }
}

// CoC chat_update: tell them once how to chat, the first time we're in game in a session
void OnFrame() {
    SampleRates();
    if (!sToldHow && gPlayState != nullptr) {
        sToldHow = true;
        CoopGame::Notify(CoopGame::NoticeKind::Other, std::string("Chat\n") + HowText());
    }
}

void Register() {
    CoopGame::OnEvent(Coop::EventKind::Chat, "Chat", OnChat);
    CoopGame::OnEveryFrame("Chat", OnFrame);
    CoopGame::AddMenuSection("Chat", CoopGame::MenuWhere::Always, DrawChatMenu);
    CoopGame::AddMenuSection("Chat.Block", CoopGame::MenuWhere::Always, DrawBlockMenu);
    CoopGame::AddMenuSection("Network", CoopGame::MenuWhere::InSession, DrawNetworkMenu);
}

} // namespace

namespace CoopGame {

// Blocked players: their chat is hidden. For voice / name tags too.
bool IsBlocked(uint32_t playerId) {
    std::lock_guard lock(Mutex());
    const Coop::RemotePlayer* p = Session().FindById(playerId);
    return p != nullptr && NameBlocked(NameOf(*p));
}

// True while the chat box has the keyboard (CoC chat_is_typing, voice uses it for push-to-talk)
bool ChatIsTyping() {
    return sTyping;
}

} // namespace CoopGame

static RegisterShipInitFunc sChatInit(Register);
static RegisterMenuInitFunc sChatOverlayInit(AddOverlay);
