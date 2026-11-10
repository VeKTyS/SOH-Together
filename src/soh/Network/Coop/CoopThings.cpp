// Things players carry, throw, and their bombs.
//
// - a pot / rock / bush / crate someone picks up: everyone else's copy of it
//   follows it (it's from the room's actor list, so everyone has one). When it
//   breaks, the kill sync in CoopEnemies.cpp breaks it everywhere.
// - bombs and bombchus only exist in the game of whoever placed them. Others get
//   a look-alike ("ghost") that follows the real one, copies its memory (fuse
//   timer, flashing...), and when the real one is gone, the ghost goes boom.
//   The ghost's boom is just the visual + sound: the real bomb already did the
//   damage (walls go through flags, enemies through the host). Otherwise
//   everything would get hit twice.
//
// Each game sends the list of its things every frame (snapshot channel), the
// host passes it on to the others.

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ActorListIndex.h"

#include <spdlog/spdlog.h>

#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

namespace {

// --- our things ---

uint32_t sOwnerTag = 0;                       // random per game, identifies our list once relayed
std::unordered_map<Actor*, uint16_t> sOurIds; // our dynamic things (bombs) -> their number
uint16_t sNextId = 1;
std::unordered_map<Actor*, int> sFollowed; // props we picked up -> frames they've been still

// --- their things ---

struct OwnerThings {
    std::map<uint16_t, Actor*> ghosts; // their localId -> our look-alike
    int quietFrames = 0;               // frames since their last list
};
std::map<uint32_t, OwnerThings> sOwners;
std::set<Actor*> sGhosts;                         // every look-alike, any owner
std::unordered_map<Actor*, int> sMovedByTeammate; // room props a teammate holds -> frames since last move

// a ghost's own code never runs: it would explode / fall / get picked up on its own
constexpr int kTeammateHoldFrames = 5;
// a thrown prop stops being followed once it's been still this long
constexpr int kStillFrames = 20;
// an owner that went quiet this long left the scene (or the game): drop their ghosts
constexpr int kOwnerGoneFrames = 30;

bool SessionActive() {
    Coop::SessionRole role = CoopGame::Session().Role();
    return role == Coop::SessionRole::Hosting || role == Coop::SessionRole::Connected;
}

bool IsCarryable(Actor* a) {
    return GetActorListIndex(a) >= 0 && (CoopGame::IsBreakable(a->id) || a->id == ACTOR_EN_BOMBF);
}

Actor* FindListActor(int8_t room, int16_t index, int16_t actorId) {
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->room == room && a->id == actorId && GetActorListIndex(a) == index) {
                return a;
            }
        }
    }
    return nullptr;
}

Coop::ThingState Describe(Actor* a, uint16_t localId) {
    Coop::ThingState t;
    t.localId = localId;
    t.room = a->room;
    t.listIndex = GetActorListIndex(a);
    t.actorId = a->id;
    t.params = a->params;
    t.pos = { a->world.pos.x, a->world.pos.y, a->world.pos.z };
    t.rot = { a->shape.rot.x, a->shape.rot.y, a->shape.rot.z };
    return t;
}

void SendOurThings() {
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr && player->heldActor != nullptr && IsCarryable(player->heldActor)) {
        sFollowed[player->heldActor] = 0;
    }

    Coop::ThingStates out;
    out.owner = sOwnerTag;
    out.sceneNum = gPlayState->sceneNum;
    out.layer = static_cast<uint8_t>(gSaveContext.sceneLayer);

    // props we hold or threw: follow them until they settle down
    for (auto it = sFollowed.begin(); it != sFollowed.end();) {
        Actor* a = it->first;
        bool held = player != nullptr && player->heldActor == a;
        bool still = a->speedXZ == 0.0f && a->velocity.y == 0.0f;
        it->second = (held || !still) ? 0 : it->second + 1;
        if (a->update == nullptr || it->second > kStillFrames) {
            it = sFollowed.erase(it);
            continue;
        }
        if (out.things.size() < Coop::kMaxThings) {
            out.things.push_back(Describe(a, 0));
        }
        ++it;
    }

    // bombs, bombchus: ours only, not the ghosts of other players' bombs
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_EXPLOSIVE].head; a != nullptr; a = a->next) {
        if (sGhosts.contains(a) || a->update == nullptr || out.things.size() >= Coop::kMaxThings) {
            continue;
        }
        if (GetActorListIndex(a) >= 0) {
            continue; // bomb flowers in the level: only followed when picked up (above)
        }
        auto [it, isNew] = sOurIds.try_emplace(a, sNextId);
        if (isNew) {
            sNextId = sNextId == 0xFFFF ? 1 : sNextId + 1;
        }
        Coop::ThingState t = Describe(a, it->second);
        CoopGame::ReadActorBody(a, t.body, t.slotKinds);
        out.things.push_back(std::move(t));
    }
    CoopGame::Session().SendThingStates(out);
}

// --- applying the others' things ---

void Boom(Actor* ghost) {
    Vec3f zero = { 0.0f, 0.0f, 0.0f };
    Vec3f accel = { 0.0f, 0.6f, 0.0f }; // same numbers the bomb uses for its own explosion
    EffectSsBomb2_SpawnLayered(gPlayState, &ghost->world.pos, &zero, &accel, 100, 19);
    Audio_PlayActorSound2(ghost, NA_SE_IT_BOMB_EXPLOSION);
}

