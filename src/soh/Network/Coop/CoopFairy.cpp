// A fairy following each teammate (TrueCoop PuppetFairy).
//
// Copied from TrueCoop soh/Network/Anchor/PuppetFairy.cpp. Only change: TrueCoop finds the
// teammate's Link with Anchor's clients[id].player, we find our puppet with CoopGame::PuppetOwner.
//
// One plain ACTOR_EN_ELF spawned with params FAIRY_KOKIRI (a simple "follow my parent" fairy,
// no Navi stuff, no talking, no C-up). It's spawned without a parent on purpose: the game's own
// follow kills the fairy when the parent dies, and puppets get killed/respawned (joins, age
// change), so a raw parent pointer could point to freed memory. Instead we stop the fairy's own
// update (ShouldActorUpdate) and move it ourselves, looking the puppet up again every frame.

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <unordered_map>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Elf/z_en_elf.h"
extern PlayState* gPlayState;
}

namespace {

// player id -> fairy. Only fairies WE spawned live here: Navi and every other En_Elf aren't
// in it, that's how the update override leaves them alone.
std::unordered_map<uint32_t, Actor*> sFairies;
// the PlayState the fairies above were spawned in (a new one = the old actors are gone)
PlayState* sFairiesPlayState = nullptr;

// true while we're inside Actor_Spawn for one of our fairies (see IsPuppetFairy)
bool sSpawning = false;

// Parked spot: puppets get moved to -9999 when they aren't in our scene, the fairy too.
constexpr f32 PARKED_COORD = -9999.0f;

// The puppet of that player (even a hidden one), nullptr if there's none. Session locked.
Actor* FindPuppet(uint32_t playerId) {
    if (gPlayState == nullptr) {
        return nullptr;
    }
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; a != nullptr; a = a->next) {
        const Coop::RemotePlayer* owner = CoopGame::PuppetOwner(a);
        if (owner != nullptr && owner->id == playerId && a->update != NULL) {
            return a;
        }
    }
    return nullptr;
}

// CLAMP a per-axis velocity target to +/-20, keeping the sign (same clamp as the game's
// follower, func_80A02C98 / func_80A02BD8).
f32 ClampFairyVel(f32 v) {
    f32 sign = (v >= 0.0f) ? 1.0f : -1.0f;
    v = fabsf(v);
    if (v > 20.0f) {
        v = 20.0f;
    }
    return v * sign;
}

void SnapFairyAway(EnElf* elf) {
    elf->actor.world.pos.x = PARKED_COORD;
    elf->actor.world.pos.y = PARKED_COORD;
    elf->actor.world.pos.z = PARKED_COORD;
    elf->actor.velocity.x = 0.0f;
    elf->actor.velocity.y = 0.0f;
    elf->actor.velocity.z = 0.0f;
}

