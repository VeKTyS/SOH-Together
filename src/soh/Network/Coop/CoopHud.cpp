// Name tags over teammates, an arrow at the screen edge for the ones you can't see,
// their hearts at their feet, and everybody's hearts listed under ours.
//
// Ported from Crests of Courage (CoC):
//   puppet.cpp   PuppetNametagDlst::layout / draw, queue_puppet_nametag, local_hides_peers
//   party_hud.cpp draw_squad, draw_world_hearts, hurt_alpha, draw_name
//   menu.cpp     group_nametags / group_health (option names, defaults, tooltips)
// CoC is Twilight Princess: its numbers are in TP world units (Link is ~150 tall there, ~56 here)
// and in TP's 608x448 screen. We keep their numbers and convert them with kTpUnit / kTpPx.
// CoC draws with TP's font and HUD panes; we draw texture rectangles with OoT's message font
// and the real heart textures of the HUD (z_lifemeter.c).

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/cosmetics/cosmeticsTypes.h"
#include "soh/Enhancements/custom-message/CustomMessageInterfaceAddon.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"
#include "soh/ShipInit.hpp"
#include "soh/ShipUtils.h"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/frame_interpolation.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "textures/parameter_static/parameter_static.h"
extern PlayState* gPlayState;
float OTRGetDimensionFromLeftEdge(float v); // OTRGlobals.h only declares them for C files
int16_t OTRGetRectDimensionFromLeftEdge(float v);
int16_t OTRGetRectDimensionFromRightEdge(float v);
}

namespace {

// --- TP -> OoT ---
// 1 TP unit is about 0.37 of ours (Link ~150 tall in TP, ~56 here)
constexpr float kTpUnit = 0.37f;
// TP's 2D screen is 608x448, ours is 320x240
constexpr float kTpPx = SCREEN_HEIGHT / 448.0f;
// TP runs 30 frames a second, OoT's game logic 20: CoC's timers are turned into seconds first
constexpr int kTicksPerSecond = 20;

// --- settings (CoC menu.cpp group_nametags / group_health, defaults from features.cpp / party_hud.cpp) ---
#define TAGS_ON CVAR_COOP("NameTags")                     // show_nametags, true
#define TAGS_EDGE CVAR_COOP("NameTags.OffScreen")         // nametag_offscreen, true
#define TAGS_HIDE_FAR CVAR_COOP("NameTags.HideFar")       // nametag_hide_far, false
#define TAGS_COLORS CVAR_COOP("NameTags.Colors")          // map_player_colors, 0 = each their own
#define HEARTS_ON CVAR_COOP("NameTags.Hearts")            // nametag_health, true
#define HEARTS_HURT_ONLY CVAR_COOP("NameTags.HurtOnly")   // world_hearts_hurt_only, true
#define HEARTS_SIZE CVAR_COOP("NameTags.HeartsSize")      // world_hearts_size, 55
#define HEARTS_SECONDS CVAR_COOP("NameTags.HurtSeconds")  // world_hearts_seconds, 5
#define HEARTS_FADE CVAR_COOP("NameTags.HurtFadeTenths")  // world_hearts_fade_tenths, 15
#define LIST_ON CVAR_COOP("PartyHud")                     // squad_health, false
#define LIST_SIZE CVAR_COOP("PartyHud.Size")              // squad_health_size, 55
#define LIST_SIDE CVAR_COOP("PartyHud.Side")              // squad_health_side, 0 = left
#define LIST_DOWN CVAR_COOP("PartyHud.Down")              // squad_health_offset_y, 0
#define LIST_FROM_SIDE CVAR_COOP("PartyHud.FromSide")     // squad_health_offset_x, 0

void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

// ---------------------------------------------------------------------------
// drawing bits (texture rectangles on OVERLAY_DISP, in 320x240 "N64 pixels";
// x can go below 0 / above 320 in widescreen, like the HUD)
// ---------------------------------------------------------------------------

struct ScreenArea {
    float minX, minY, width, height;
};

ScreenArea Screen() {
    float left = OTRGetRectDimensionFromLeftEdge(0.0f);
    float right = OTRGetRectDimensionFromRightEdge(SCREEN_WIDTH);
    return { left, 0.0f, right - left, static_cast<float>(SCREEN_HEIGHT) };
}

// The message font is 16x16 per character; the glyphs sit on a line about 13 px down.
// CoC's y is the text baseline (JUTFont), we keep that.
constexpr float kGlyphAscent = 0.8f;

// Name -> the game font's character codes (accents like nametag.cpp does), cached per player
// because Interface_ReplaceSpecialCharacters allocates. CoC caps names at 31 chars.
struct FontName {
    bool done = false;
    std::string raw;
    std::string font;
};
std::map<uint32_t, FontName> sFontNames;

const std::string& ToFont(uint32_t id, const std::string& raw) {
    FontName& entry = sFontNames[id];
    if (entry.done && entry.raw == raw) {
        return entry.font;
    }
    entry.done = true;
    entry.raw = raw;
    std::vector<char> buffer(raw.begin(), raw.end());
    buffer.push_back('\0');
    const char* replaced = Interface_ReplaceSpecialCharacters(buffer.data());
    std::string out = replaced != nullptr ? replaced : "";
    delete[] replaced;
    // same filter as nametag.cpp: only what the font has, no control characters
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](char c) {
                                 uint8_t u = static_cast<uint8_t>(c);
                                 return u > 171 || u < ' ';
                             }),
              out.end());
    if (out.size() > 31) {
        out.resize(31);
    }
    entry.font = out;
    return entry.font;
}

