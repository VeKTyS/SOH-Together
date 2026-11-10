// "Puppets" = the other players' Links, drawn in our world.
//
// Same trick as SoH's Anchor (DummyPlayer.cpp): we spawn a real Link actor,
// then catch it right before it initializes and swap its functions for ours.
// It never thinks for itself: every frame it just copies the pose (skeleton
// angles, gear, held item...) the other player sent us, and Link's normal
// drawing code does the rest.
//
// For now puppets are only visual: you can't hit them, push them or target them.

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/nametag.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
}

namespace {

// Stuck on each puppet actor so we know whose Link it is.
struct PuppetInfo {
    Coop::PeerId peer = 0;
    uint8_t age = 0; // skeleton it was built with, child and adult ones differ
};
ObjectExtension::Register<PuppetInfo> sPuppetInfoRegister;

// Set while we spawn a puppet, so the ShouldActorInit hook knows this Link is ours.
Coop::PeerId sSpawningFor = 0;
// Puppets get respawned on the next Link update when this is set.
bool sRefreshNeeded = false;
// Who had a puppet last time, to notice joins and leaves.
std::set<Coop::PeerId> sPeersWithPuppet;

void Puppet_Init(Actor* actor, PlayState* play);
void Puppet_Update(Actor* actor, PlayState* play);
void Puppet_Draw(Actor* actor, PlayState* play);
void Puppet_Destroy(Actor* actor, PlayState* play);

const Coop::RemotePlayer* FindPlayer(Coop::PeerId peer) {
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (p.peer == peer) {
            return &p;
        }
    }
    return nullptr;
}

Coop::PeerId PeerOf(Actor* actor) {
    PuppetInfo* info = ObjectExtension::GetInstance().Get<PuppetInfo>(actor);
    return info != nullptr ? info->peer : 0;
}

// Values come from the network: a bad one could index past the end of the
// game's tables when drawing, so anything out of range falls back to a safe default.
template <typename T> T InRange(T value, int min, int maxExclusive, T fallback) {
    return value >= min && value < maxExclusive ? value : fallback;
}

// The game reads the player's age and B item from the save in a few places
// while building/drawing Link. We swap in the other player's values for the
// duration of the call and put ours back right after.
struct BorrowAge {
    s32 savedAge;
    u8 savedB;
    BorrowAge(uint8_t age, uint8_t buttonItem0)
        : savedAge(gSaveContext.linkAge), savedB(gSaveContext.equips.buttonItems[0]) {
        gSaveContext.linkAge = age;
        gSaveContext.equips.buttonItems[0] = buttonItem0;
    }
    ~BorrowAge() {
        gSaveContext.linkAge = savedAge;
        gSaveContext.equips.buttonItems[0] = savedB;
    }
};

void Puppet_Init(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;
    std::lock_guard lock(CoopGame::Mutex());
    const Coop::RemotePlayer* remote = FindPlayer(PeerOf(actor));
    if (remote == nullptr) {
        Actor_Kill(actor);
        return;
    }
    uint8_t age = remote->state ? InRange<uint8_t>(remote->state->linkAge, 0, 2, 0) : (uint8_t)gSaveContext.linkAge;
    ObjectExtension::GetInstance().Get<PuppetInfo>(actor)->age = age;

    BorrowAge borrow(age, gSaveContext.equips.buttonItems[0]);
    // copied from Anchor's DummyPlayer_Init (itself based on EnTorch2_Init / Player_Init)
    actor->room = -1; // stays alive when we change rooms
    player->itemAction = player->heldItemAction = -1;
    player->heldItemId = ITEM_NONE;
    Player_UseItem(play, player, ITEM_NONE);
    Player_SetModelGroup(player, Player_ActionToModelGroup(player, player->heldItemAction));
    play->playerInit(player, play, gPlayerSkelHeaders[age]);
    // puppets don't swing swords for real, don't let them eat a sword trail slot
    Effect_Delete(play, player->meleeWeaponEffectIndex);
    player->meleeWeaponEffectIndex = TOTAL_EFFECT_COUNT;
    play->func_11D54(player, play);

    // purely visual for now: no targeting
    actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
    NameTag_RegisterForActorWithOptions(actor, remote->name.c_str(), {});
}

void Hide(Actor* actor) {
    actor->world.pos = { -9999.0f, -9999.0f, -9999.0f };
    actor->shape.shadowAlpha = 0;
}

