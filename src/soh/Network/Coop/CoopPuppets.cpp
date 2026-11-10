// "Puppets" = the other players' Links, drawn in our world.
//
// Same trick as SoH's Anchor (DummyPlayer.cpp): we spawn a real Link actor,
// then catch it right before it initializes and swap its functions for ours.
// It never thinks for itself: every frame it just copies the pose (skeleton
// angles, gear, held item...) the other player sent us, and Link's normal
// drawing code does the rest.
//
// PvP is copied from Anchor too: when the room's PvP mode says this player is
// an enemy, the puppet gets a body you can hit, and hitting it tells its owner
// they got hurt (EventKind::DamagePlayer, Anchor Packets/DamagePlayer.cpp).

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/nametag.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
}

// Room settings, they live in CoopRoom.cpp (Anchor's roomState / TeamId)
namespace {

// copied from Anchor.h: starting at 5 to continue from the last value in the
// PlayerDamageResponseType enum
typedef enum {
    DUMMY_PLAYER_HIT_RESPONSE_STUN = 5,
    DUMMY_PLAYER_HIT_RESPONSE_FIRE,
    DUMMY_PLAYER_HIT_RESPONSE_NORMAL,
} DummyPlayerDamageResponseType;

// copied from Anchor's DummyPlayer.cpp: what each weapon does to a puppet
DamageTable DummyPlayerDamageTable = {
    /* Deku nut      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Deku stick    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Slingshot     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Explosive     */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Boomerang     */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Normal arrow  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Hammer swing  */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Hookshot      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Kokiri sword  */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master sword  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant's Knife */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Fire arrow    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice arrow     */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_FROZEN),
    /* Light arrow   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_ELECTRIFIED),
    /* Unk arrow 1   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 2   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 3   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Fire magic    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice magic     */ DMG_ENTRY(3, PLAYER_HIT_RESPONSE_FROZEN),
    /* Light magic   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_ELECTRIFIED),
    /* Shield        */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Mirror Ray    */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Kokiri spin   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant spin    */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master spin   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Kokiri jump   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant jump    */ DMG_ENTRY(8, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master jump   */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Unknown 1     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Unblockable   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Hammer jump   */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Unknown 2     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
};

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

    // a body you can hit, copied from Anchor's DummyPlayer_Init. UpdatePvp
    // only puts it in the game when PvP says so.
    player->cylinder.base.acFlags = AC_ON | AC_TYPE_PLAYER;
    player->cylinder.base.ocFlags2 = OC2_TYPE_1;
    player->cylinder.info.bumperFlags = BUMP_ON | BUMP_HOOKABLE | BUMP_NO_HITMARK;
    player->actor.flags |= ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER;
    player->cylinder.dim.radius = 30;
    player->actor.colChkInfo.damageTable = &DummyPlayerDamageTable;

    // no targeting until PvP says otherwise (UpdatePvp decides every frame)
    actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
    NameTag_RegisterForActorWithOptions(actor, remote->name.c_str(), {});
}

void Hide(Actor* actor) {
    actor->world.pos = { -9999.0f, -9999.0f, -9999.0f };
    actor->shape.shadowAlpha = 0;
    actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
}

// Did an enemy hit the puppet? Enemies already hurt each player in their own
// game, sending that hit too would hurt them twice (TrueCoop DummyPlayer.cpp
// :159-164, the IsTrackedEnemy check). Our synced enemies are the ENEMY / BOSS
// ones (CoopEnemies.cpp IsSyncedCategory). We look for the pointer in the lists
// instead of reading it: the enemy may be gone since the hit.
bool HitByEnemy(PlayState* play, Actor* attacker) {
    if (attacker == nullptr) {
        return false;
    }
    for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        for (Actor* a = play->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a == attacker) {
                return true;
            }
        }
    }
    return false;
}