float TextWidth(const std::string& text, float cell) {
    float w = 0.0f;
    for (char c : text) {
        w += Ship_GetCharFontWidth(static_cast<uint8_t>(c)) * cell / FONT_CHAR_TEX_WIDTH;
    }
    return w;
}

int32_t Scale1010(float textureSize, float drawnSize) {
    // texture rectangle step, s5.10 fixed point: how many texels per pixel
    return static_cast<int32_t>(textureSize / std::max(drawnSize, 0.5f) * 1024.0f);
}

void TextPass(Gfx*& g, const std::string& text, float x, float baseline, float cell, uint8_t r, uint8_t gr,
              uint8_t b, uint8_t a) {
    gDPPipeSync(g++);
    // font texture is an intensity map: use it as alpha, color from prim (same as nametag.cpp)
    gDPSetCombineLERP(g++, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0);
    gDPSetPrimColor(g++, 0, 0, r, gr, b, a);
    const float top = baseline - cell * kGlyphAscent;
    const int32_t step = Scale1010(FONT_CHAR_TEX_WIDTH, cell);
    float cx = x;
    for (char ch : text) {
        uint8_t c = static_cast<uint8_t>(ch);
        if (c != ' ') {
            gDPLoadTextureBlock_4b(g++, Ship_GetCharFontTexture(c), G_IM_FMT_I, FONT_CHAR_TEX_WIDTH,
                                   FONT_CHAR_TEX_HEIGHT, 0, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
                                   G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
            gSPWideTextureRectangle(g++, static_cast<s32>(cx * 4), static_cast<s32>(top * 4),
                                    static_cast<s32>((cx + cell) * 4), static_cast<s32>((top + cell) * 4),
                                    G_TX_RENDERTILE, 0, 0, step, step);
        }
        cx += Ship_GetCharFontWidth(c) * cell / FONT_CHAR_TEX_WIDTH;
    }
}

// CoC draw_name / the name tag draw(): a black shadow at 70% then the text
void DrawText(Gfx*& g, const std::string& text, float x, float baseline, float cell, Color_RGB8 color,
              uint8_t alpha) {
    if (text.empty()) {
        return;
    }
    const float shadow = cell * 0.08f;
    TextPass(g, text, x + shadow, baseline + shadow, cell, 0, 0, 0, static_cast<uint8_t>(alpha * 0.7f));
    TextPass(g, text, x, baseline, cell, color.r, color.g, color.b, alpha);
}

// Which heart picture for a heart that's partly full, same table as z_lifemeter.c sHeartTextures
const char* const kHeartTextures[16] = {
    gHeartFullTex,         gHeartQuarterTex,      gHeartQuarterTex,      gHeartQuarterTex,
    gHeartQuarterTex,      gHeartQuarterTex,      gHeartHalfTex,         gHeartHalfTex,
    gHeartHalfTex,         gHeartHalfTex,         gHeartHalfTex,         gHeartThreeQuarterTex,
    gHeartThreeQuarterTex, gHeartThreeQuarterTex, gHeartThreeQuarterTex, gHeartThreeQuarterTex,
};
constexpr int kHeartSlots = 20;   // CoC kHeartSlots, OoT has 20 too
constexpr int kHeartsPerRow = 10; // CoC kHeartsPerRow
constexpr float kHudPitch = 10.0f; // z_lifemeter.c: offsetX += 10 per heart

// HUD heart size: 16x16 picture drawn at HeartsScale (z_lifemeter.c HealthMeter_Draw)
float HudHeartsScale() {
    if (CVarGetInteger(CVAR_COSMETIC("HUD.HeartsCount.PosType"), 0) != ORIGINAL_LOCATION) {
        return CVarGetFloat(CVAR_COSMETIC("HUD.HeartsCount.Scale"), 0.7f);
    }
    return 0.7f;
}

// CoC set_hearts + the screen draw, done the OoT way (HealthMeter_Draw): full hearts, one
// partial heart, empty ones; 10 per row. 16 health = one heart.
void DrawHearts(Gfx*& g, int16_t health, int16_t capacity, float left, float top, float pitch, float size,
                float alpha) {
    int hearts = std::clamp<int>(capacity / 16, 0, kHeartSlots);
    int life = std::clamp<int>(health, 0, hearts * 16);
    int full = life / 16;
    int fraction = life % 16;
    if (fraction == 0) {
        full--;
    }
    InterfaceContext* ui = &gPlayState->interfaceCtx;
    uint8_t a = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);

    gDPPipeSync(g++);
    gDPSetCombineLERP(g++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0, PRIMITIVE,
                      ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
    gDPSetPrimColor(g++, 0, 0, ui->heartsPrimR[0], ui->heartsPrimG[0], ui->heartsPrimB[0], a);
    gDPSetEnvColor(g++, ui->heartsEnvR[0], ui->heartsEnvG[0], ui->heartsEnvB[0], 255);
    const int32_t step = Scale1010(16.0f, size);
    for (int i = 0; i < hearts; i++) {
        const char* tex = i < full ? gHeartFullTex : (i == full ? kHeartTextures[fraction] : gHeartEmptyTex);
        float x = left + (i % kHeartsPerRow) * pitch;
        float y = top + (i / kHeartsPerRow) * pitch;
        gDPLoadTextureBlock(g++, tex, G_IM_FMT_IA, G_IM_SIZ_8b, 16, 16, 0, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        gSPWideTextureRectangle(g++, static_cast<s32>(x * 4), static_cast<s32>(y * 4),
                                static_cast<s32>((x + size) * 4), static_cast<s32>((y + size) * 4),
                                G_TX_RENDERTILE, 0, 0, step, step);
    }
}

// ---------------------------------------------------------------------------
// name tags (CoC puppet.cpp PuppetNametagDlst)
// ---------------------------------------------------------------------------

struct Tag {
    bool visible = false;
    bool edge = false;
    std::string text; // name with the arrow, in font codes
    Color_RGB8 color = { 255, 255, 255 };
    float x = 0, y = 0;
    float cell = 18.0f * kTpPx;
    uint8_t alpha = 255;
    // for the hearts at their feet (CoC puppet_hook_health_anchor)
    bool talking = false; // voice chat: they're talking right now
    bool footOnScreen = false;
    float footX = 0, footY = 0;
    float camDist = 0;
    // CoC smooths the head / feet a bit so the tag doesn't shake
    Vec3f smoothHead = { 0, 0, 0 };
    Vec3f smoothFeet = { 0, 0, 0 };
    bool haveSmooth = false;
};
std::map<uint32_t, Tag> sTags;

// Where a world point lands on screen. dirX/dirY: the direction to it in screen pixels even
// when it's behind the camera (not divided by w). w < 1 = behind (CoC: toCam.z > -1).
struct Projected {
    float x, y;
    float dirX, dirY;
    float w;
};

Projected Project(Vec3f world) {
    Vec3f clip;
    float w = 0.0f;
    SkinMatrix_Vec3fMtxFMultXYZW(&gPlayState->viewProjectionMtxF, &world, &clip, &w);
    if (CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0)) {
        clip.x = -clip.x; // same as the enemy health bar (z_parameter.c)
    }
    Projected p;
    p.dirX = clip.x * (SCREEN_WIDTH / 2);
    p.dirY = -clip.y * (SCREEN_HEIGHT / 2);
    p.w = w;
    if (w > 0.0001f) {
        p.x = SCREEN_WIDTH / 2 + p.dirX / w;
        p.y = SCREEN_HEIGHT / 2 + p.dirY / w;
    } else {
        p.x = p.y = -100000.0f;
    }
    return p;
}

float Dist(const Vec3f& a, const Vec3f& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// CoC local_hides_peers: no tags while a cutscene runs, except when we're just talking to someone
bool LocalHidesPeers() {
    if (gPlayState->csCtx.state != CS_STATE_IDLE) {
        return true;
    }
    Player* me = GET_PLAYER(gPlayState);
    if (me->stateFlags1 & PLAYER_STATE1_TALKING) {
        return false;
    }
    return Player_InCsMode(gPlayState);
}

// CoC queue_puppet_nametag + PuppetNametagDlst::layout. Session locked.
void LayoutTag(const Coop::RemotePlayer& p, Actor* puppet) {
    Tag& tag = sTags[p.id];
    tag.visible = false;

    // CoC: head joint + 45, or pos + 140 when there's no skeleton. The game puts the
    // puppet's focus on its head when it's drawn (Player_PostLimbDrawGameplay).
    Vec3f head = puppet->focus.pos;
    if (head.y <= puppet->world.pos.y) {
        head = puppet->world.pos;
        head.y += 140.0f * kTpUnit;
    } else {
        head.y += 45.0f * kTpUnit;
    }
    Vec3f feet = puppet->world.pos;
    feet.y -= 8.0f * kTpUnit;

    // smoothing, same numbers as CoC (ease 0.5, snap past 400)
    {
        const float kEase = 0.5f;
        const float kSnapDist = 400.0f * kTpUnit;
        if (!tag.haveSmooth || Dist(head, tag.smoothHead) > kSnapDist) {
            tag.smoothHead = head;
            tag.smoothFeet = feet;
            tag.haveSmooth = true;
        } else {
            tag.smoothHead.x += (head.x - tag.smoothHead.x) * (1.0f - kEase);
            tag.smoothHead.y += (head.y - tag.smoothHead.y) * (1.0f - kEase);
            tag.smoothHead.z += (head.z - tag.smoothHead.z) * (1.0f - kEase);
            tag.smoothFeet.x += (feet.x - tag.smoothFeet.x) * (1.0f - kEase);
            tag.smoothFeet.y += (feet.y - tag.smoothFeet.y) * (1.0f - kEase);
            tag.smoothFeet.z += (feet.z - tag.smoothFeet.z) * (1.0f - kEase);
        }
    }
    head = tag.smoothHead;

    const float dist = Dist(head, gPlayState->view.eye);
    // "Hide when far away": about a field's width (CoC 6000)
    if (CVarGetInteger(TAGS_HIDE_FAR, 0) && dist > 6000.0f * kTpUnit) {
        return;
    }

    // full size up close, smaller the further away, never under 7 (CoC numbers)
    const float kFullSizeDist = 500.0f * kTpUnit;
    float cell = dist <= kFullSizeDist ? 18.0f * kTpPx : 18.0f * kTpPx * (kFullSizeDist / dist);
    cell = std::max(cell, 7.0f * kTpPx);
    tag.cell = cell;
    tag.alpha = 255;

    const ScreenArea s = Screen();
    const Projected hp = Project(head);
    const bool behind = hp.w < 1.0f;
    const bool onScreen = !behind && hp.x >= s.minX && hp.x <= s.minX + s.width && hp.y >= s.minY + 16.0f * kTpPx &&
                          hp.y <= s.minY + s.height;

    tag.edge = false;
    const char* marker = nullptr;
    float tx = hp.x;
    float ty = hp.y;
    if (!onScreen) {
        // "Track them off screen": put it on the screen border, on their side
        if (!CVarGetInteger(TAGS_EDGE, 1)) {
            return;
        }
        const float cx = s.minX + s.width * 0.5f;
        const float cy = s.minY + s.height * 0.5f;
        float dx;
        float dy;
        if (behind) {
            // behind us: point the way they are, always along the bottom half (CoC)
            dx = hp.dirX;
            dy = hp.dirY;
            if (dy < 0.0f) {
                dy = -dy;
            }
            if (dx * dx + dy * dy < 1.0f) {
                dy = 1.0f;
            }
        } else {
            dx = hp.x - cx;
            dy = hp.y - cy;
        }
        const float kMargin = 28.0f * kTpPx;
        const float halfW = s.width * 0.5f - kMargin;
        const float halfH = s.height * 0.5f - kMargin;
        const float ax = std::fabs(dx);
        const float ay = std::fabs(dy);
        if (ax < 0.001f && ay < 0.001f) {
            return;
        }
        const float sx = ax > 0.001f ? halfW / ax : 1.0e9f;
        const float sy = ay > 0.001f ? halfH / ay : 1.0e9f;
        const bool sideways = sx < sy;
        const float scale = sideways ? sx : sy;
        tx = cx + dx * scale;
        ty = cy + dy * scale;
        if (sideways) {
            marker = dx < 0.0f ? "<" : ">";
        } else {
            marker = dy < 0.0f ? "^" : "v";
        }
        tag.edge = true;
        tag.cell = 14.0f * kTpPx;
        tag.alpha = 220;
    }

    const std::string& name = ToFont(p.id, p.name);
    if (marker == nullptr) {
        tag.text = name;
    } else if (marker[0] == '<' || marker[0] == '^') {
        tag.text = std::string(marker) + " " + name;
    } else {
        tag.text = name + " " + marker;
    }

    // "Colours": each their own (the color they picked, synced with their state) or all white
    tag.color = CVarGetInteger(TAGS_COLORS, 0) == 1 ? Color_RGB8{ 255, 255, 255 } : CoopGame::PlayerColor(p);

    const Projected fp = Project(tag.smoothFeet);
    tag.x = tx;
    tag.y = ty;
    tag.footX = fp.x;
    tag.footY = fp.y;
    tag.camDist = dist;
    tag.talking = CoopGame::VoiceIsTalking(p.id);
    tag.footOnScreen = fp.w >= 1.0f && fp.x >= s.minX && fp.x <= s.minX + s.width && fp.y >= s.minY &&
                       fp.y <= s.minY + s.height - cell;
    tag.visible = true;
}

// A plain colored box (combiner = prim color only, the loaded texture is ignored)
void Box(Gfx*& g, float x, float y, float w, float h) {
    gSPWideTextureRectangle(g++, static_cast<s32>(x * 4), static_cast<s32>(y * 4), static_cast<s32>((x + w) * 4),
                            static_cast<s32>((y + h) * 4), G_TX_RENDERTILE, 0, 0, 1 << 10, 1 << 10);
}

// CoC draw_voice_icon draws a speaker picture next to the name while they talk. We don't copy
// the picture (GameCube texture blob): three little bars like a sound meter, same spot and size.
void DrawTalkingIcon(Gfx*& g, float x, float baseline, float size, uint8_t alpha) {
    const float top = baseline - size * 0.82f; // CoC: top = baseline - size * 0.82
    const float barW = size * 0.2f;
    const float heights[3] = { 0.4f, 0.7f, 1.0f };
    gDPPipeSync(g++);
    gDPSetCombineMode(g++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    for (int pass = 0; pass < 2; pass++) {
        // shadow first, then white, like the text
        const float shadow = pass == 0 ? size * 0.06f : 0.0f;
        if (pass == 0) {
            gDPSetPrimColor(g++, 0, 0, 0, 0, 0, static_cast<uint8_t>(alpha * 0.7f));
        } else {
            gDPSetPrimColor(g++, 0, 0, 255, 255, 255, alpha);
        }
        for (int i = 0; i < 3; i++) {
            const float h = size * 0.8f * heights[i];
            Box(g, x + shadow + i * barW * 1.6f, top + shadow + size * 0.9f - h, barW, h);
        }
    }
}

// CoC PuppetNametagDlst::draw
void DrawTag(Gfx*& g, const Tag& tag) {
    const float cell = tag.cell;
    const float textWidth = TextWidth(tag.text, cell);
    float x = tag.x - textWidth * 0.5f;
    float y = tag.y;
    if (tag.edge) {
        const ScreenArea s = Screen();
        const float minX = s.minX + 6.0f * kTpPx;
        const float maxX = s.minX + s.width - 6.0f * kTpPx;
        const float minY = s.minY + cell + 6.0f * kTpPx;
        const float maxY = s.minY + s.height - 6.0f * kTpPx;
        if (x < minX) {
            x = minX;
        }
        if (x + textWidth > maxX) {
            x = maxX - textWidth;
        }
        if (y < minY) {
            y = minY;
        }
        if (y > maxY) {
            y = maxY;
        }
    }
    DrawText(g, tag.text, x, y, cell, tag.color, tag.alpha);
    // CoC: the voice icon right after the name, not on the edge arrows; size clamped 14..22
    if (tag.talking && !tag.edge) {
        const float size = std::clamp(cell * 1.25f, 14.0f * kTpPx, 22.0f * kTpPx);
        DrawTalkingIcon(g, x + textWidth + cell * 0.28f, y, size, tag.alpha);
    }
}

// ---------------------------------------------------------------------------
// hearts at their feet (CoC party_hud.cpp draw_world_hearts / hurt_alpha)
// ---------------------------------------------------------------------------

struct HurtWatch {
    int16_t life = 0;
    bool known = false;
    int show = 0;
};
std::map<uint32_t, HurtWatch> sHurt;

int HurtShowTicks() {
    int seconds = CVarGetInteger(HEARTS_SECONDS, 5);
    return (seconds > 0 ? seconds : 5) * kTicksPerSecond;
}

int HurtFadeTicks() {
    int tenths = CVarGetInteger(HEARTS_FADE, 15);
    return tenths * kTicksPerSecond / 10;
}

// "Only when they get hurt": 1 for a while after their health drops, then fades to 0
float HurtAlpha(uint32_t id, int16_t life) {
    HurtWatch& w = sHurt[id];
    if (w.known && life < w.life) {
        w.show = HurtShowTicks();
    }
    w.life = life;
    w.known = true;
    if (w.show <= 0) {
        return 0.0f;
    }
    --w.show;
    const int fade = HurtFadeTicks();
    if (fade <= 0 || w.show >= fade) {
        return 1.0f;
    }
    return static_cast<float>(w.show) / static_cast<float>(fade);
}

// player we know the hearts of (CoC: present && lifeKnown)
bool LifeKnown(const Coop::RemotePlayer& p) {
    return p.state && p.state->inGame && p.state->healthCapacity > 0;
}

void DrawWorldHearts(Gfx*& g, float alphaRate) {
    const bool hurtOnly = CVarGetInteger(HEARTS_HURT_ONLY, 1) != 0;
    const float hudSize = 16.0f * HudHeartsScale();
    // CoC fades them out between 3000 and 4500 from the camera
    const float kFadeStart = 3000.0f * kTpUnit;
    const float kFadeEnd = 4500.0f * kTpUnit;
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (p.id == CoopGame::SelfId() || !LifeKnown(p) || p.state->healthCapacity < 16 ||
            CoopGame::IsBlocked(p.id)) {
            continue;
        }
        auto it = sTags.find(p.id);
        if (it == sTags.end() || !it->second.visible || it->second.edge || !it->second.footOnScreen) {
            continue;
        }
        const Tag& tag = it->second;
        float fade = 1.0f;
        if (tag.camDist > kFadeStart) {
            fade = 1.0f - (tag.camDist - kFadeStart) / (kFadeEnd - kFadeStart);
        }
        if (hurtOnly) {
            fade *= HurtAlpha(p.id, p.state->health);
        }
        if (fade <= 0.02f) {
            continue;
        }
        // "Size" is against the name tag, so they shrink with distance too
        const float pick = std::clamp(CVarGetInteger(HEARTS_SIZE, 55), 20, 150) / 100.0f;
        const float k = pick * (tag.cell / (18.0f * kTpPx));
        const float pitch = kHudPitch * k;
        int hearts = std::min<int>(p.state->healthCapacity / 16, kHeartsPerRow);
        const float rowWidth = hearts * pitch;
        DrawHearts(g, p.state->health, p.state->healthCapacity, tag.footX - rowWidth * 0.5f,
                   tag.footY + pitch * 0.7f, pitch, hudSize * k, alphaRate * fade);
    }
}

// ---------------------------------------------------------------------------
// everybody's hearts under ours (CoC party_hud.cpp draw_squad)
// ---------------------------------------------------------------------------

constexpr float kSquadFloorY = 222.0f * kTpPx; // the list never goes past the middle of the screen
constexpr int kMaxFullEntries = 3;
constexpr int kMaxTextRows = 6;
constexpr float kFitSteps[] = { 1.0f, 0.85f, 0.7f, 0.6f, 0.5f };
constexpr int kFitStepCount = sizeof(kFitSteps) / sizeof(kFitSteps[0]);
int sFitStep = 0;
float sMemberH[Coop::kMaxPlayers + 4] = {};
float sMemberHScale = 0.0f;
std::vector<uint32_t> sOrder; // CoC s_order: the list only reorders every few seconds
int sOrderAge = 0;
bool sHiddenForCrowd = false;

struct SquadMember {
    uint32_t id;
    int rank;
    float dist;
    std::string name; // font codes
    int16_t life;
    int16_t maxLife;
    bool low;
};

// CoC draw_name: white, or red when they're low
void DrawListName(Gfx*& g, const std::string& text, float x, float y, float cell, uint8_t alpha, bool low,
                  bool alignRight) {
    if (alignRight) {
        x -= TextWidth(text, cell);
    }
    DrawText(g, text, x, y, cell, low ? Color_RGB8{ 255, 90, 80 } : Color_RGB8{ 255, 255, 255 }, alpha);
}

void DrawSquad(Gfx*& g) {
    const bool listOn = CVarGetInteger(LIST_ON, 0) != 0;
    const bool worldOn = CVarGetInteger(HEARTS_ON, 1) != 0 && CVarGetInteger(TAGS_ON, 1) != 0;
    if (!listOn && !worldOn) {
        return;
    }
    // follows our own hearts: hidden HUD (cutscenes...) = hidden list
    const float alphaRate = gPlayState->interfaceCtx.healthAlpha / 255.0f;
    if (alphaRate <= 0.01f) {
        return;
    }

    std::vector<SquadMember> members;
    const Player* me = GET_PLAYER(gPlayState);
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (p.id == CoopGame::SelfId() || !LifeKnown(p) || CoopGame::IsBlocked(p.id)) {
            continue;
        }
        SquadMember m;
        m.id = p.id;
        m.name = ToFont(p.id, p.name);
        m.life = p.state->health;
        m.maxLife = p.state->healthCapacity;
        // CoC: life <= a quarter of the max
        m.low = m.maxLife >= 16 && m.life <= m.maxLife / 4;
        const bool sameStage = p.state->sceneNum == gPlayState->sceneNum;
        m.rank = !sameStage ? 2 : (p.state->roomNum == gPlayState->roomCtx.curRoom.num ? 0 : 1);
        m.dist = 1.0e9f;
        if (sameStage) {
            float dx = p.state->x - me->actor.world.pos.x;
            float dy = p.state->y - me->actor.world.pos.y;
            float dz = p.state->z - me->actor.world.pos.z;
            m.dist = dx * dx + dy * dy + dz * dz;
        }
        members.push_back(m);
    }
    // same room first, then same area, then the rest; nearest first. Only re-sorted every
    // 4 s (CoC: 120 frames at 30 fps) so the list doesn't keep jumping around.
    if (++sOrderAge >= 4 * kTicksPerSecond) {
        sOrderAge = 0;
        std::stable_sort(members.begin(), members.end(), [](const SquadMember& a, const SquadMember& b) {
            if (a.rank != b.rank) {
                return a.rank < b.rank;
            }
            return a.dist < b.dist;
        });
        sOrder.clear();
        for (const SquadMember& m : members) {
            sOrder.push_back(m.id);
        }
    } else {
        auto pos = [](uint32_t id) {
            auto it = std::find(sOrder.begin(), sOrder.end(), id);
            return it == sOrder.end() ? sOrder.size() : static_cast<size_t>(it - sOrder.begin());
        };
        std::stable_sort(members.begin(), members.end(),
                         [&](const SquadMember& a, const SquadMember& b) { return pos(a.id) < pos(b.id); });
    }
    int count = static_cast<int>(members.size());

    // CoC: past 3 other players the list hides itself, with a message both ways
    {
        const bool crowd = listOn && count > kMaxFullEntries;
        if (crowd != sHiddenForCrowd) {
            sHiddenForCrowd = crowd;
            if (crowd) {
                CoopGame::Notify(CoopGame::NoticeKind::Players,
                                 "Party HUD hidden\nIt shows up to 3 other players. It comes back when there are 3 "
                                 "or fewer.");
            } else if (listOn) { // CoC also says it when you just turned the list off, we don't
                CoopGame::Notify(CoopGame::NoticeKind::Players, "Party HUD\nBack on. 3 or fewer other players.");
            }
        }
        if (crowd) {
            count = 0;
        }
    }
    if (!listOn) {
        count = 0;
    }
    if (count == 0 && !worldOn) {
        return;
    }

    if (worldOn) {
        DrawWorldHearts(g, alphaRate);
    }
    if (count == 0) {
        return;
    }

    // where our own hearts are (z_lifemeter.c HealthMeter_Draw: heart i is centered on
    // (30 + offsetX + 10 * column, 26 + offsetY + 10 * row), 16 px picture at HeartsScale)
    const float hudScale = HudHeartsScale();
    float offX = getHealthMeterXOffset();
    float offY = getHealthMeterYOffset();
    if (offX < -1000.0f) { // hearts hidden in the cosmetics editor: use their normal spot
        offX = OTRGetDimensionFromLeftEdge(0.0f);
        offY = 0.0f;
    }
    const int lineLength = CVarGetInteger(CVAR_COSMETIC("HUD.Hearts.LineLength"), 10);
    const int myHearts = std::clamp<int>(gSaveContext.healthCapacity / 16, 1, kHeartSlots);
    const float realSize = 16.0f * hudScale;
    const float slotH = realSize;
    const float left = 30.0f + offX - realSize * 0.5f;
    const int myRows = lineLength > 0 ? (myHearts - 1) / lineLength + 1 : 1;
    float bottom = 26.0f + offY + kHudPitch * (myRows - 1) + realSize * 0.5f;

    // the magic bar sits under the hearts: go under it too (CoC does that for its gauges).
    // Only when it's in its normal place, like z_parameter.c computes it.
    if (gSaveContext.magicLevel != 0 && gPlayState->interfaceCtx.magicAlpha > 0 &&
        CVarGetInteger(CVAR_COSMETIC("HUD.MagicBar.PosType"), 0) == ORIGINAL_LOCATION) {
        const int magicDrop = R_MAGIC_BAR_LARGE_Y - R_MAGIC_BAR_SMALL_Y + 2;
        int magicY = R_MAGIC_BAR_SMALL_Y;
        if (lineLength != 0 && (gSaveContext.healthCapacity - 1) / 16 >= lineLength) {
            magicY = R_MAGIC_BAR_LARGE_Y + magicDrop * ((gSaveContext.healthCapacity - 1) / (16 * lineLength) - 1);
        }
        const float gaugeBottom = static_cast<float>(magicY) + 16.0f;
        if (gaugeBottom > bottom && gaugeBottom < bottom + slotH * 4.0f) {
            bottom = gaugeBottom;
        }
    }

    float scalePct = static_cast<float>(CVarGetInteger(LIST_SIZE, 55));
    scalePct = std::clamp(scalePct, 20.0f, 100.0f);
    const bool onRight = CVarGetInteger(LIST_SIDE, 0) == 1;
    const float inset = CVarGetInteger(LIST_FROM_SIDE, 0) * kTpPx;
    const ScreenArea s = Screen();
    float rightEdge = 0.0f;
    if (onRight) {
        rightEdge = s.minX + s.width - (left - s.minX) - inset;
    }
    const float rowLeft = left + inset;
    const float baseK = scalePct / 100.0f;

    // shrink the whole list step by step until it fits above the floor (CoC kFitSteps)
    const float room = kSquadFloorY - bottom;
    if (sMemberHScale > 0.0f && room > 0.0f) {
        float measured = 0.0f;
        for (int n = 0; n < count && n < static_cast<int>(std::size(sMemberH)); ++n) {
            measured += sMemberH[n];
        }
        int best = kFitStepCount - 1;
        for (int i = 0; i < kFitStepCount; ++i) {
            const float need = measured * (baseK * kFitSteps[i]) / sMemberHScale;
            const float allowed = i < sFitStep ? room * 0.92f : room;
            if (need <= allowed) {
                best = i;
                break;
            }
        }
        sFitStep = best;
    }
    const float k = baseK * kFitSteps[sFitStep];
    const float localSlotH = slotH * k;
    const float gap = localSlotH * 0.25f;
    const float nameCell = localSlotH * 0.8f;
    const uint8_t alpha = static_cast<uint8_t>(255.0f * alphaRate);

    float lineTop = bottom + gap + CVarGetInteger(LIST_DOWN, 0) * kTpPx;
    bool compact = false;
    int textRows = 0;
    for (int n = 0; n < count; ++n) {
        const SquadMember& m = members[n];
        const float memberTop = lineTop;
        const float expectH = sMemberHScale > 0.0f && n < static_cast<int>(std::size(sMemberH))
                                  ? sMemberH[n] * k / sMemberHScale
                                  : 0.0f;
        // no room for hearts: one line of text each, "+N more" past 6
        if (compact || n >= kMaxFullEntries || (n > 0 && lineTop + expectH > kSquadFloorY)) {
            compact = true;
            char line[64];
            if (textRows >= kMaxTextRows) {
                std::snprintf(line, sizeof(line), "+%d more", count - n);
                lineTop += nameCell * 1.15f;
                DrawListName(g, line, onRight ? rightEdge : rowLeft, lineTop, nameCell, alpha, false, onRight);
                break;
            }
            ++textRows;
            std::snprintf(line, sizeof(line), "%s  %d/%d", m.name.c_str(), (std::max<int>(m.life, 0) + 15) / 16,
                          m.maxLife / 16);
            lineTop += nameCell * 1.15f;
            DrawListName(g, line, onRight ? rightEdge : rowLeft, lineTop, nameCell, alpha, m.low, onRight);
            continue;
        }

        const float heartsTop = lineTop + nameCell;
        float rowX = rowLeft;
        if (onRight) {
            int perRow = std::clamp<int>(m.maxLife / 16, 1, kHeartsPerRow);
            rowX = rightEdge - ((perRow - 1) * kHudPitch + realSize) * k;
        }
        DrawHearts(g, m.life, m.maxLife, rowX, heartsTop, kHudPitch * k, realSize * k, alphaRate);
        DrawListName(g, m.name, onRight ? rightEdge : rowX, heartsTop - nameCell * 0.12f, nameCell, alpha, m.low,
                     onRight);

        int peerHearts = std::clamp<int>(m.maxLife / 16, 1, kHeartSlots);
        float localBottom = heartsTop + ((peerHearts - 1) / kHeartsPerRow) * kHudPitch * k + realSize * k;
        lineTop = localBottom + gap;
        if (n < static_cast<int>(std::size(sMemberH))) {
            sMemberH[n] = lineTop - memberTop;
        }
    }
    if (!compact) {
        sMemberHScale = k;
    }
}

// forget everything when the session ends, and teammates who left
void ForgetGone() {
    if (!CoopGame::SessionActive()) {
        sTags.clear();
        sHurt.clear();
        sFontNames.clear();
        sOrder.clear();
        sHiddenForCrowd = false;
        sMemberHScale = 0.0f;
        sFitStep = 0;
        return;
    }
    auto gone = [](uint32_t id) { return CoopGame::Session().FindById(id) == nullptr; };
    std::erase_if(sTags, [&](const auto& kv) { return gone(kv.first); });
    std::erase_if(sHurt, [&](const auto& kv) { return gone(kv.first); });
    std::erase_if(sFontNames, [&](const auto& kv) { return gone(kv.first); });
}

// ---------------------------------------------------------------------------
// menu (CoC menu.cpp group_nametags / group_health, same texts)
// ---------------------------------------------------------------------------

void Tip(const char* text) {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool Toggle(const char* label, const char* cvar, int def, const char* tip) {
    bool on = CVarGetInteger(cvar, def) != 0;
    if (ImGui::Checkbox(label, &on)) {
        CVarSetInteger(cvar, on ? 1 : 0);
        SaveCVars();
    }
    Tip(tip);
    return on;
}

// CoC add_number: a slider with a step
void Number(const char* label, const char* cvar, int def, int min, int max, int step, const char* format,
            const char* tip) {
    int value = std::clamp(CVarGetInteger(cvar, def), min, max);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::SliderInt(label, &value, min, max, format, ImGuiSliderFlags_AlwaysClamp)) {
        value = min + (value - min + step / 2) / step * step;
        CVarSetInteger(cvar, std::clamp(value, min, max));
        SaveCVars();
    }
    Tip(tip);
}

void Choice(const char* label, const char* cvar, int def, const char* const* items, int count, const char* tip) {
    int value = std::clamp(CVarGetInteger(cvar, def), 0, count - 1);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::Combo(label, &value, items, count)) {
        CVarSetInteger(cvar, value);
        SaveCVars();
    }
    Tip(tip);
}

