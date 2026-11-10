// Teammates as arrows on the minimap, in their color. Copied from Anchor
// (soh/Network/Anchor/HookHandlers.cpp, OnMinimapDrawCompassIcons): same
// drawing code, only the "who's where" comes from our puppets. Who shows up
// follows the room's "Show Locations" setting and teams, like Anchor.

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/cosmetics/cosmeticsTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/OTRGlobals.h"
#include "soh/cvar_prefixes.h"
#include "soh/frame_interpolation.h"

#include <libultraship/bridge/consolevariablebridge.h>

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

// Room settings, they live in CoopRoom.cpp (Anchor's roomState / TeamId)
namespace {

struct CompassIcon {
    Vec3f pos;
    Vec3s rot;
    float scale;
    Color_RGB8 color;
};

bool IsDungeon(int16_t scene) {
    return scene == SCENE_DEKU_TREE || scene == SCENE_DODONGOS_CAVERN || scene == SCENE_JABU_JABU ||
           scene == SCENE_FOREST_TEMPLE || scene == SCENE_FIRE_TEMPLE || scene == SCENE_WATER_TEMPLE ||
           scene == SCENE_SPIRIT_TEMPLE || scene == SCENE_SHADOW_TEMPLE || scene == SCENE_BOTTOM_OF_THE_WELL ||
           scene == SCENE_ICE_CAVERN;
}

} // namespace

// OPEN_DISPS declares FrameInterpolation_* again inside the function. That only
// stays extern "C" if frame_interpolation.h came first and we're not in a namespace.
static void DrawCompassIcons() {
    if (!CVarGetInteger(CVAR_COOP("ShowOnMinimap"), 1)) {
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

    std::vector<CompassIcon> compassIcons;
    bool isInDungeon = IsDungeon(gPlayState->sceneNum);

    // When going through a door, curRoom changes right away but the minimap still
    // shows the previous room while fading out
    s8 displayedRoomNum =
        gPlayState->roomCtx.prevRoom.num >= 0 ? gPlayState->roomCtx.prevRoom.num : gPlayState->roomCtx.curRoom.num;

    {
        std::lock_guard lock(CoopGame::Mutex());
        for (Actor* puppet : CoopGame::VisiblePuppets()) {
            const Coop::RemotePlayer* p = CoopGame::PuppetOwner(puppet);
            if (p == nullptr || !p->state) {
                continue;
            }
            // in dungeons, only teammates in the room shown on the map
            if (isInDungeon && p->state->roomNum != displayedRoomNum) {
                continue;
            }
            // not "everyone": only my team (Anchor HookHandlers.cpp OnMinimapDrawCompassIcons)
            if (showLocationsMode != 2 && p->state->team != team) {
                continue;
            }
            compassIcons.push_back(
                CompassIcon{ puppet->world.pos, puppet->shape.rot, 0.3f, CoopGame::PlayerColor(*p) });
        }
    }
    if (compassIcons.empty()) {
        return; // the game already drew our own arrow
    }

    // ours last so it's drawn on top of the others
    Player* player = GET_PLAYER(gPlayState);
    compassIcons.push_back(
        CompassIcon{ player->actor.world.pos, player->actor.shape.rot, 0.4f, CoopGame::OwnColor() });

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

    s16 mapWidth = isInDungeon ? R_DGN_MINIMAP_X : R_OW_MINIMAP_X;
    s16 mapStartPosX = isInDungeon ? 96 : gMapData->owMinimapWidth[R_MAP_INDEX];
    bool mirrored = CVarGetInteger(CVAR_ENHANCEMENT("MirroredWorld"), 0);

    OPEN_DISPS(gPlayState->state.gfxCtx);
    Gfx_SetupDL_42Overlay(gPlayState->state.gfxCtx);

    for (auto& compassIcon : compassIcons) {
        gSPMatrix(OVERLAY_DISP++, &gMtxClear, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gDPSetCombineLERP(OVERLAY_DISP++, PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0,
                          PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, PRIMITIVE, 0);
        gDPSetEnvColor(OVERLAY_DISP++, 0, 0, 0, 255);
        gDPSetCombineMode(OVERLAY_DISP++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);

        // compass offsets are in tenths of N64 pixels from the screen center
        s16 mirrorOffset = ((mapWidth / 2) - ((R_COMPASS_OFFSET_X / 10) - (mapStartPosX - SCREEN_WIDTH / 2))) * 2 * 10;

        s16 tempX = (s16)compassIcon.pos.x;
        s16 tempZ = (s16)compassIcon.pos.z;
        tempX /= R_COMPASS_SCALE_X * (mirrored ? -1 : 1);
        tempZ /= R_COMPASS_SCALE_Y;

        s16 tempXOffset = R_COMPASS_OFFSET_X + (mirrored ? mirrorOffset : 0);
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
        s16 rotation = ((0x7FFF - compassIcon.rot.y) / 0x400) * (mirrored ? -1 : 1);
        Matrix_RotateY(rotation / 10.0f, MTXMODE_APPLY);
        gSPMatrix(OVERLAY_DISP++, MATRIX_NEWMTX(gPlayState->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

        gDPSetPrimColor(OVERLAY_DISP++, 0, 0xFF, compassIcon.color.r, compassIcon.color.g, compassIcon.color.b, 255);
        gSPDisplayList(OVERLAY_DISP++, (Gfx*)gCompassArrowDL);
    }

    CLOSE_DISPS(gPlayState->state.gfxCtx);
}

namespace CoopGame {

void RegisterMinimapHooks() {
    COND_HOOK(OnMinimapDrawCompassIcons, true, DrawCompassIcons);
}

} // namespace CoopGame