void RemoveGhost(Actor* ghost, bool explode) {
    if (explode && ghost->category == ACTORCAT_EXPLOSIVE) {
        Boom(ghost);
    }
    sGhosts.erase(ghost);
    Actor_Kill(ghost);
}

void DropOwner(OwnerThings& owner, bool explode) {
    for (auto& [id, ghost] : owner.ghosts) {
        RemoveGhost(ghost, explode);
    }
    owner.ghosts.clear();
}

void ApplyTheirThings(const Coop::ThingStates& list) {
    OwnerThings& owner = sOwners[list.owner];
    owner.quietFrames = 0;
    if (list.sceneNum != gPlayState->sceneNum || list.layer != static_cast<uint8_t>(gSaveContext.sceneLayer)) {
        DropOwner(owner, false); // they're somewhere else
        return;
    }
    std::set<uint16_t> seen;
    for (const Coop::ThingState& t : list.things) {
        if (t.listIndex >= 0) {
            // a pot / rock from the room that they're holding: move ours to their spot
            if (Actor* a = FindListActor(t.room, t.listIndex, t.actorId)) {
                a->world.pos = { t.pos.x, t.pos.y, t.pos.z };
                a->shape.rot = { t.rot.x, t.rot.y, t.rot.z };
                a->world.rot.y = t.rot.y;
                a->speedXZ = 0.0f;
                a->velocity = { 0.0f, 0.0f, 0.0f };
                sMovedByTeammate[a] = 0;
            }
            continue;
        }
        seen.insert(t.localId);
        Actor*& ghost = owner.ghosts[t.localId];
        if (ghost == nullptr) {
            ghost = Actor_Spawn(&gPlayState->actorCtx, gPlayState, t.actorId, t.pos.x, t.pos.y, t.pos.z, t.rot.x,
                                t.rot.y, t.rot.z, t.params);
            if (ghost == nullptr) {
                owner.ghosts.erase(t.localId);
                continue;
            }
            sGhosts.insert(ghost);
        }
        ghost->world.pos = { t.pos.x, t.pos.y, t.pos.z };
        ghost->shape.rot = { t.rot.x, t.rot.y, t.rot.z };
        CoopGame::WriteActorBody(ghost, t.body, t.slotKinds); // fuse timer, flashing...
    }
    // gone from their list: it blew up (or got picked up by the game, same thing for us)
    for (auto it = owner.ghosts.begin(); it != owner.ghosts.end();) {
        if (!seen.contains(it->first)) {
            RemoveGhost(it->second, true);
            it = owner.ghosts.erase(it);
        } else {
            ++it;
        }
    }
}

void OnShouldActorUpdate(void* ptr, bool* should) {
    Actor* a = static_cast<Actor*>(ptr);
    if (sGhosts.contains(a)) {
        *should = false;
        return;
    }
    auto it = sMovedByTeammate.find(a);
    if (it != sMovedByTeammate.end() && it->second < kTeammateHoldFrames) {
        *should = false; // in a teammate's hands: don't let it fall / break on its own
    }
}

void OnActorDestroy(void* ptr) {
    Actor* a = static_cast<Actor*>(ptr);
    sOurIds.erase(a);
    sFollowed.erase(a);
    sMovedByTeammate.erase(a);
    if (sGhosts.erase(a) > 0) {
        for (auto& [tag, owner] : sOwners) {
            std::erase_if(owner.ghosts, [&](const auto& g) { return g.second == a; });
        }
    }
}

} // namespace

namespace CoopGame {

void UpdateThings() {
    std::vector<Coop::ThingStates> lists = Session().TakeThingStates();
    if (!SessionActive() || !LocalState().inGame || gPlayState == nullptr) {
        for (auto& [tag, owner] : sOwners) {
            for (auto& [id, ghost] : owner.ghosts) {
                sGhosts.erase(ghost); // the scene is gone anyway, actors are freed with it
            }
        }
        sOwners.clear();
        sFollowed.clear();
        sMovedByTeammate.clear();
        return;
    }
    if (sOwnerTag == 0) {
        sOwnerTag = std::random_device{}() | 1; // never 0
    }
    SendOurThings();

    for (const Coop::ThingStates& list : lists) {
        if (list.owner != sOwnerTag) {
            ApplyTheirThings(list);
        }
    }
    for (auto it = sOwners.begin(); it != sOwners.end();) {
        if (++it->second.quietFrames > kOwnerGoneFrames) {
            DropOwner(it->second, false);
            it = sOwners.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = sMovedByTeammate.begin(); it != sMovedByTeammate.end();) {
        if (++it->second > kTeammateHoldFrames) {
            it = sMovedByTeammate.erase(it); // they let go: its own code takes over again
        } else {
            ++it;
        }
    }
}

void RegisterThingHooks() {
    COND_HOOK(ShouldActorUpdate, true, OnShouldActorUpdate);
    COND_HOOK(OnActorDestroy, true, OnActorDestroy);
}

} // namespace CoopGame
