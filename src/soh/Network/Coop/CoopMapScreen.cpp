// Teammates on the pause screen map (Crests of Courage map_markers.cpp, "Map screen").
//
// CoC puts a Link face where each player is, with their name under it, on TP's field map and
// dungeon maps. OoT's pause maps work differently, so this is the same idea fitted to them:
// - world map page: OoT only shows which area you're in (the blue box). So a teammate shows as
//   Link's head in the middle of THEIR area box, name under it. Like CoC, only players outside
//   (field, market, ...) are on it, not the ones inside a building or a dungeon.
// - dungeon map page: OoT has no "where am I" spot on the floor map, only Link's head next to
//   the floor you're on. So teammates in the same dungeon get a head on the row of the floor
//   they're on (CoC: "everybody in the same dungeon, on the floor they are on"). The ones not on
//   the floor you're looking at are faded, or hidden ("Show players on other floors").
//
// There's no hook inside the map page drawing, so we draw right after the pages, from
// VB_DRAW_SAVE_MENU (asked every frame by KaleidoScope_DrawPages once all pages are drawn), with
// the map page's own matrix rebuilt the same way the game builds it. That way the heads sit on
// the page exactly like the game's own icons.
// Who you may see follows the room's "Show Locations" setting and teams, like the minimap.

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/ShipUtils.h"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/frame_interpolation.h"
#include "soh/resource/type/Scene.h"
#include "soh/resource/type/scenecommand/SetCameraSettings.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "textures/icon_item_dungeon_static/icon_item_dungeon_static.h"
extern PlayState* gPlayState;
}

// settings, CoC map_markers_init() names and defaults
#define MAP_FULL CVAR_COOP("Map.Players")             // map_players, off
#define MAP_DUNGEON CVAR_COOP("Map.PlayersDungeon")   // map_players_dungeon, off
#define MAP_NAMES CVAR_COOP("Map.Names")              // map_player_names, on
#define MAP_NAME_SIZE CVAR_COOP("Map.NameSize")       // map_player_name_size, 100 %
#define MAP_OTHER_FLOORS CVAR_COOP("Map.OtherFloors") // map_players_other_floors, on (shared with the minimap)
#define MAP_COLORS CVAR_COOP("Map.Colors")            // map_player_colors, 0 = each their own, 1 = all white