// Our own follow for one fairy. Redoes z_en_elf.c's func_80A02A20 (hover bob) and
// func_80A02C98 / func_80A02BD8 (clamped velocity) toward the puppet instead of a parent.
// Session locked.
void DrivePuppetFairy(uint32_t playerId, Actor* actor) {
    EnElf* elf = (EnElf*)actor;

    // look the puppet up every frame, never keep a pointer to it (it can be respawned)
    Actor* puppet = FindPuppet(playerId);
    const Coop::RemotePlayer* client = puppet != nullptr ? CoopGame::PuppetOwner(puppet) : nullptr;

    // Parked: puppet in another scene / not in game / no pose yet (same test as Puppet_Update's
    // Hide) or hidden at -9999. SNAP away, a slide under the 20/frame clamp would streak the
    // fairy across the whole map. The frame tick kills it if the puppet is really gone.
    bool parked = (client == nullptr) || (gPlayState == nullptr) || !client->state || !client->pose ||
                  !client->state->inGame || (client->state->sceneNum != gPlayState->sceneNum) ||
                  (puppet->world.pos.y < -9000.0f);
    if (parked) {
        SnapFairyAway(elf);
        return;
    }

    // their color can change from the menu (or arrive after the fairy spawned): keep it up to date
    Color_RGB8 color = CoopGame::PlayerColor(*client);
    elf->outerColor.r = (f32)color.r;
    elf->outerColor.g = (f32)color.g;
    elf->outerColor.b = (f32)color.b;

    Vec3f puppetPos = puppet->world.pos;
    f32 hover = (1500.0f * elf->actor.scale.y) + 40.0f; // same as z_en_elf.c func_80A0353C

    // just came back from being parked: snap next to the puppet before following again
    // (otherwise it would slowly crawl back from -9999 at 20/frame)
    if (elf->actor.world.pos.x <= (PARKED_COORD + 1.0f)) {
        elf->actor.world.pos.x = puppetPos.x;
        elf->actor.world.pos.y = puppetPos.y + hover;
        elf->actor.world.pos.z = puppetPos.z;
        elf->actor.velocity.x = 0.0f;
        elf->actor.velocity.y = 0.0f;
        elf->actor.velocity.z = 0.0f;
    }

    SkelAnime_Update(&elf->skelAnime);

    // hover bob, func_80A02A20 with the values EnElf_Init set for FAIRY_KOKIRI
    elf->unk_28C.x = Math_SinS(elf->unk_2AC) * elf->unk_2B8;
    elf->unk_28C.y = Math_SinS(elf->unk_2AA) * elf->unk_2B4;
    elf->unk_28C.z = Math_CosS(elf->unk_2AC) * elf->unk_2B8;
    elf->unk_2AC += elf->unk_2B0;
    elf->unk_2AA += elf->unk_2AE;

    // follow target = puppet position + hover height
    Vec3f target = puppetPos;
    target.y += hover;

    // func_80A02C98 (xz) + func_80A02BD8 (y): velocity toward the target * 0.2, clamped to 20 per axis
    const f32 rate = 0.2f;
    f32 xVel = ClampFairyVel(((target.x + elf->unk_28C.x) - elf->actor.world.pos.x) * rate);
    f32 yVel = ClampFairyVel(((target.y + elf->unk_28C.y) - elf->actor.world.pos.y) * rate);
    f32 zVel = ClampFairyVel(((target.z + elf->unk_28C.z) - elf->actor.world.pos.z) * rate);

    Math_StepToF(&elf->actor.velocity.y, yVel, 32.0f);
    Math_StepToF(&elf->actor.velocity.x, xVel, 1.5f);
    Math_StepToF(&elf->actor.velocity.z, zVel, 1.5f);
    Actor_UpdatePos(&elf->actor);

    // face where it's going (end of func_80A0353C) and tick the timer the draw uses for the
    // shimmer. EnElf_Update normally does both, but we turned it off.
    elf->unk_2BC = Math_Atan2S(elf->actor.velocity.z, elf->actor.velocity.x);
    elf->actor.shape.rot.y = elf->unk_2BC;
    elf->timer++;

    // keep the two point lights on the fairy (EnElf_UpdateLights default case: glow radius 100,
    // no-glow radius -1, white)
    Lights_PointGlowSetInfo(&elf->lightInfoGlow, (s16)elf->actor.world.pos.x, (s16)elf->actor.world.pos.y,
                            (s16)elf->actor.world.pos.z, 255, 255, 255, 100);
    Lights_PointNoGlowSetInfo(&elf->lightInfoNoGlow, (s16)elf->actor.world.pos.x, (s16)elf->actor.world.pos.y,
                              (s16)elf->actor.world.pos.z, 255, 255, 255, -1);
}

void KillAllFairies() {
    for (auto& [playerId, fairy] : sFairies) {
        Actor_Kill(fairy);
    }
    sFairies.clear();
}

