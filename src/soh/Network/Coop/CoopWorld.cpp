// Make things already on screen react when a teammate's flag arrives.
//
// Flags from the network are written straight into the save, but actors that
// are already loaded only read their flag when they spawn. So a wall your
// friend bombed stays there until you reload the room. These hooks poke the
// common ones (bomb walls, webs, locked doors, boulders, ice...) so they react
// right away.
//
// The list and the tricks come from SoH's Anchor (Network/Anchor/HookHandlers.cpp),
// they depend on each actor's internals so there's no other way to write them.
// Left out on purpose:
// - EnItem00: Anchor removes any floor item whose flag is set, that would also
//   eat your own pot/enemy loot (loot is per player here, see CoopFlags.cpp)

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Bg_Bombwall/z_bg_bombwall.h"
#include "src/overlays/actors/ovl_Bg_Breakwall/z_bg_breakwall.h"
#include "src/overlays/actors/ovl_Bg_Haka_Zou/z_bg_haka_zou.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hamstep/z_bg_hidan_hamstep.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Hrock/z_bg_hidan_hrock.h"
#include "src/overlays/actors/ovl_Bg_Ice_Shelter/z_bg_ice_shelter.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombchuiwa/z_bg_jya_bombchuiwa.h"
#include "src/overlays/actors/ovl_Bg_Jya_Bombiwa/z_bg_jya_bombiwa.h"
#include "src/overlays/actors/ovl_Bg_Mizu_Bwall/z_bg_mizu_bwall.h"
#include "src/overlays/actors/ovl_Bg_Spot08_Bakudankabe/z_bg_spot08_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot11_Bakudankabe/z_bg_spot11_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Spot17_Bakudankabe/z_bg_spot17_bakudankabe.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Maruta/z_bg_ydan_maruta.h"
#include "src/overlays/actors/ovl_Bg_Ydan_Sp/z_bg_ydan_sp.h"
#include "src/overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "src/overlays/actors/ovl_En_Door/z_en_door.h"
#include "src/overlays/actors/ovl_En_Si/z_en_si.h"
#include "src/overlays/actors/ovl_En_Sw/z_en_sw.h"
#include "src/overlays/actors/ovl_Item_B_Heart/z_item_b_heart.h"
#include "src/overlays/actors/ovl_Obj_Bombiwa/z_obj_bombiwa.h"
#include "src/overlays/actors/ovl_Obj_Hamishi/z_obj_hamishi.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Dalm/z_bg_hidan_dalm.h"
#include "src/overlays/actors/ovl_Bg_Hidan_Kowarerukabe/z_bg_hidan_kowarerukabe.h"
extern PlayState* gPlayState;
// these live in the actors' .c files without a header, same list as Anchor
void func_8086ED70(BgBombwall* bgBombwall, PlayState* play);
void BgBreakwall_Wait(BgBreakwall* bgBreakwall, PlayState* play);
void BgHakaZou_WaitForHit(BgHakaZou* bgHakaZou, PlayState* play);
void func_808887C4(BgHidanHamstep* bgHidanHamstep, PlayState* play);
void func_808896B8(BgHidanHrock* bgHidanHrock, PlayState* play);
void BgIceShelter_Idle(BgIceShelter* bgIceShelter, PlayState* play);
void BgIceShelter_SetupMelt(BgIceShelter* bgIceShelter);
void ObjBombiwa_Break(ObjBombiwa* objBombiwa, PlayState* play);
void ObjHamishi_Break(ObjHamishi* objHamishi, PlayState* play);
void BgJyaBombchuiwa_WaitForExplosion(BgJyaBombchuiwa* bgJyaBombchuiwa, PlayState* play);
void BgMizuBwall_Idle(BgMizuBwall* bgMizuBwall, PlayState* play);
void func_808B6BC0(BgSpot17Bakudankabe* bgSpot17Bakudankabe, PlayState* play);
void func_808BF078(BgYdanMaruta* bgYdanMaruta, PlayState* play);
void BgYdanSp_FloorWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_WallWebIdle(BgYdanSp* bgYdanSp, PlayState* play);
void BgYdanSp_BurnWeb(BgYdanSp* bgYdanSp, PlayState* play);
void EnDoor_Idle(EnDoor* enDoor, PlayState* play);
}