namespace {

// Link's head on the dungeon floor list is 16x16, ours a bit smaller so a few fit on a row
constexpr int kHead = 12;
// CoC: names are 15 px at 100% on TP's 608x448 screen, that's about 8 px on OoT's 320x240
constexpr float kNameHeight = 8.0f;

// the world map area boxes, copied from KaleidoScope_DrawWorldMap (z_kaleido_map_PAL.c, they're static there)
const s16 kAreaBoxPosX[] = {
    -41, 19, 44, 40, 49, 51, -49, 83, 80, -67, 50, -109, -76, -86, -10, -6, 19, 24, 11, -17, 37, -6,
};
const s16 kAreaBoxWidths[] = {
    96, 32, 32, 48, 48, 32, 48, 32, 32, 32, 16, 32, 32, 16, 32, 32, 32, 32, 32, 32, 16, 32,
};
const s16 kAreaBoxPosY[] = {
    30, 36, 35, 26, 7, 11, -31, 30, 38, 23, 2, 42, 40, 32, 38, 50, 57, 58, 56, 12, 36, 50,
};
const s16 kAreaBoxHeights[] = {
    59, 19, 13, 19, 38, 17, 38, 17, 13, 26, 16, 26, 26, 16, 19, 17, 26, 13, 17, 17, 16, 17,
};
constexpr int kAreaCount = ARRAY_COUNT(kAreaBoxPosX);

// One head to draw
struct Mark {
    std::string name;
    Color_RGB8 color;
    int16_t area = -1; // world map
    int8_t floor = -1; // dungeon map
    float alpha = 1.0f;
};

// CoC spot_of(): somebody on the title screen / loading still shows where we last saw them
std::unordered_map<uint32_t, Coop::PlayerState> sLastSeen;

// Same list as the kaleido's sInDungeonScene switch (z_kaleido_scope_PAL.c): the scenes that get
// the dungeon map page. Returns the dungeon (map index, like Map_Init), -1 if none.
int DungeonOf(int16_t scene) {
    if (scene >= SCENE_DEKU_TREE && scene <= SCENE_ICE_CAVERN) {
        return scene;
    }
    if (scene >= SCENE_DEKU_TREE_BOSS && scene <= SCENE_SHADOW_TEMPLE_BOSS) {
        return scene - SCENE_DEKU_TREE_BOSS;
    }
    return -1;
}

// The floor someone is on, same as Map_Update (z_map_exp.c) does for VREG(30)
int FloorOf(int16_t scene, float y) {
    if (gMapData == nullptr) {
        return 0;
    }
    if (scene >= SCENE_DEKU_TREE_BOSS && scene <= SCENE_SHADOW_TEMPLE_BOSS) {
        return std::clamp<int>(gMapData->bossFloor[scene - SCENE_DEKU_TREE_BOSS], 0, 7);
    }
    int dungeon = DungeonOf(scene);
    if (dungeon < 0) {
        return 0;
    }
    int floor = 0;
    for (; floor < 8; floor++) {
        if (y > gMapData->floorCoordY[dungeon][floor]) {
            break;
        }
    }
    return std::min(floor, 7); // the game can end up on 8 when below every floor
}

// Which world map area a scene is in. The game reads it from the scene file when it loads it
// (Scene_CommandMiscSettings), so for a teammate's scene we read that same scene file once.
// Only the "outside" scenes, the ones the game marks as visited on the world map
// (same two ranges as Scene_CommandMiscSettings). -1 = not on the world map.
int16_t WorldAreaOf(int16_t scene) {
    bool outside = (scene >= SCENE_HYRULE_FIELD && scene <= SCENE_OUTSIDE_GANONS_CASTLE) ||
                   (scene >= SCENE_MARKET_ENTRANCE_DAY && scene <= SCENE_TEMPLE_OF_TIME_EXTERIOR_RUINS);
    if (!outside) {
        return -1;
    }
    static std::array<int16_t, SCENE_ID_MAX> sCache = [] {
        std::array<int16_t, SCENE_ID_MAX> a;
        a.fill(-2);
        return a;
    }();
    if (sCache[scene] != -2) {
        return sCache[scene];
    }
    sCache[scene] = -1;
    // same path as OTRPlay_SpawnScene (outside scenes have no MQ version, always "shared")
    const char* file = gSceneTable[scene].sceneFile.fileName;
    if (file == nullptr) {
        return -1;
    }
    std::string path = std::string("scenes/shared/") + file + "/" + file;
    auto res = Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(path);
    auto sceneRes = std::dynamic_pointer_cast<SOH::Scene>(res);
    if (sceneRes == nullptr) {
        SPDLOG_WARN("[Coop] map screen: couldn't read {} for its world map area", path);
        return -1;
    }
    for (const auto& cmd : sceneRes->commands) {
        if (cmd != nullptr && cmd->cmdId == SOH::SceneCommandID::SetCameraSettings) {
            int32_t area = static_cast<SOH::SetCameraSettings*>(cmd.get())->settings.worldMapArea;
            if (area >= 0 && area < kAreaCount) {
                sCache[scene] = static_cast<int16_t>(area);
            }
            break;
        }
    }
    return sCache[scene];
}

// The font only has plain characters (nametag.cpp keeps 32..172, the rest are button icons)
std::string Printable(const std::string& name) {
    std::string out;
    for (char c : name) {
        out += (c >= ' ' && c <= '~') ? c : '?';
        if (out.size() >= 20) {
            break;
        }
    }
    return out.empty() ? std::string("Player") : out;
}

// width in font pixels (16 px tall font), same widths nametag.cpp uses
int TextWidth(const std::string& text) {
    int w = 0;
    for (char c : text) {
        w += static_cast<int>(Ship_GetCharFontWidth(static_cast<u8>(c)));
    }
    return w;
}

float NameScale() {
    return kNameHeight / 16.0f * std::clamp(CVarGetInteger(MAP_NAME_SIZE, 100), 50, 200) / 100.0f;
}

// slot width of one player in a row: the head, or the name if it's wider
float SlotWidth(const Mark& m, bool names) {
    float w = kHead;
    if (names) {
        w = std::max(w, TextWidth(m.name) * NameScale());
    }
    return w + 3.0f;
}

// Everybody we may see right now, with where they are. Session locked by the caller.
std::vector<std::pair<const Coop::RemotePlayer*, Coop::PlayerState>> VisiblePlayers() {
    std::vector<std::pair<const Coop::RemotePlayer*, Coop::PlayerState>> out;
    const auto& players = CoopGame::Session().Players();

    // forget the ones that left
    for (auto it = sLastSeen.begin(); it != sLastSeen.end();) {
        bool still = std::any_of(players.begin(), players.end(),
                                 [&](const Coop::RemotePlayer& p) { return p.id == it->first; });
        it = still ? std::next(it) : sLastSeen.erase(it);
    }

    // Anchor: the room's "Show Locations" mode, 0 = nobody, 1 = my team, 2 = everyone
    uint8_t showLocationsMode = CoopGame::RoomShowLocationsMode();
    if (showLocationsMode == 0) {
        return out;
    }
    uint8_t team = CoopGame::OwnTeam();
    for (const Coop::RemotePlayer& p : players) {
        if (p.id == CoopGame::SelfId()) {
            continue;
        }
        if (p.state && p.state->inGame) {
            sLastSeen[p.id] = *p.state;
        }
        auto seen = sLastSeen.find(p.id);
        if (seen == sLastSeen.end()) {
            continue;
        }
        if (showLocationsMode != 2 && seen->second.team != team) {
            continue;
        }
        out.emplace_back(&p, seen->second);
    }
    return out;
}

} // namespace