// Every frame (session locked): kill fairies whose puppet is gone, then spawn one for every
// puppet that doesn't have one. TrueCoop's PuppetFairyLifecycle.
void PuppetFairyLifecycle() {
    if (gPlayState == nullptr) {
        sFairies.clear(); // no play state = no actors anymore
        sFairiesPlayState = nullptr;
        return;
    }
    // a new PlayState: the old fairies died with the old one, forget the pointers
    if (gPlayState != sFairiesPlayState) {
        sFairies.clear();
        sFairiesPlayState = gPlayState;
    }
    if (!CVarGetInteger(CVAR_COOP("PuppetFairy"), 1)) {
        KillAllFairies();
        return;
    }

    // 1) reap. Erase the entry together with the kill so no stale pointer stays around.
    for (auto it = sFairies.begin(); it != sFairies.end();) {
        if (FindPuppet(it->first) == nullptr) {
            Actor_Kill(it->second);
            it = sFairies.erase(it);
        } else {
            ++it;
        }
    }

    // 2) spawn (TrueCoop skips this in Anchor's global room, we don't have one)
    std::vector<std::pair<uint32_t, Vec3f>> missing;
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; a != nullptr; a = a->next) {
        const Coop::RemotePlayer* owner = CoopGame::PuppetOwner(a);
        if (owner == nullptr || a->update == NULL || sFairies.count(owner->id)) {
            continue;
        }
        missing.push_back({ owner->id, a->world.pos });
    }
    // spawn after the loop: Actor_Spawn adds to the actor lists
    for (const auto& [playerId, pos] : missing) {
        sSpawning = true; // the fairy runs its init (and other files' init hooks) inside Actor_Spawn
        Actor* fairy = Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_ELF, pos.x, pos.y + 60.0f, pos.z, 0, 0,
                                   0, FAIRY_KOKIRI);
        sSpawning = false;
        if (fairy == nullptr) {
            continue;
        }

        // tint the outer (aura) color with the player's color, the inner core stays white
        const Coop::RemotePlayer* client = CoopGame::Session().FindById(playerId);
        if (client != nullptr) {
            Color_RGB8 color = CoopGame::PlayerColor(*client);
            EnElf* elf = (EnElf*)fairy;
            elf->outerColor.r = (f32)color.r;
            elf->outerColor.g = (f32)color.g;
            elf->outerColor.b = (f32)color.b;
        }

        sFairies[playerId] = fairy;
    }
}

void Register() {
    CoopGame::OnEveryFrame("Fairy.Lifecycle", PuppetFairyLifecycle);

    // Follow override: stop our fairies' own update and move them ourselves. Any En_Elf NOT
    // in sFairies (Navi, forest fairies, healing fairies) goes through untouched.
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_ELF, true, [](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;
        std::lock_guard lock(CoopGame::Mutex());
        for (auto& [playerId, fairy] : sFairies) {
            if (fairy == actor) {
                *should = false;
                uint32_t id = playerId; // copy: erase below would free the key we point to
                if (!CoopGame::SessionActive()) {
                    // session over (TrueCoop drops its map on disconnect and lets the fairy kill
                    // itself, it has no parent): we kill it right away instead
                    Actor_Kill(actor);
                    sFairies.erase(id);
                    return;
                }
                DrivePuppetFairy(id, actor);
                return;
            }
        }
    });

    // the scene going away frees the fairies: drop the entry the moment its actor is destroyed
    // so the follow override never touches a freed pointer
    COND_HOOK(OnActorDestroy, true, [](void* actorRef) {
        Actor* actor = (Actor*)actorRef;
        std::lock_guard lock(CoopGame::Mutex());
        for (auto it = sFairies.begin(); it != sFairies.end();) {
            if (it->second == actor) {
                it = sFairies.erase(it);
            } else {
                ++it;
            }
        }
    });

    // belt and braces: a fresh scene starts with an empty map
    COND_HOOK(OnSceneInit, true, [](int16_t sceneNum) {
        std::lock_guard lock(CoopGame::Mutex());
        sFairies.clear();
    });
}

} // namespace

namespace CoopGame {

// True for the fairies we spawn next to puppets. Other sync code (enemies, spawns, things)
// should skip them: they only exist on this screen.
bool IsPuppetFairy(::Actor* actor) {
    std::lock_guard lock(Mutex());
    if (sSpawning && actor != nullptr && actor->id == ACTOR_EN_ELF) {
        return true; // being spawned right now, not in the map yet
    }
    for (auto& [playerId, fairy] : sFairies) {
        if (fairy == actor) {
            return true;
        }
    }
    return false;
}

} // namespace CoopGame

static RegisterShipInitFunc sInit(Register);