void Puppet_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;
    std::lock_guard lock(CoopGame::Mutex());
    const Coop::RemotePlayer* remote = FindPlayer(PeerOf(actor));
    if (remote == nullptr) {
        Actor_Kill(actor);
        return;
    }
    // somewhere else, or no pose yet: park it out of sight
    if (!remote->state || !remote->pose || !remote->state->inGame || remote->state->sceneNum != play->sceneNum) {
        Hide(actor);
        return;
    }
    const Coop::PlayerState& s = *remote->state;
    const Coop::PlayerPose& p = *remote->pose;

    // they grew up / went back in time: the skeleton is different, rebuild the puppet
    if (s.linkAge != ObjectExtension::GetInstance().Get<PuppetInfo>(actor)->age) {
        sRefreshNeeded = true;
        Hide(actor);
        return;
    }

    actor->shape.shadowAlpha = 255;
    actor->world.pos = { s.x, s.y, s.z };
    actor->shape.rot = { 0, s.yaw, 0 };
    for (size_t i = 0; i < Coop::kPoseJointCount; i++) {
        player->skelAnime.jointTable[i] = { p.joints[i].x, p.joints[i].y, p.joints[i].z };
    }
    player->upperLimbRot = { p.upperLimbRot.x, p.upperLimbRot.y, p.upperLimbRot.z };
    player->skelAnime.prevTransl = { p.prevTransl.x, p.prevTransl.y, p.prevTransl.z };
    player->skelAnime.movementFlags = p.movementFlags;
    player->currentBoots = InRange<s8>(p.currentBoots, 0, PLAYER_BOOTS_MAX, 0);
    player->currentShield = InRange<s8>(p.currentShield, 0, PLAYER_SHIELD_MAX, 0);
    player->currentTunic = InRange<s8>(p.currentTunic, 0, PLAYER_TUNIC_MAX, 0);
    player->heldItemId = p.buttonItem0;
    player->stateFlags1 = p.stateFlags1;
    player->stateFlags2 = p.stateFlags2;
    player->itemAction = InRange<s8>(p.itemAction, -1, PLAYER_IA_MAX, -1);
    player->heldItemAction = InRange<s8>(p.heldItemAction, -1, PLAYER_IA_MAX, -1);
    player->invincibilityTimer = p.invincibilityTimer;
    player->unk_862 = InRange<s16>(p.unk_862, -GID_MAXIMUM, GID_MAXIMUM + 1, 0);
    player->unk_85C = p.unk_85C;
    player->av1.actionVar1 = p.actionVar1;

    // root motion, same as Player_ApplyAnimMovementScaledByAge but with THEIR age
    Vec3f diff;
    SkelAnime_UpdateTranslation(&player->skelAnime, &diff, actor->shape.rot.y);
    if (player->skelAnime.movementFlags & 1) {
        if (s.linkAge == LINK_AGE_CHILD) {
            diff.x *= 0.64f;
            diff.z *= 0.64f;
        }
        actor->world.pos.x += diff.x * actor->scale.x;
        actor->world.pos.z += diff.z * actor->scale.z;
    }
    if ((player->skelAnime.movementFlags & 2) && !(player->skelAnime.movementFlags & 4)) {
        diff.y *= player->ageProperties->unk_08;
    }
    if (player->skelAnime.movementFlags & 2) {
        actor->world.pos.y += diff.y * actor->scale.y;
    }

    uint8_t modelGroup = InRange<uint8_t>(p.modelGroup, 0, PLAYER_MODELGROUP_MAX, 0);
    if (player->modelGroup != modelGroup) {
        BorrowAge borrow(s.linkAge, p.buttonItem0);
        Player_SetModelGroup(player, modelGroup);
    }
}

void Puppet_Draw(Actor* actor, PlayState* play) {
    std::lock_guard lock(CoopGame::Mutex());
    const Coop::RemotePlayer* remote = FindPlayer(PeerOf(actor));
    if (remote == nullptr || !remote->state || !remote->pose || remote->state->sceneNum != play->sceneNum ||
        actor->shape.shadowAlpha == 0) {
        return;
    }
    BorrowAge borrow(remote->state->linkAge, remote->pose->buttonItem0);
    Player_Draw(actor, play);
}