// The map page's matrix, built like KaleidoScope_DrawPages does for PAUSE_MAP, then moved to
// (x, y) on the page and scaled. Global scope on purpose: OPEN_DISPS needs it (see CoopMinimap.cpp).
static void LoadPageMatrix(PlayState* play, float x, float y, float scale) {
    PauseContext* pauseCtx = &play->pauseCtx;
    OPEN_DISPS(play->state.gfxCtx);
    Matrix_Translate((f32)WREG(3) / 100.0f, (f32)WREG(2) / 100.0f, 0.0f, MTXMODE_NEW);
    Matrix_Scale(0.78f, 0.78f, 0.78f, MTXMODE_APPLY);
    Matrix_RotateZ(-pauseCtx->unk_1FC / 100.0f, MTXMODE_APPLY);
    Matrix_RotateY(-1.57f, MTXMODE_APPLY);
    Matrix_Translate(x, y, 0.0f, MTXMODE_APPLY);
    Matrix_Scale(scale, scale, 1.0f, MTXMODE_APPLY);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    CLOSE_DISPS(play->state.gfxCtx);
}

// A quad from (0,0) going right and down (page y goes up), laid out like the kaleido's own
// vertices: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right, drawn as 0,2,3,1.
static void DrawQuad(PlayState* play, int w, int h, int texW, int texH) {
    OPEN_DISPS(play->state.gfxCtx);
    Vtx* vtx = static_cast<Vtx*>(Graph_Alloc(play->state.gfxCtx, 4 * sizeof(Vtx)));
    for (int i = 0; i < 4; i++) {
        bool right = (i & 1) != 0;
        bool bottom = (i & 2) != 0;
        vtx[i].v.ob[0] = static_cast<s16>(right ? w : 0);
        vtx[i].v.ob[1] = static_cast<s16>(bottom ? -h : 0);
        vtx[i].v.ob[2] = 0;
        vtx[i].v.flag = 0;
        vtx[i].v.tc[0] = static_cast<s16>((right ? texW : 0) << 5);
        vtx[i].v.tc[1] = static_cast<s16>((bottom ? texH : 0) << 5);
        vtx[i].v.cn[0] = vtx[i].v.cn[1] = vtx[i].v.cn[2] = vtx[i].v.cn[3] = 255;
    }
    gSPVertex(POLY_OPA_DISP++, (uintptr_t)vtx, 4, 0);
    gSP1Quadrangle(POLY_OPA_DISP++, 0, 2, 3, 1, 0);
    CLOSE_DISPS(play->state.gfxCtx);
}

