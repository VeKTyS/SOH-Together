// Teammates as arrows on the minimap, in their color. Copied from Anchor
// (soh/Network/Anchor/HookHandlers.cpp, OnMinimapDrawCompassIcons): same
// drawing code, only the "who's where" comes from our puppets. Who shows up
// follows the room's "Show Locations" setting and teams, like Anchor.
//
// On top of that, the minimap options of Crests of Courage (map_markers.cpp, on_meter_map_draw_post):
// - "Marker size": how big their arrows are
// - "Keep far players at the edge": somebody off the minimap (or in another room of the dungeon,
//   OoT's dungeon minimap only shows the room you're in) stays on its border, on their side,
//   pointing at them
// - "Show players on other floors": in dungeons, the ones upstairs or down are faded (or hidden)
// - "Colours": each their own or all white
// CoC also shows players in the next area over (TP's field map knows where areas are next to
// each other). OoT's scenes don't share coordinates, so that part has nothing to port.

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/cosmetics/cosmeticsTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/UIWidgets.hpp"
#include "soh/cvar_prefixes.h"
#include "soh/frame_interpolation.h"

#include <libultraship/bridge/consolevariablebridge.h>

#include <algorithm>
#include <cmath>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
extern PlayState* gPlayState;
extern MapData* gMapData;
float OTRGetDimensionFromLeftEdge(float v);  // OTRGlobals.h only declares them for C files
float OTRGetDimensionFromRightEdge(float v);
}

// settings, CoC map_markers_init() names and defaults
#define MINIMAP_ON CVAR_COOP("ShowOnMinimap")          // minimap_players (ours was already on by default)
#define MINIMAP_SIZE CVAR_COOP("Minimap.MarkerSize")   // minimap_arrow_size, 100 %
#define MINIMAP_EDGE CVAR_COOP("Minimap.Edge")         // minimap_players_edge, on
#define MAP_OTHER_FLOORS CVAR_COOP("Map.OtherFloors")  // map_players_other_floors, on (shared with CoopMapScreen)
#define MAP_COLORS CVAR_COOP("Map.Colors")             // map_player_colors, 0 = each their own, 1 = all white

// Room settings, they live in CoopRoom.cpp (Anchor's roomState / TeamId)
namespace {

struct CompassIcon {
    s16 x; // Anchor's tempX / tempZ: world position divided by the compass scale
    s16 z;
    s16 rotY;
    float scale;
    Color_RGB8 color;
    u8 alpha;
};

bool IsDungeon(int16_t scene) {
    return scene == SCENE_DEKU_TREE || scene == SCENE_DODONGOS_CAVERN || scene == SCENE_JABU_JABU ||
           scene == SCENE_FOREST_TEMPLE || scene == SCENE_FIRE_TEMPLE || scene == SCENE_WATER_TEMPLE ||
           scene == SCENE_SPIRIT_TEMPLE || scene == SCENE_SHADOW_TEMPLE || scene == SCENE_BOTTOM_OF_THE_WELL ||
           scene == SCENE_ICE_CAVERN;
}

// The floor of a height in this dungeon, same loop as Map_Update (z_map_exp.c) for VREG(30)
int FloorOf(int mapIndex, float y) {
    int floor = 0;
    for (; floor < 8; floor++) {
        if (y > gMapData->floorCoordY[mapIndex][floor]) {
            break;
        }
    }
    return std::min(floor, 7);
}

} // namespace