// sub-options only show when what they depend on is on (CoC greys them out)
void DrawNameTagSettings() {
    ImGui::SeparatorText("Name tags");
    if (!Toggle("Show their name", TAGS_ON, 1, "Above them, smaller the further away they are.")) {
        return;
    }
    ImGui::Indent();
    Toggle("Track them off screen", TAGS_EDGE, 1,
           "An arrow at the edge of the screen pointing at players you cannot see.");
    Toggle("Hide when far away", TAGS_HIDE_FAR, 0,
           "Tags disappear past about a field's width instead of following them forever.");
    static const char* const kColors[] = { "Each their own", "All white" };
    Choice("Colours", TAGS_COLORS, 0, kColors, 2,
           "Each player's name is in their own colour (the \"Your color\" they picked), so you can tell who is "
           "who.");
    if (Toggle("Show hearts under them", HEARTS_ON, 1, "Real hearts at their feet, sized like their name tag.")) {
        ImGui::Indent();
        Number("Size", HEARTS_SIZE, 55, 20, 150, 5, "%d%%",
               "Against the size of their name tag, so they still shrink with distance.");
        if (Toggle("Only when they get hurt", HEARTS_HURT_ONLY, 1,
                   "Hidden until they take damage, then shown for a while.")) {
            Number("Stay up for", HEARTS_SECONDS, 5, 1, 30, 1, "%d s", "How long after a hit the hearts remain.");
            Number("Fade out over", HEARTS_FADE, 15, 0, 50, 1, "%d tenths of a second",
                   "The tail end of that time is spent fading. Zero cuts them off instead.");
        }
        ImGui::Unindent();
    }
    ImGui::Unindent();
}