// Link's head, top-left corner at (x, y) on the page
static void DrawHead(PlayState* play, float x, float y, u8 alpha) {
    OPEN_DISPS(play->state.gfxCtx);
    LoadPageMatrix(play, x, y, kHead / 16.0f);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 255, alpha);
    gDPLoadTextureBlock(POLY_OPA_DISP++, gDungeonMapLinkHeadTex, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0,
                        G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD,
                        G_TX_NOLOD);
    DrawQuad(play, 16, 16, 16, 16);
    CLOSE_DISPS(play->state.gfxCtx);
}

// One line of text, top at y, centered on x. Font chars like nametag.cpp (game font, 16 px tall).
static void DrawPageText(PlayState* play, const std::string& text, float centerX, float top, Color_RGB8 color,
                     u8 alpha) {
    float scale = NameScale();
    float left = centerX - TextWidth(text) * scale / 2.0f;
    OPEN_DISPS(play->state.gfxCtx);
    LoadPageMatrix(play, left, top, scale);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, color.r, color.g, color.b, alpha);
    int penX = 0;
    for (char c : text) {
        int w = static_cast<int>(Ship_GetCharFontWidth(static_cast<u8>(c)));
        if (c != ' ' && w > 0) {
            void* tex = Ship_GetCharFontTexture(static_cast<u8>(c));
            gDPLoadTextureBlock_4b(POLY_OPA_DISP++, tex, G_IM_FMT_I, FONT_CHAR_TEX_WIDTH, FONT_CHAR_TEX_HEIGHT, 0,
                                   G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK, G_TX_NOMASK,
                                   G_TX_NOLOD, G_TX_NOLOD);
            // move the quad to the pen, in font pixels
            Vtx* vtx = static_cast<Vtx*>(Graph_Alloc(play->state.gfxCtx, 4 * sizeof(Vtx)));
            for (int i = 0; i < 4; i++) {
                bool right = (i & 1) != 0;
                bool bottom = (i & 2) != 0;
                vtx[i].v.ob[0] = static_cast<s16>(penX + (right ? w : 0));
                vtx[i].v.ob[1] = static_cast<s16>(bottom ? -FONT_CHAR_TEX_HEIGHT : 0);
                vtx[i].v.ob[2] = 0;
                vtx[i].v.flag = 0;
                vtx[i].v.tc[0] = static_cast<s16>((right ? w : 0) << 5);
                vtx[i].v.tc[1] = static_cast<s16>((bottom ? FONT_CHAR_TEX_HEIGHT : 0) << 5);
                vtx[i].v.cn[0] = vtx[i].v.cn[1] = vtx[i].v.cn[2] = vtx[i].v.cn[3] = 255;
            }
            gSPVertex(POLY_OPA_DISP++, (uintptr_t)vtx, 4, 0);
            gSP1Quadrangle(POLY_OPA_DISP++, 0, 2, 3, 1, 0);
        }
        penX += w;
    }
    CLOSE_DISPS(play->state.gfxCtx);
}