namespace {

// Only while playing co-op: solo games must behave exactly like normal SoH.
// Read without the session lock on purpose (one enum read from the game
// thread, the worst case is reacting one frame late).
bool Active() {
    Coop::SessionRole role = CoopGame::Session().Role();
    return gPlayState != nullptr && (role == Coop::SessionRole::Hosting || role == Coop::SessionRole::Connected);
}

bool Switch(s32 flag) {
    return Flags_GetSwitch(gPlayState, flag);
}

// "pretend it just got hit by a bomb / hammer", the actor's own update does the rest
void FakeHit(Collider& collider) {
    collider.acFlags |= AC_HIT;
}

} // namespace

namespace CoopGame {

void RegisterWorldHooks() {
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BOMBWALL, true, [](void* ref, bool*) {
        BgBombwall* a = static_cast<BgBombwall*>(ref);
        if (Active() && a->actionFunc == func_8086ED70 && Switch(a->dyna.actor.params & 0x3F)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_BREAKWALL, true, [](void* ref, bool*) {
        BgBreakwall* a = static_cast<BgBreakwall*>(ref);
        if (Active() && a->actionFunc == BgBreakwall_Wait && Switch(a->dyna.actor.params & 0x3F)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HAKA_ZOU, true, [](void* ref, bool*) {
        BgHakaZou* a = static_cast<BgHakaZou*>(ref);
        if (Active() && a->actionFunc == BgHakaZou_WaitForHit && Switch(a->switchFlag)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HAMSTEP, true, [](void* ref, bool*) {
        BgHidanHamstep* a = static_cast<BgHidanHamstep*>(ref);
        if (Active() && a->actionFunc == func_808887C4 && Switch((a->dyna.actor.params >> 8) & 0xFF)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_HIDAN_HROCK, true, [](void* ref, bool*) {
        BgHidanHrock* a = static_cast<BgHidanHrock*>(ref);
        if (Active() && a->actionFunc == func_808896B8 && Switch(a->unk_16A)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_ICE_SHELTER, true, [](void* ref, bool*) {
        BgIceShelter* a = static_cast<BgIceShelter*>(ref);
        if (Active() && a->actionFunc == BgIceShelter_Idle && Switch(a->dyna.actor.params & 0x3F)) {
            BgIceShelter_SetupMelt(a);
            Audio_PlayActorSound2(&a->dyna.actor, NA_SE_EV_ICE_MELT);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBCHUIWA, true, [](void* ref, bool*) {
        BgJyaBombchuiwa* a = static_cast<BgJyaBombchuiwa*>(ref);
        if (Active() && a->actionFunc == BgJyaBombchuiwa_WaitForExplosion && Switch(a->actor.params & 0x3F)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_JYA_BOMBIWA, true, [](void* ref, bool*) {
        BgJyaBombiwa* a = static_cast<BgJyaBombiwa*>(ref);
        if (Active() && Switch(a->dyna.actor.params & 0x3F)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_MIZU_BWALL, true, [](void* ref, bool*) {
        BgMizuBwall* a = static_cast<BgMizuBwall*>(ref);
        if (Active() && a->actionFunc == BgMizuBwall_Idle && Switch(((u16)a->dyna.actor.params >> 8) & 0x3F)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT08_BAKUDANKABE, true, [](void* ref, bool*) {
        BgSpot08Bakudankabe* a = static_cast<BgSpot08Bakudankabe*>(ref);
        if (Active() && Switch(a->dyna.actor.params & 0x3F)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT11_BAKUDANKABE, true, [](void* ref, bool*) {
        BgSpot11Bakudankabe* a = static_cast<BgSpot11Bakudankabe*>(ref);
        if (Active() && Switch(a->dyna.actor.params & 0x3F)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_SPOT17_BAKUDANKABE, true, [](void* ref, bool* should) {
        BgSpot17Bakudankabe* a = static_cast<BgSpot17Bakudankabe*>(ref);
        if (Active() && Switch(a->dyna.actor.params & 0x3F)) {
            func_808B6BC0(a, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &a->dyna.actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
            Actor_Kill(&a->dyna.actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_MARUTA, true, [](void* ref, bool*) {
        BgYdanMaruta* a = static_cast<BgYdanMaruta*>(ref);
        if (Active() && a->actionFunc == func_808BF078 && Switch(a->switchFlag)) {
            FakeHit(a->collider.base);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_BG_YDAN_SP, true, [](void* ref, bool*) {
        BgYdanSp* a = static_cast<BgYdanSp*>(ref);
        if (Active() && (a->actionFunc == BgYdanSp_FloorWebIdle || a->actionFunc == BgYdanSp_WallWebIdle) &&
            Switch(a->isDestroyedSwitchFlag)) {
            BgYdanSp_BurnWeb(a, gPlayState);
        }
    });

    // locked doors: count down their unlock timer once the key was used by someone
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_DOOR_SHUTTER, true, [](void* ref, bool*) {
        DoorShutter* a = static_cast<DoorShutter*>(ref);
        if (Active() && Switch(a->dyna.actor.params & 0x3F)) {
            DECR(a->unlockTimer);
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_DOOR, true, [](void* ref, bool*) {
        EnDoor* a = static_cast<EnDoor*>(ref);
        if (Active() && a->actionFunc == EnDoor_Idle && Switch(a->actor.params & 0x3F)) {
            DECR(a->lockTimer);
        }
    });

    // gold skulltula / its token, when a teammate already got that token
    // (gsFlags come from CoopFlags.cpp's GsFlags event). Anchor HookHandlers.cpp
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SI, true, [](void* ref, bool* should) {
        EnSi* a = static_cast<EnSi*>(ref);
        if (Active() && GET_GS_FLAGS((a->actor.params & 0x1F00) >> 8) & (a->actor.params & 0xFF)) {
            Actor_Kill(&a->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_SW, true, [](void* ref, bool* should) {
        EnSw* a = static_cast<EnSw*>(ref);
        if (Active() && GET_GS_FLAGS((a->actor.params & 0x1F00) >> 8) & (a->actor.params & 0xFF)) {
            Actor_Kill(&a->actor);
            *should = false;
        }
    });

    // boss heart container already taken by someone
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_ITEM_B_HEART, true, [](void* ref, bool* should) {
        ItemBHeart* a = static_cast<ItemBHeart*>(ref);
        if (Active() && Flags_GetCollectible(gPlayState, 0x1F)) {
            Actor_Kill(&a->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_BOMBIWA, true, [](void* ref, bool* should) {
        ObjBombiwa* a = static_cast<ObjBombiwa*>(ref);
        if (Active() && Switch(a->actor.params & 0x3F)) {
            ObjBombiwa_Break(a, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &a->actor.world.pos, 80, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&a->actor);
            *should = false;
        }
    });

    COND_ID_HOOK(ShouldActorUpdate, ACTOR_OBJ_HAMISHI, true, [](void* ref, bool* should) {
        ObjHamishi* a = static_cast<ObjHamishi*>(ref);
        if (Active() && Switch(a->actor.params & 0x3F)) {
            ObjHamishi_Break(a, gPlayState);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &a->actor.world.pos, 40, NA_SE_EV_WALL_BROKEN);
            Actor_Kill(&a->actor);
            *should = false;
        }
    });

    COND_VB_SHOULD(VB_HAMMER_TOTEM_BREAK, true, {
        BgHidanDalm* a = va_arg(args, BgHidanDalm*);
        if (Active() && Switch(a->switchFlag)) {
            *should = true;
        }
    });

    COND_VB_SHOULD(VB_FIRE_TEMPLE_BOMBABLE_WALL_BREAK, true, {
        BgHidanKowarerukabe* a = va_arg(args, BgHidanKowarerukabe*);
        if (Active() && Switch((a->dyna.actor.params >> 8) & 0x3F)) {
            *should = true;
        }
    });
}

} // namespace CoopGame