void DrawPartyHudSettings() {
    ImGui::SeparatorText("Players on HUD");
    if (!Toggle("Show players on HUD", LIST_ON, 0,
                "Everybody's hearts under your own. Up to 3 other players; with more, it hides itself until "
                "there are 3 or fewer again.")) {
        return;
    }
    ImGui::Indent();
    Number("Size", LIST_SIZE, 55, 20, 100, 5, "%d%%", "How big theirs are next to your own hearts.");
    static const char* const kSides[] = { "Left", "Right" };
    Choice("Side", LIST_SIDE, 0, kSides, 2,
           "Left sits under your own hearts. Right mirrors it on the other edge of the screen.");
    Number("Distance down", LIST_DOWN, 0, 0, 300, 2, "%d",
           "Moves the whole list down. The space between players stays the same.");
    Number("Distance from the side", LIST_FROM_SIDE, 0, -40, 400, 2, "%d",
           "Moves the whole list in from the edge of the screen it is on.");
    ImGui::Unindent();
}

} // namespace

// OPEN_DISPS declares FrameInterpolation_* again inside the function. That only
// stays extern "C" if frame_interpolation.h came first and we're not in a namespace
// (same as CoopMinimap.cpp). Runs at the end of the world draw, before the HUD, so
// it's not drawn over the pause menu (same spot as SoH's nametag.cpp).
static void DrawCoopHud() {
    if (gPlayState == nullptr || GET_PLAYER(gPlayState) == nullptr) {
        return;
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!CoopGame::SessionActive()) {
        return;
    }

    // lay out the tags of the teammates standing in our scene
    for (auto& [id, tag] : sTags) {
        tag.visible = false;
    }
    if (CVarGetInteger(TAGS_ON, 1) && !LocalHidesPeers()) {
        for (Actor* puppet : CoopGame::VisiblePuppets()) {
            const Coop::RemotePlayer* p = CoopGame::PuppetOwner(puppet);
            if (p == nullptr || !p->state || !p->state->inGame || p->state->sceneNum != gPlayState->sceneNum ||
                puppet->shape.shadowAlpha == 0 || p->name.empty() || CoopGame::IsBlocked(p->id)) {
                continue;
            }
            LayoutTag(*p, puppet);
        }
    }

    OPEN_DISPS(gPlayState->state.gfxCtx);
    Gfx* g = OVERLAY_DISP;
    g = Gfx_SetupDL_39(g);

    DrawSquad(g); // hearts at their feet + the list under our hearts
    for (const auto& [id, tag] : sTags) {
        if (tag.visible) {
            DrawTag(g, tag);
        }
    }

    gDPPipeSync(g++);
    OVERLAY_DISP = g;
    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

namespace {

void Register() {
    COND_HOOK(OnPlayDrawEnd, true, DrawCoopHud);
    COND_HOOK(OnGameFrameUpdate, true, []() {
        std::lock_guard lock(CoopGame::Mutex());
        ForgetGone();
    });
    CoopGame::AddMenuSection("Hud.1.NameTags", CoopGame::MenuWhere::Always, DrawNameTagSettings);
    CoopGame::AddMenuSection("Hud.2.PlayersOnHud", CoopGame::MenuWhere::Always, DrawPartyHudSettings);
}

} // namespace

static RegisterShipInitFunc sInit(Register);