// PvP part of Anchor's DummyPlayer_Update (the end of it), plus TrueCoop's enemy check
void UpdatePvp(Player* player, PlayState* play, const Coop::RemotePlayer& remote) {
    Actor* actor = &player->actor;
    uint8_t pvpMode = CoopGame::RoomPvpMode();
    if (pvpMode == 0 || (pvpMode == 1 && remote.state->team == CoopGame::OwnTeam())) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    actor->flags &= ~ACTOR_FLAG_LOCK_ON_DISABLED;

    if (player->cylinder.base.acFlags & AC_HIT && player->invincibilityTimer == 0 &&
        !HitByEnemy(play, player->cylinder.base.ac)) {
        // same fields as Anchor's SendPacket_DamagePlayer: who, damage effect, damage
        Coop::ByteWriter w;
        (w.U32)(remote.id); // U32 is also a game macro (r4300.h), the () dodge it
        w.U8(player->actor.colChkInfo.damageEffect);
        w.U8(player->actor.colChkInfo.damage);
        CoopGame::SendEvent(Coop::EventKind::DamagePlayer, w.Take());
        if (player->actor.colChkInfo.damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
            Actor_SetColorFilter(&player->actor, 0, 0xFF, 0, 24);
        } else {
            player->invincibilityTimer = 20;
        }
    }

    Collider_UpdateCylinder(&player->actor, &player->cylinder);

    if (!(player->stateFlags2 & PLAYER_STATE2_FROZEN)) {
        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_HANGING_OFF_LEDGE |
                                     PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_ON_HORSE))) {
            CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);
        }

        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_DAMAGED)) &&
            (player->invincibilityTimer <= 0)) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &player->cylinder.base);

            if (player->invincibilityTimer < 0) {
                CollisionCheck_SetAT(play, &play->colChkCtx, &player->cylinder.base);
            }
        }
    }

    if (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE)) {
        player->actor.colChkInfo.mass = MASS_IMMOVABLE;
    } else {
        player->actor.colChkInfo.mass = 50;
    }

    Collider_ResetCylinderAC(play, &player->cylinder.base);
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
    actor->shape.rot = { p.rotX, s.yaw, p.rotZ };
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
    // on fire: same flames the game puts on a burning Link (Player_UpdateBodyBurn),
    // without the damage, that's for the real one
    for (size_t i = 0; i < p.flameTimers.size() && i < PLAYER_BODYPART_MAX; i++) {
        uint8_t t = p.flameTimers[i];
        if (t == 0) {
            continue;
        }
        f32 scale = t > 20 ? CLAMP((t - 20.0f) * 0.01f, 0.19999999f, 0.2f) : t * 0.01f;
        f32 intensity = CLAMP((t - 25.0f) * 0.02f, 0.0f, 1.0f);
        // body part -1 + a position relative to the puppet: with a real body part
        // number the effect sticks to OUR Link (it uses GET_PLAYER), not the puppet
        Vec3f offset = { player->bodyPartsPos[i].x - player->actor.world.pos.x,
                         player->bodyPartsPos[i].y - player->actor.world.pos.y,
                         player->bodyPartsPos[i].z - player->actor.world.pos.z };
        EffectSsFireTail_SpawnFlame(gPlayState, &player->actor, &offset, scale, -1, intensity);
    }

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

    UpdatePvp(player, play, *remote);
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

// The puppet of player `id` in our scene, nullptr if they don't have one
Player* PuppetOfPlayer(uint32_t id) {
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; actor != nullptr; actor = actor->next) {
        const Coop::RemotePlayer* owner = CoopGame::PuppetOwner(actor);
        if (owner != nullptr && owner->id == id) {
            return (Player*)actor;
        }
    }
    return nullptr;
}