// CoC on_draw_icons_post: a dark copy a bit down-right first, then the name in their colour
static void DrawName(PlayState* play, const Mark& m, float centerX, float top, u8 alpha) {
    float shadow = 16.0f * NameScale() * 0.08f; // CoC: cell * 0.08
    DrawPageText(play, m.name, centerX + shadow, top - shadow, Color_RGB8{ 0, 0, 0 }, static_cast<u8>(alpha * 0.8f));
    DrawPageText(play, m.name, centerX, top, m.color, alpha);
}

// A row of heads (names under them), starting at x, head tops at y
static void DrawRow(PlayState* play, const std::vector<const Mark*>& row, float x, float y, u8 pageAlpha,
                    bool names) {
    for (const Mark* m : row) {
        float slot = SlotWidth(*m, names);
        float center = x + slot / 2.0f;
        u8 alpha = static_cast<u8>(pageAlpha * std::clamp(m->alpha, 0.0f, 1.0f));
        if (alpha >= 13) { // CoC skips anything under 5%
            DrawHead(play, center - kHead / 2.0f, y, alpha);
            if (names) {
                DrawName(play, *m, center, y - kHead, alpha);
            }
        }
        x += slot;
    }
}

static void DrawMapScreenMarkers() {
    PlayState* play = gPlayState;
    if (play == nullptr || !CoopGame::SessionActive()) {
        return;
    }
    PauseContext* pauseCtx = &play->pauseCtx;
    // only once the menu is open on the map page and not turning to another page
    // (state 6 = open, unk_1E4 1 = switching pages, debugState = the inventory editor)
    if (pauseCtx->state != 6 || pauseCtx->debugState != 0 || pauseCtx->pageIndex != PAUSE_MAP ||
        pauseCtx->unk_1E4 == 1 || gMapData == nullptr) {
        return;
    }

    int myDungeon = DungeonOf(play->sceneNum);
    bool dungeonPage = myDungeon >= 0;
    if (!CVarGetInteger(dungeonPage ? MAP_DUNGEON : MAP_FULL, 0)) {
        return;
    }
    bool names = CVarGetInteger(MAP_NAMES, 1) != 0;
    bool otherFloors = CVarGetInteger(MAP_OTHER_FLOORS, 1) != 0;
    bool allWhite = CVarGetInteger(MAP_COLORS, 0) == 1;
    // the floor shown on the right (dungeonMapSlot 3..10 = floors 0..7, see KaleidoScope_DrawDungeonMap)
    int shownFloor = pauseCtx->dungeonMapSlot - 3;

    // copy what we need with the session locked, the rest (reading a scene file) without it
    std::vector<std::pair<Mark, Coop::PlayerState>> seen;
    {
        std::lock_guard lock(CoopGame::Mutex());
        for (const auto& [p, state] : VisiblePlayers()) {
            Mark m;
            m.name = Printable(p->name);
            m.color = allWhite ? Color_RGB8{ 255, 255, 255 } : CoopGame::PlayerColor(*p);
            seen.emplace_back(std::move(m), state);
        }
    }
    std::vector<Mark> marks;
    {
        for (auto& [m, state] : seen) {
            if (dungeonPage) {
                // CoC dmap_place: only the ones in the same dungeon (boss room included)
                if (DungeonOf(state.sceneNum) != myDungeon) {
                    continue;
                }
                m.floor = static_cast<int8_t>(FloorOf(state.sceneNum, state.y));
                // CoC dmap_alpha: on another floor = 35%, or not at all
                if (m.floor != shownFloor) {
                    if (!otherFloors) {
                        continue;
                    }
                    m.alpha = 0.35f;
                }
            } else {
                m.area = WorldAreaOf(state.sceneNum);
                if (m.area < 0) {
                    continue; // inside a building or a dungeon: not on the world map (CoC)
                }
            }
            marks.push_back(std::move(m));
        }
    }
    if (marks.empty()) {
        return;
    }

    u8 pageAlpha = static_cast<u8>(std::min<u16>(pauseCtx->alpha, 255));
    OPEN_DISPS(play->state.gfxCtx);
    Gfx_SetupDL_42Opa(play->state.gfxCtx);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);

    if (dungeonPage) {
        // a row per floor, right of the floor buttons (x -88, 24 wide), where the floor rows are:
        // Link's own head goes at y = offsetY - floor * 14 + 49, 16 tall (KaleidoScope_DrawDungeonMap)
        int skullFloor = -1;
        if (CHECK_DUNGEON_ITEM(DUNGEON_COMPASS, gSaveContext.mapIndex) &&
            gMapData->skullFloorIconY[gSaveContext.mapIndex] != -99) {
            // the boss skull sits at x -62 on its floor's row: start after it there
            skullFloor = static_cast<int>(std::lround((49 - gMapData->skullFloorIconY[gSaveContext.mapIndex]) / 14.0f));
        }
        for (int floor = 0; floor < 8; floor++) {
            std::vector<const Mark*> row;
            for (const Mark& m : marks) {
                if (m.floor == floor) {
                    row.push_back(&m);
                }
            }
            if (row.empty()) {
                continue;
            }
            float top = pauseCtx->offsetY - floor * 14 + 49 - (16 - kHead) / 2.0f;
            DrawRow(play, row, floor == skullFloor ? -45.0f : -63.0f, top, pageAlpha, names);
        }
    } else {
        bool mirrored = CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0) != 0;
        for (int area = 0; area < kAreaCount; area++) {
            std::vector<const Mark*> row;
            float width = 0.0f;
            for (const Mark& m : marks) {
                if (m.area == area) {
                    row.push_back(&m);
                    width += SlotWidth(m, names);
                }
            }
            if (row.empty()) {
                continue;
            }
            // middle of the area box. The game flips the whole world map with a -1 x scale in
            // mirror mode; we keep the page matrix straight (so names read right) and flip x ourselves
            float centerX = kAreaBoxPosX[area] + kAreaBoxWidths[area] / 2.0f;
            if (mirrored) {
                centerX = -centerX;
            }
            float centerY = kAreaBoxPosY[area] + pauseCtx->offsetY - kAreaBoxHeights[area] / 2.0f;
            DrawRow(play, row, centerX - width / 2.0f, centerY + kHead / 2.0f, pageAlpha, names);
        }
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