// OPEN_DISPS declares FrameInterpolation_* again inside the function. That only
// stays extern "C" if frame_interpolation.h came first and we're not in a namespace.
static void DrawCompassIcons() {
    if (!CVarGetInteger(MINIMAP_ON, 1) || gMapData == nullptr) {
        return;
    }
    Coop::SessionRole role = CoopGame::Session().Role();
    if (role != Coop::SessionRole::Hosting && role != Coop::SessionRole::Connected) {
        return;
    }

    // Anchor: the room's "Show Locations" mode, 0 = nobody, 1 = my team, 2 = everyone
    uint8_t showLocationsMode;
    {
        std::lock_guard lock(CoopGame::Mutex());
        showLocationsMode = CoopGame::RoomShowLocationsMode();
    }
    if (showLocationsMode == 0) {
        return;
    }
    uint8_t team = CoopGame::OwnTeam();

    // CoC options
    float sizeScale = std::clamp(CVarGetInteger(MINIMAP_SIZE, 100), 50, 250) / 100.0f;
    bool edge = CVarGetInteger(MINIMAP_EDGE, 1) != 0;
    bool otherFloors = CVarGetInteger(MAP_OTHER_FLOORS, 1) != 0;
    bool allWhite = CVarGetInteger(MAP_COLORS, 0) == 1;

    bool isInDungeon = IsDungeon(gPlayState->sceneNum);
    bool mirrored = CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0);

    // where the minimap is, same numbers Minimap_Draw uses (before the cosmetic moves/margins,
    // those move the map and the arrows the same way so we can ignore them here)
    s16 mapStartPosX = isInDungeon ? R_DGN_MINIMAP_X : R_OW_MINIMAP_X;
    s16 mapStartPosY = isInDungeon ? R_DGN_MINIMAP_Y : R_OW_MINIMAP_Y;
    s16 mapWidth = isInDungeon ? 96 : gMapData->owMinimapWidth[R_MAP_INDEX];
    s16 mapHeight = isInDungeon ? 85 : gMapData->owMinimapHeight[R_MAP_INDEX];

    // compass offsets are in tenths of N64 pixels from the screen center.
    // (Anchor had mapWidth / mapStartPosX swapped here, this is Minimap_DrawCompassIcons' version)
    s16 mirrorOffset = ((mapWidth / 2) - ((R_COMPASS_OFFSET_X / 10) - (mapStartPosX - SCREEN_WIDTH / 2))) * 2 * 10;
    s16 tempXOffset = R_COMPASS_OFFSET_X + (mirrored ? mirrorOffset : 0);

    // the minimap box in the same units as the arrows (pixels from the screen center, y going up)
    float boxLeft = static_cast<float>(mapStartPosX - SCREEN_WIDTH / 2);
    float boxRight = boxLeft + mapWidth;
    float boxTop = static_cast<float>(SCREEN_HEIGHT / 2 - mapStartPosY);
    float boxBottom = boxTop - mapHeight;

    auto toIcon = [&](const Vec3f& pos, s16 rotY, float scale, Color_RGB8 color, float alpha) {
        s16 tempX = (s16)pos.x;
        s16 tempZ = (s16)pos.z;
        tempX /= R_COMPASS_SCALE_X * (mirrored ? -1 : 1);
        tempZ /= R_COMPASS_SCALE_Y;
        return CompassIcon{ tempX, tempZ, rotY, scale, color, static_cast<u8>(std::clamp(alpha, 0.0f, 1.0f) * 255) };
    };

    std::vector<CompassIcon> compassIcons;

    // When going through a door, curRoom changes right away but the minimap still
    // shows the previous room while fading out
    s8 displayedRoomNum =
        gPlayState->roomCtx.prevRoom.num >= 0 ? gPlayState->roomCtx.prevRoom.num : gPlayState->roomCtx.curRoom.num;

    Player* player = GET_PLAYER(gPlayState);
    int localFloor = isInDungeon ? FloorOf(gSaveContext.mapIndex, player->actor.world.pos.y) : 0;

    {
        std::lock_guard lock(CoopGame::Mutex());
        for (Actor* puppet : CoopGame::VisiblePuppets()) {
            const Coop::RemotePlayer* p = CoopGame::PuppetOwner(puppet);
            if (p == nullptr || !p->state) {
                continue;
            }
            // not "everyone": only my team (Anchor HookHandlers.cpp OnMinimapDrawCompassIcons)
            if (showLocationsMode != 2 && p->state->team != team) {
                continue;
            }
            float alpha = 1.0f;
            // CoC: upstairs or down = faded, or not shown
            if (isInDungeon && FloorOf(gSaveContext.mapIndex, puppet->world.pos.y) != localFloor) {
                if (!otherFloors) {
                    continue;
                }
                alpha *= 0.45f;
            }
            // Anchor only showed teammates in the room on the dungeon minimap. The others aren't on
            // this room's picture, so they go to the edge (or nowhere, like before, without "edge").
            bool otherRoom = isInDungeon && p->state->roomNum != displayedRoomNum;
            if (otherRoom && !edge) {
                continue;
            }

            Color_RGB8 color = allWhite ? Color_RGB8{ 255, 255, 255 } : CoopGame::PlayerColor(*p);
            CompassIcon icon = toIcon(puppet->world.pos, puppet->shape.rot.y, 0.3f * sizeScale, color, alpha);

            // CoC on_meter_map_draw_post "outside" / edge part, in the arrows' units
            float x = (tempXOffset + icon.x) / 10.0f;
            float y = (R_COMPASS_OFFSET_Y - icon.z) / 10.0f;
            float inset = 3.0f * sizeScale + 1.0f; // CoC: size * 0.7, keeps the whole arrow on the map
            bool outside = x < boxLeft + inset || x > boxRight - inset || y > boxTop - inset ||
                           y < boxBottom + inset;
            if (outside || otherRoom) {
                if (!edge) {
                    continue;
                }
                float centerX = (boxLeft + boxRight) / 2.0f;
                float centerY = (boxTop + boxBottom) / 2.0f;
                float ox = x - centerX;
                float oy = y - centerY;
                float len = std::sqrt(ox * ox + oy * oy);
                if (len < 0.001f) {
                    continue;
                }
                if (outside) {
                    // CoC: clamp onto the box
                    x = std::clamp(x, boxLeft + inset, boxRight - inset);
                    y = std::clamp(y, boxBottom + inset, boxTop - inset);
                } else {
                    // another room but inside the box: push it out to the border along the same line
                    float halfW = std::max(mapWidth / 2.0f - inset, 0.0f);
                    float halfH = std::max(mapHeight / 2.0f - inset, 0.0f);
                    float t = std::min(std::fabs(ox) > 0.001f ? halfW / std::fabs(ox) : 1e9f,
                                       std::fabs(oy) > 0.001f ? halfH / std::fabs(oy) : 1e9f);
                    x = centerX + ox * t;
                    y = centerY + oy * t;
                }
                // the arrow points at them (CoC computes this direction too). World x goes right on
                // the map (left when mirrored), world z goes down.
                float worldDx = ox * (mirrored ? -1.0f : 1.0f);
                float worldDz = -oy;
                icon.rotY = static_cast<s16>(std::atan2(worldDx, worldDz) * (32768.0f / 3.14159265f));
                icon.x = static_cast<s16>(std::lround(x * 10.0f - tempXOffset));
                icon.z = static_cast<s16>(std::lround(R_COMPASS_OFFSET_Y - y * 10.0f));
                icon.alpha = static_cast<u8>(icon.alpha * 0.8f);
            }
            compassIcons.push_back(icon);
        }
    }
    if (compassIcons.empty()) {
        return; // the game already drew our own arrow
    }

    // ours last so it's drawn on top of the others
    compassIcons.push_back(
        toIcon(player->actor.world.pos, player->actor.shape.rot.y, 0.4f, CoopGame::OwnColor(), 1.0f));

    // Adapted internals of Minimap_DrawCompassIcons() (same as Anchor)
    s16 leftMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.L"), 0);
    s16 rightMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.R"), 0);
    s16 bottomMinimapMargin = CVarGetInteger(CVAR_COSMETIC("HUD.Margin.B"), 0);

    s16 xMarginsMinimap = 0;
    s16 yMarginsMinimap = 0;
    if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
        if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0) == ORIGINAL_LOCATION) {
            xMarginsMinimap = rightMinimapMargin;
        }
        yMarginsMinimap = bottomMinimapMargin;
    }

    OPEN_DISPS(gPlayState->state.gfxCtx);
    Gfx_SetupDL_42Overlay(gPlayState->state.gfxCtx);

    for (auto& compassIcon : compassIcons) {
        gSPMatrix(OVERLAY_DISP++, &gMtxClear, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                          PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
        gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 255);
        gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);

        s16 tempX = compassIcon.x;
        s16 tempZ = compassIcon.z;

        s32 posType = CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosType"), 0);
        s32 posX = CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosX"), 0);
        s32 posY = CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.PosY"), 0);
        if (posType != ORIGINAL_LOCATION) {
            if (posType == ANCHOR_LEFT) {
                if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
                    xMarginsMinimap = leftMinimapMargin;
                }
                Matrix_Translate(
                    OTRGetDimensionFromLeftEdge((tempXOffset + (xMarginsMinimap * 10) + tempX + (posX * 10)) / 10.0f),
                    (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ + ((posY * 10) * -1)) / 10.0f, 0.0f,
                    MTXMODE_NEW);
            } else if (posType == ANCHOR_RIGHT) {
                if (CVarGetInteger(CVAR_COSMETIC("HUD.Minimap.UseMargins"), 0) != 0) {
                    xMarginsMinimap = rightMinimapMargin;
                }
                Matrix_Translate(
                    OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX + (posX * 10)) / 10.0f),
                    (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ + ((posY * 10) * -1)) / 10.0f, 0.0f,
                    MTXMODE_NEW);
            } else if (posType == ANCHOR_NONE) {
                Matrix_Translate((tempXOffset + tempX + (posX * 10) / 10.0f),
                                 (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ + ((posY * 10) * -1)) /
                                     10.0f,
                                 0.0f, MTXMODE_NEW);
            }
        } else {
            Matrix_Translate(OTRGetDimensionFromRightEdge((tempXOffset + (xMarginsMinimap * 10) + tempX) / 10.0f),
                             (R_COMPASS_OFFSET_Y + ((yMarginsMinimap * 10) * -1) - tempZ) / 10.0f, 0.0f, MTXMODE_NEW);
        }
        Matrix_Scale(compassIcon.scale, compassIcon.scale, compassIcon.scale, MTXMODE_APPLY);
        Matrix_RotateX(-1.6f, MTXMODE_APPLY);
        s16 rotation = ((0x7FFF - compassIcon.rotY) / 0x400) * (mirrored ? -1 : 1);
        Matrix_RotateY(rotation / 10.0f, MTXMODE_APPLY);
        gSPMatrix(OVERLAY_DISP++, MATRIX_NEWMTX(gPlayState->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

        gDPSetPrimColor(OVERLAY_DISP++, 0, 0xFF, compassIcon.color.r, compassIcon.color.g, compassIcon.color.b,
                        compassIcon.alpha);
        gSPDisplayList(OVERLAY_DISP++, (Gfx*)gCompassArrowDL);
    }

    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

namespace {

// CoC menu.cpp group_map, "Minimap" and "Both" (the "Map screen" part is in CoopMapScreen.cpp)
void DrawMinimapMenu() {
    ImGui::SeparatorText("Minimap");
    UIWidgets::CVarCheckbox("Show players on the minimap", MINIMAP_ON,
                            UIWidgets::CheckboxOptions()
                                .DefaultValue(true)
                                .Tooltip("An arrow for each player in the same area, in their colour.")
                                .Color(THEME_COLOR));
    ImGui::BeginDisabled(!CVarGetInteger(MINIMAP_ON, 1));
    UIWidgets::CVarSliderInt(
        "Marker size", MINIMAP_SIZE,
        UIWidgets::IntSliderOptions().Min(50).Max(250).Step(10).DefaultValue(100).Format("%d%%").Color(THEME_COLOR));
    UIWidgets::CVarCheckbox(
        "Keep far players at the edge", MINIMAP_EDGE,
        UIWidgets::CheckboxOptions()
            .DefaultValue(true)
            .Tooltip("Somebody off the edge of the minimap stays on its border, on their side.")
            .Color(THEME_COLOR));
    ImGui::EndDisabled();

    ImGui::SeparatorText("Both");
    UIWidgets::CVarCheckbox(
        "Show players on other floors", MAP_OTHER_FLOORS,
        UIWidgets::CheckboxOptions()
            .DefaultValue(true)
            .Tooltip("Faded, so you can tell they are upstairs or down.")
            .Color(THEME_COLOR));
    static const char* kColors[] = { "Each their own", "All white" };
    UIWidgets::CVarCombobox("Colours", MAP_COLORS, kColors,
                            UIWidgets::ComboboxOptions()
                                .DefaultIndex(0)
                                .Tooltip("Each player gets a colour for their name and arrow, so you can tell "
                                         "who is who.")
                                .Color(THEME_COLOR));
}

} // namespace

namespace CoopGame {

void RegisterMinimapHooks() {
    COND_HOOK(OnMinimapDrawCompassIcons, true, DrawCompassIcons);
    AddMenuSection("Minimap", MenuWhere::Always, DrawMinimapMenu);
}

} // namespace CoopGame