// Someone hit our puppet in their game. Copied from Anchor's
// HandlePacket_DamagePlayer (Packets/DamagePlayer.cpp), with TrueCoop's
// "already invincible = ignore it" check.
void OnDamagePlayer(const Coop::Event& e) {
    Coop::ByteReader r(e.data);
    uint32_t target = 0;
    uint8_t damageEffect = 0;
    uint8_t damage = 0;
    if (!(r.U32)(target) || !r.U8(damageEffect) || !r.U8(damage) || r.Remaining() != 0) {
        return;
    }
    // everybody gets the event, only the one who got hit cares
    if (target != CoopGame::SelfId()) {
        return;
    }
    // DMG_ENTRY packs both in 4 bits, anything bigger is a broken packet
    if (damageEffect > DUMMY_PLAYER_HIT_RESPONSE_NORMAL || damage > 0xF) {
        return;
    }
    if (gPlayState == nullptr) {
        return;
    }
    // Anchor needs the attacker's Link here, to know which way to knock us back
    Player* otherPlayer = PuppetOfPlayer(e.origin);
    Player* self = GET_PLAYER(gPlayState);
    if (otherPlayer == nullptr || self == nullptr) {
        return;
    }

    // TrueCoop: our own i-frames win. Two hits landing close together would
    // otherwise both hurt us.
    if (self->invincibilityTimer > 0) {
        return;
    }

    // Prevent incoming damage during cutscenes or item get sequences
    if (Player_InBlockingCsMode(gPlayState, self) || self->stateFlags1 & PLAYER_STATE1_IN_ITEM_CS ||
        self->stateFlags1 & PLAYER_STATE1_GETTING_ITEM) {
        return;
    }

    self->actor.colChkInfo.damage = damage * 8; // Arbitrary number currently, need to fine tune (Anchor)

    if (damageEffect == DUMMY_PLAYER_HIT_RESPONSE_FIRE) {
        for (int i = 0; i < ARRAY_COUNT(self->bodyFlameTimers); i++) {
            self->bodyFlameTimers[i] = static_cast<uint8_t>(Rand_S16Offset(0, 200));
        }
        self->bodyIsBurning = true;
    } else if (damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
        self->actor.freezeTimer = 20;
        Actor_SetColorFilter(&self->actor, 0, 0xFF, 0, 24);
        return;
    }

    func_80837C0C(gPlayState, self, damageEffect, 4.0f, 5.0f,
                  Actor_WorldYawTowardActor(&otherPlayer->actor, &self->actor), 20);
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
    pose.rotX = player->actor.shape.rot.x;
    pose.rotZ = player->actor.shape.rot.z;
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
    for (size_t i = 0; i < pose.flameTimers.size() && i < PLAYER_BODYPART_MAX; i++) {
        pose.flameTimers[i] = player->bodyIsBurning ? player->bodyFlameTimers[i] : 0;
    }
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
    // PvP: a teammate hit our puppet in their game
    OnEvent(Coop::EventKind::DamagePlayer, "CoopPuppets", OnDamagePlayer);
}

const Coop::RemotePlayer* PuppetOwner(Actor* puppet) {
    if (puppet == nullptr || puppet->update != Puppet_Update) {
        return nullptr;
    }
    PuppetInfo* info = ObjectExtension::GetInstance().Get<PuppetInfo>(puppet);
    if (info == nullptr) {
        return nullptr;
    }
    for (const Coop::RemotePlayer& p : Session().Players()) {
        if (p.peer == info->peer) {
            return &p;
        }
    }
    return nullptr;
}

std::vector<Actor*> VisiblePuppets() {
    std::vector<Actor*> out;
    if (gPlayState == nullptr) {
        return out;
    }
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_NPC].head; actor != nullptr; actor = actor->next) {
        // hidden ones are parked at -9999 (other scene, no pose yet)
        if (actor->update == Puppet_Update && actor->world.pos.y > -9000.0f) {
            out.push_back(actor);
        }
    }
    return out;
}

// Dead puppets stay in VisiblePuppets() (the minimap and the life code still
// want them), ask this when it matters. Session locked.
bool PuppetIsDead(Actor* puppet) {
    if (puppet == nullptr || puppet->update != Puppet_Update) {
        return false;
    }
    // their Link is playing the death animation
    if (((Player*)puppet)->stateFlags1 & PLAYER_STATE1_DEAD) {
        return true;
    }
    // or they're down / game over (CoopLife.cpp)
    const Coop::RemotePlayer* owner = PuppetOwner(puppet);
    if (owner == nullptr) {
        return false;
    }
    LifeState life = PlayerLifeState(owner->id);
    return life == LifeState::Downed || life == LifeState::GameOver;
}

} // namespace CoopGame