void Puppet_Destroy(Actor* actor, PlayState* play) {
    NameTag_RemoveAllForActor(actor);
    // it was spawned as ACTOR_PLAYER: put the id back so the game's actor
    // counter for Link goes back down properly (see Anchor's DummyPlayer_Destroy)
    actor->id = ACTOR_PLAYER;
}

void KillAllPuppets() {
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; actor != nullptr; actor = actor->next) {
        if (actor->update == Puppet_Update) {
            Actor_Kill(actor);
        }
    }
}

// Runs from Link's update (like Anchor), when the game is in a good spot to spawn actors.
void RespawnPuppetsIfNeeded() {
    if (!sRefreshNeeded || gPlayState == nullptr) {
        return;
    }
    sRefreshNeeded = false;
    KillAllPuppets();

    std::vector<Coop::PeerId> peers;
    {
        std::lock_guard lock(CoopGame::Mutex());
        for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
            peers.push_back(p.peer);
        }
    }
    // no lock here: Actor_Spawn calls Puppet_Init right away, which locks
    for (Coop::PeerId peer : peers) {
        sSpawningFor = peer;
        Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_PLAYER, 0, -9999.0f, 0, 0, 0, 0, 0);
    }
    sSpawningFor = 0;
}

} // namespace

namespace CoopGame {

bool ReadLocalPose(Coop::PlayerPose& pose) {
    if (gPlayState == nullptr) {
        return false;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return false;
    }
    for (size_t i = 0; i < Coop::kPoseJointCount; i++) {
        const Vec3s& j = player->skelAnime.jointTable[i];
        pose.joints[i] = { j.x, j.y, j.z };
    }
    pose.upperLimbRot = { player->upperLimbRot.x, player->upperLimbRot.y, player->upperLimbRot.z };
    pose.prevTransl = { player->skelAnime.prevTransl.x, player->skelAnime.prevTransl.y,
                        player->skelAnime.prevTransl.z };
    pose.movementFlags = player->skelAnime.movementFlags;
    pose.currentBoots = player->currentBoots;
    pose.currentShield = player->currentShield;
    pose.currentTunic = player->currentTunic;
    pose.stateFlags1 = player->stateFlags1;
    // if we're hidden for a cutscene, still show us to the others
    pose.stateFlags2 = player->stateFlags2 & ~PLAYER_STATE2_DISABLE_DRAW;
    pose.buttonItem0 = gSaveContext.equips.buttonItems[0];
    pose.itemAction = player->itemAction;
    pose.heldItemAction = player->heldItemAction;
    pose.modelGroup = player->modelGroup;
    pose.invincibilityTimer = player->invincibilityTimer;
    pose.unk_862 = player->unk_862;
    pose.unk_85C = player->unk_85C;
    pose.actionVar1 = player->av1.actionVar1;
    return true;
}

void UpdatePuppets() {
    std::set<Coop::PeerId> now;
    for (const Coop::RemotePlayer& p : Session().Players()) {
        now.insert(p.peer);
    }
    if (now != sPeersWithPuppet) {
        sPeersWithPuppet = now;
        sRefreshNeeded = true;
    }
}

void RegisterPuppetHooks() {
    // the Link we're spawning becomes a puppet instead of a second player
    COND_ID_HOOK(ShouldActorInit, ACTOR_PLAYER, true, [](void* actorRef, bool* should) {
        if (sSpawningFor == 0) {
            return; // the real Link, or Anchor's dummies: not ours
        }
        Actor* actor = (Actor*)actorRef;
        ObjectExtension::GetInstance().Set<PuppetInfo>(actor, PuppetInfo{ sSpawningFor, 0 });
        // it's already in the "player" list at this point, move it out of there
        Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, actor, ACTORCAT_NPC);
        actor->id = ACTOR_EN_OE2; // any id that isn't ACTOR_PLAYER, same as Anchor
        actor->category = ACTORCAT_NPC;
        actor->init = Puppet_Init;
        actor->update = Puppet_Update;
        actor->draw = Puppet_Draw;
        actor->destroy = Puppet_Destroy;
    });
    // a scene load wipes all actors, puppets included
    COND_HOOK(OnSceneSpawnActors, true, []() { sRefreshNeeded = true; });
    COND_HOOK(OnPlayerUpdate, true, RespawnPuppetsIfNeeded);
}

} // namespace CoopGame