namespace {

void DrawMenu() {
    ImGui::SeparatorText("Map screen");
    UIWidgets::CVarCheckbox("Show players on the map", MAP_FULL,
                            UIWidgets::CheckboxOptions()
                                .DefaultValue(false)
                                .Tooltip("Their face where they are, on the field map. Players inside a building "
                                         "or a dungeon are not on it.")
                                .Color(THEME_COLOR));
    UIWidgets::CVarCheckbox(
        "Show players on dungeon maps", MAP_DUNGEON,
        UIWidgets::CheckboxOptions()
            .DefaultValue(false)
            .Tooltip("Everybody in the same dungeon, next to the floor they are on.")
            .Color(THEME_COLOR));
    UIWidgets::CVarCheckbox(
        "Their names", MAP_NAMES,
        UIWidgets::CheckboxOptions().DefaultValue(true).Tooltip("Under each face.").Color(THEME_COLOR));
    ImGui::BeginDisabled(!CVarGetInteger(MAP_NAMES, 1));
    UIWidgets::CVarSliderInt(
        "Name size", MAP_NAME_SIZE,
        UIWidgets::IntSliderOptions().Min(50).Max(200).Step(10).DefaultValue(100).Format("%d%%").Color(THEME_COLOR));
    ImGui::EndDisabled();
}

void Register() {
    COND_VB_SHOULD(VB_DRAW_SAVE_MENU, true, {
        // we don't change the answer, it's only our "all pages are drawn" moment
        DrawMapScreenMarkers();
    });
    CoopGame::AddMenuSection("Map screen", CoopGame::MenuWhere::Always, DrawMenu);
}

} // namespace

static RegisterShipInitFunc sInit(Register);
