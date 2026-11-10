// Enemy sync.
//
// Same enemy on every game: enemies placed in a level come from the room's
// actor list, and SoH remembers their position in that list (ActorListIndex).
// Same game data on both sides = same numbers. So an enemy is
// scene + layer + room + list index. Enemies spawned on the fly (spawner
// children, Gohma larvas...) don't have one and stay local for now.
//
// Who runs the enemies: the host. When a client is in the same scene as the
// host, the client's copies are frozen and just copy what the host sends every
// frame (position, skeleton, health, which colliders are on). When the client
// hits one, the hit goes to the host, which replays it on its enemy through the
// enemy's own damage code (so shields, weak spots and i-frames still work).
// Same idea as OOT True Co-op, our own code.
//
// When nobody is copying (client alone in a scene, or bosses which are too
// special to copy), everyone runs their own AI and we share the health instead:
// damage done on one game is replayed on the others.
//
// Deaths: when an enemy dies somewhere, it dies everywhere, and everyone gets
// their own death animation and their own loot (loot is per player).
//
// Needs 3 hooks we add to SoH (patches/soh-coop-hooks.patch): OnColliderSetup,
// OnColliderSubmit and OnSkelAnimeInit, to know an enemy's colliders and skeleton.

#include "soh/Network/Coop/CoopGame.h"

#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ActorListIndex.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
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

// --- tuning ---

// a client stops copying an enemy if the host hasn't talked about it for this long
constexpr int kStaleFrames = 30;
// a remote kill that the enemy's own code didn't finish by then gets forced
constexpr int kForceKillFrames = 60;
// shared health: damage we were told about but the enemy didn't take by then is forced
constexpr int kExpectedDamageFrames = 40;

// --- what we know about each enemy actor ---

struct EnemyInfo {
    std::vector<Collider*> colliders; // in setup order, same order on every game
    SkelAnime* skel = nullptr;
    uint8_t submitted = 0; // host: colliders used since the last send (1 AT, 2 AC, 4 OC)
    bool cullSaved = false;
    uint32_t savedCullFlags = 0;
    // client copying it
    bool mirrored = false;
    bool released = false; // dying: let its own code play the death
    int forceKillIn = 0;
    // host: hits from clients, applied right before the enemy's update
    std::vector<Coop::EnemyHit> pendingHits;
    // shared health mode
    uint8_t prevHealth = 0;
    bool healthKnown = false;
    uint8_t expectedRemote = 0; // damage we caused because someone told us to
    int expectedTimer = 0;
};

std::unordered_map<Actor*, EnemyInfo> sEnemies;
// skeletons are set up inside the actor's init, before we know who's who.
// Kept here until OnActorInit says which actor owns them.
std::vector<SkelAnime*> sPendingSkels;

// --- the network picture, refreshed once per frame (game thread only) ---

bool sHostStreaming = false; // host, and a client is in our scene
bool sClientCopying = false; // client, and the host's enemy stream is fresh

struct RoomIndex {
    int8_t room;
    int16_t index;
    bool operator<(const RoomIndex& o) const {
        return std::tie(room, index) < std::tie(o.room, o.index);
    }
};
std::map<RoomIndex, Coop::EnemyState> sHostStates; // client: latest host state per enemy
int sStatesAge = 1000;                             // frames since the last host packet
size_t sLastStateCount = 0;                        // only for the log
bool sLayerMismatchLogged = false;
int16_t sReloadedInScene = -1; // scene we already reloaded once to match the host's version

// Enemies dead for the group (ours and theirs), so they don't come back when
// someone walks back into the room while a teammate is still in it.
auto KeyOf(const Coop::EnemyKill& k) {
    return std::make_tuple(k.sceneNum, k.layer, k.room, k.listIndex);
}
struct KeyLess {
    bool operator()(const Coop::EnemyKill& a, const Coop::EnemyKill& b) const {
        return KeyOf(a) < KeyOf(b);
    }
};
std::set<Coop::EnemyKill, KeyLess> sDead;

// true while we kill/damage an enemy because of the network, so it doesn't bounce back
bool sApplyingRemote = false;

// fake "thing that hit you" handed to enemies when we replay a hit. Enemies read
// it during their update (a frame later), so it has to stay alive a bit: ring.
ColliderInfo sFakeHitters[8];
uint8_t sFakeHitterNext = 0;

// --- small helpers ---

bool IsSyncedCategory(Actor* actor) {
    return actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS;
}

bool SessionActive() {
    Coop::SessionRole role = CoopGame::Session().Role();
    return role == Coop::SessionRole::Hosting || role == Coop::SessionRole::Connected;
}

uint8_t CurrentLayer() {
    return static_cast<uint8_t>(gSaveContext.sceneLayer);
}

// Builds the network id of a live actor. False if it's not one we sync.
bool MakeKey(Actor* actor, Coop::EnemyKill& out) {
    if (gPlayState == nullptr || !IsSyncedCategory(actor)) {
        return false;
    }
    int16_t index = GetActorListIndex(actor);
    if (index < 0) {
        return false; // spawned on the fly, not from the room list
    }
    out.sceneNum = gPlayState->sceneNum;
    out.layer = CurrentLayer();
    out.room = actor->room;
    out.listIndex = index;
    out.actorId = actor->id;
    out.params = actor->params;
    return true;
}

Actor* FindEnemy(int8_t room, int16_t index, int16_t actorId) {
    for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->room == room && a->id == actorId && GetActorListIndex(a) == index) {
                return a;
            }
        }
    }
    return nullptr;
}

template <typename Fn> void ForEachElement(Collider* col, Fn&& fn) {
    switch (col->shape) {
        case COLSHAPE_JNTSPH: {
            ColliderJntSph* c = (ColliderJntSph*)col;
            for (s32 i = 0; i < c->count && c->elements != nullptr; i++) {
                fn(&c->elements[i].info);
            }
            break;
        }
        case COLSHAPE_CYLINDER:
            fn(&((ColliderCylinder*)col)->info);
            break;
        case COLSHAPE_TRIS: {
            ColliderTris* c = (ColliderTris*)col;
            for (s32 i = 0; i < c->count && c->elements != nullptr; i++) {
                fn(&c->elements[i].info);
            }
            break;
        }
        case COLSHAPE_QUAD:
            fn(&((ColliderQuad*)col)->info);
            break;
        default:
            break;
    }
}

// Enemies far from our own Link don't update (culling). While someone else may
// be next to them, they must keep going.
void ForceUncull(Actor* actor, EnemyInfo& info) {
    if (!info.cullSaved) {
        info.savedCullFlags = actor->flags & (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED);
        info.cullSaved = true;
    }
    actor->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED;
}

void RestoreCull(Actor* actor, EnemyInfo& info) {
    if (info.cullSaved) {
        actor->flags &= ~(ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED);
        actor->flags |= info.savedCullFlags;
        info.cullSaved = false;
    }
}

// The thing that "hit" the enemy when we replay a hit: the nearest other
// player's Link if we see one, else ours. Some enemies use it for knockback.
Actor* FakeAttacker(Actor* enemy) {
    Actor* best = &GET_PLAYER(gPlayState)->actor;
    float bestDist = Actor_WorldDistXZToActor(enemy, best);
    for (Actor* puppet : CoopGame::VisiblePuppets()) {
        float d = Actor_WorldDistXZToActor(enemy, puppet);
        if (d < bestDist) {
            best = puppet;
            bestDist = d;
        }
    }
    return best;
}

// Make the enemy believe it just got hit, then let its own update deal with it
// (hurt animation, knockback, death, drops). Same trick Anchor uses for bomb walls.
void ReplayHit(Actor* actor, EnemyInfo& info, uint8_t damage, uint8_t effect, uint32_t dmgFlags, Vec3s hitPos) {
    Collider* target = nullptr;
    for (Collider* col : info.colliders) {
        if (col->actor == actor && (col->acFlags & AC_ON)) {
            target = col;
            break;
        }
    }
    if (target == nullptr) {
        // no hurtbox right now (or we never saw its colliders): just take the health
        actor->colChkInfo.health = actor->colChkInfo.health > damage ? actor->colChkInfo.health - damage : 0;
        return;
    }
    ColliderInfo* hitter = &sFakeHitters[sFakeHitterNext];
    sFakeHitterNext = (sFakeHitterNext + 1) % ARRAY_COUNT(sFakeHitters);
    memset(hitter, 0, sizeof(*hitter));
    hitter->toucher.dmgFlags = dmgFlags != 0 ? dmgFlags : DMG_SLASH;
    hitter->toucher.damage = damage;
    hitter->toucherFlags = TOUCH_ON | TOUCH_HIT;

    actor->colChkInfo.damage = damage;
    actor->colChkInfo.damageEffect = effect;
    target->acFlags |= AC_HIT;
    target->ac = FakeAttacker(actor);
    ForEachElement(target, [&](ColliderInfo* e) {
        e->bumperFlags |= BUMP_HIT;
        e->acHitInfo = hitter;
        e->acHit = nullptr;
        e->bumper.hitPos = hitPos;
    });
}

// Small white puff, the same one enemies leave when they die.
void Poof(Actor* actor) {
    Vec3f pos = actor->world.pos;
    pos.y += 20.0f;
    Vec3f zero = { 0.0f, 0.0f, 0.0f };
    EffectSsDeadDb_Spawn(gPlayState, &pos, &zero, &zero, 150, 0, 255, 255, 255, 255, 150, 150, 150, 1, 13, 1);
}

// --- deaths ---

void SendKill(Actor* actor) {
    if (sApplyingRemote) {
        return;
    }
    Coop::EnemyKill kill;
    if (!MakeKey(actor, kill) || !sDead.insert(kill).second) {
        return; // not ours to sync, or already sent (defeat hook then kill hook)
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!SessionActive()) {
        return;
    }
    CoopGame::Session().SendEnemyKill(kill);
    SPDLOG_INFO("[Coop] Enemy killed: actor {:#x} room {} index {}", kill.actorId, kill.room, kill.listIndex);
}

// Leaving a room also calls Actor_Kill on its enemies: that's not a death.
bool IsBeingUnloaded(Actor* actor) {
    const RoomContext& rooms = gPlayState->roomCtx;
    return actor->room >= 0 && actor->room != rooms.curRoom.num && actor->room != rooms.prevRoom.num;
}

void OnActorKill(void* ptr) {
    Actor* actor = static_cast<Actor*>(ptr);
    // most enemies only call Actor_Kill at the end of their death animation,
    // with 0 health. Anything else is a despawn, not a death.
    if (gPlayState == nullptr || !IsSyncedCategory(actor) || actor->colChkInfo.health != 0 || IsBeingUnloaded(actor)) {
        return;
    }
    SendKill(actor);
}

// Fired by ~50 enemies right when they're beaten (before the death animation),
// so the others see it sooner.
void OnEnemyDefeat(void* ptr) {
    SendKill(static_cast<Actor*>(ptr));
}

// Someone else killed this one: if our copy is loaded, kill it with a big hit
// so it plays its own death (and drops its own loot, for us).
void ApplyKill(const Coop::EnemyKill& kill) {
    sDead.insert(kill);
    if (gPlayState->sceneNum != kill.sceneNum || CurrentLayer() != kill.layer) {
        return; // not here: the dead list stops it from spawning when we get there
    }
    Actor* actor = FindEnemy(kill.room, kill.listIndex, kill.actorId);
    if (actor == nullptr || actor->params != kill.params || actor->update == nullptr) {
        return;
    }
    EnemyInfo& info = sEnemies[actor];
    info.mirrored = false;
    info.released = true; // stop copying, its own code has to run the death
    info.forceKillIn = kForceKillFrames;
    ForceUncull(actor, info); // even if it's far from us, the death must play
    uint8_t health = actor->colChkInfo.health;
    actor->colChkInfo.health = 1; // whatever it had, the next hit kills
    Vec3s pos = { (s16)actor->world.pos.x, (s16)actor->world.pos.y, (s16)actor->world.pos.z };
    ReplayHit(actor, info, std::max<uint8_t>(health, 1), 0, DMG_SLASH, pos);
    SPDLOG_INFO("[Coop] Enemy killed by a teammate: actor {:#x} room {} index {}", kill.actorId, kill.room,
                kill.listIndex);
}

// Some enemies ignore hits in some states (blocking, burrowed...). If the
// remote kill didn't take, finish it by hand.
void TickForcedKills() {
    for (auto& [actor, info] : sEnemies) {
        if (info.forceKillIn > 0 && --info.forceKillIn == 0 && actor->update != nullptr &&
            actor->colChkInfo.health != 0) {
            Poof(actor);
            sApplyingRemote = true;
            actor->colChkInfo.health = 0;
            Actor_Kill(actor);
            sApplyingRemote = false;
        }
    }
}

// Forget the dead enemies of rooms nobody is in. Same as the normal game:
// leave a room and come back, its enemies are back (unless the room is
// "cleared", but that's a flag and flags are already synced).
void ForgetEmptyRooms() {
    std::vector<std::pair<int16_t, int16_t>> occupied; // scene, room (-1 = loading, unknown room)
    if (CoopGame::LocalState().inGame) {
        occupied.emplace_back(CoopGame::LocalState().sceneNum, CoopGame::LocalState().roomNum);
    }
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        if (p.state && p.state->inGame) {
            occupied.emplace_back(p.state->sceneNum, p.state->roomNum);
        }
    }
    std::erase_if(sDead, [&](const Coop::EnemyKill& k) {
        return std::none_of(occupied.begin(), occupied.end(), [&](const auto& o) {
            return o.first == k.sceneNum && (o.second == k.room || o.second == -1);
        });
    });
}

// Is this room list entry an enemy that's already dead for the group?
bool IsDead(PlayState* play, const ActorEntry* entry) {
    if (sDead.empty() || play->setupActorList == nullptr || entry < play->setupActorList ||
        entry >= play->setupActorList + play->numSetupActors) {
        return false;
    }
    Coop::EnemyKill key;
    key.sceneNum = play->sceneNum;
    key.layer = CurrentLayer();
    key.room = play->roomCtx.curRoom.num;
    key.listIndex = static_cast<int16_t>(entry - play->setupActorList);
    auto it = sDead.find(key);
    return it != sDead.end() && it->actorId == entry->id && it->params == entry->params;
}

// --- host: send our enemies ---

Coop::Vec3f32 ToNet(const Vec3f& v) {
    return { v.x, v.y, v.z };
}
Coop::Vec3i16 ToNet(const Vec3s& v) {
    return { v.x, v.y, v.z };
}

void SendHostStates() {
    Coop::EnemyStates out;
    out.sceneNum = gPlayState->sceneNum;
    out.layer = CurrentLayer();
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != nullptr; a = a->next) {
        int16_t index = GetActorListIndex(a);
        if (index < 0 || a->update == nullptr || out.enemies.size() >= Coop::kMaxEnemiesPerPacket) {
            continue;
        }
        EnemyInfo& info = sEnemies[a];
        ForceUncull(a, info);
        Coop::EnemyState e;
        e.room = a->room;
        e.listIndex = index;
        e.actorId = a->id;
        e.pos = ToNet(a->world.pos);
        e.focus = ToNet(a->focus.pos);
        e.scale = ToNet(a->scale);
        e.shapeRot = ToNet(a->shape.rot);
        e.worldRot = ToNet(a->world.rot);
        e.health = a->colChkInfo.health;
        e.submitted = info.submitted;
        info.submitted = 0;
        for (Collider* col : info.colliders) {
            if (col->actor == a && col->shape == COLSHAPE_QUAD && e.quadCorners.size() / 4 < Coop::kMaxEnemyQuads) {
                for (const Vec3f& corner : ((ColliderQuad*)col)->dim.quad) {
                    e.quadCorners.push_back(ToNet(corner));
                }
            }
        }
        if (info.skel != nullptr && info.skel->jointTable != nullptr && info.skel->limbCount <= Coop::kMaxEnemyJoints) {
            e.joints.resize(info.skel->limbCount);
            for (size_t i = 0; i < e.joints.size(); i++) {
                e.joints[i] = ToNet(info.skel->jointTable[i]);
            }
        }
        out.enemies.push_back(std::move(e));
    }
    CoopGame::Session().SendEnemyStates(out);
}

// --- client: copy the host's enemies ---

// Freeze our copy and make it look like the host's.
void ApplyHostState(Actor* actor, EnemyInfo& info, const Coop::EnemyState& s) {
    actor->world.pos = { s.pos.x, s.pos.y, s.pos.z };
    actor->focus.pos = { s.focus.x, s.focus.y, s.focus.z };
    actor->scale = { s.scale.x, s.scale.y, s.scale.z };
    actor->shape.rot = { s.shapeRot.x, s.shapeRot.y, s.shapeRot.z };
    actor->world.rot = { s.worldRot.x, s.worldRot.y, s.worldRot.z };
    actor->colChkInfo.health = s.health;
    if (info.skel != nullptr && info.skel->jointTable != nullptr && s.joints.size() == info.skel->limbCount) {
        for (size_t i = 0; i < s.joints.size(); i++) {
            info.skel->jointTable[i] = { s.joints[i].x, s.joints[i].y, s.joints[i].z };
        }
    }

    // put its colliders in the collision check like the host's enemy did
    size_t quad = 0;
    for (Collider* col : info.colliders) {
        if (col->actor != actor) {
            continue;
        }
        if (col->shape == COLSHAPE_CYLINDER) {
            Collider_UpdateCylinder(actor, (ColliderCylinder*)col);
        } else if (col->shape == COLSHAPE_QUAD) {
            if (quad * 4 + 4 > s.quadCorners.size()) {
                continue; // no fresh corners: an old sword position would hit from nowhere
            }
            Vec3f c[4];
            for (int i = 0; i < 4; i++) {
                const Coop::Vec3f32& v = s.quadCorners[quad * 4 + i];
                c[i] = { v.x, v.y, v.z };
            }
            Collider_SetQuadVertices((ColliderQuad*)col, &c[0], &c[1], &c[2], &c[3]);
            quad++;
        } else if (col->shape == COLSHAPE_TRIS) {
            continue; // triangle shapes are moved by the AI, we don't get them: better no hitbox than a wrong one
        }
        // spheres follow the skeleton when it's drawn, nothing to do for them
        if ((s.submitted & 2) && (col->acFlags & AC_ON)) {
            CollisionCheck_SetAC(gPlayState, &gPlayState->colChkCtx, col);
        }
        if ((s.submitted & 1) && (col->atFlags & AT_ON)) {
            CollisionCheck_SetAT(gPlayState, &gPlayState->colChkCtx, col);
        }
        if ((s.submitted & 4) && (col->ocFlags1 & OC1_ON)) {
            CollisionCheck_SetOC(gPlayState, &gPlayState->colChkCtx, col);
        }
    }
}

// Did our Link hit our copy last frame? Then tell the host. The hit sound and
// spark already happened locally, we add the red flash.
void ForwardLocalHits(Actor* actor, EnemyInfo& info) {
    bool hit = false;
    uint32_t dmgFlags = 0;
    Vec3s hitPos = { (s16)actor->world.pos.x, (s16)actor->world.pos.y, (s16)actor->world.pos.z };
    for (Collider* col : info.colliders) {
        if (col->actor != actor || !(col->acFlags & AC_HIT)) {
            continue;
        }
        hit = true;
        ForEachElement(col, [&](ColliderInfo* e) {
            if (dmgFlags == 0 && (e->bumperFlags & BUMP_HIT) && e->acHitInfo != nullptr) {
                dmgFlags = e->acHitInfo->toucher.dmgFlags;
                hitPos = e->bumper.hitPos;
            }
            e->bumperFlags &= ~BUMP_HIT;
        });
        col->acFlags &= ~AC_HIT; // one swing = one hit sent
    }
    if (!hit) {
        return;
    }
    Coop::EnemyHit h;
    h.sceneNum = gPlayState->sceneNum;
    h.room = actor->room;
    h.listIndex = GetActorListIndex(actor);
    h.actorId = actor->id;
    h.damage = actor->colChkInfo.damage;
    h.effect = actor->colChkInfo.damageEffect;
    h.dmgFlags = dmgFlags;
    h.hitPos = ToNet(hitPos);
    actor->colChkInfo.damage = 0;
    Actor_SetColorFilter(actor, 0x4000, 255, 0, 8);
    std::lock_guard lock(CoopGame::Mutex());
    CoopGame::Session().SendEnemyHit(h);
}

// --- host: enemies go after the nearest player, not only the host ---

// The game measures every enemy's distance to OUR Link right before its update.
// Point those numbers at the closest player instead (ours or a teammate's).
// Enemies that look at the player directly still only see the host, but most
// aggro / facing checks use these fields.
void AimAtNearestPlayer(Actor* actor) {
    std::vector<Actor*> puppets = CoopGame::VisiblePuppets();
    if (puppets.empty()) {
        return;
    }
    Actor* best = &GET_PLAYER(gPlayState)->actor;
    float bestXZ = actor->xzDistToPlayer;
    float bestY = actor->yDistToPlayer;
    float bestSq = actor->xyzDistToPlayerSq;
    for (Actor* p : puppets) {
        float xz = Actor_WorldDistXZToActor(actor, p);
        float y = Actor_HeightDiff(actor, p);
        float sq = SQ(xz) + SQ(y);
        if (sq < bestSq) {
            best = p;
            bestXZ = xz;
            bestY = y;
            bestSq = sq;
        }
    }
    actor->xzDistToPlayer = bestXZ;
    actor->yDistToPlayer = bestY;
    actor->xyzDistToPlayerSq = bestSq;
    actor->yawTowardsPlayer = Actor_WorldYawTowardActor(actor, best);
}

// --- shared health (bosses, or enemies nobody is copying) ---

bool SharesHealth(Actor* actor, const EnemyInfo& info) {
    if (actor->category == ACTORCAT_BOSS) {
        return true;
    }
    return !sHostStreaming && !info.mirrored;
}

void SendHealthDrop(Actor* actor, uint8_t damage) {
    Coop::EnemyHit h;
    h.sceneNum = gPlayState->sceneNum;
    h.room = actor->room;
    h.listIndex = GetActorListIndex(actor);
    h.actorId = actor->id;
    h.damage = damage;
    h.effect = actor->colChkInfo.damageEffect;
    h.hitPos = ToNet(Vec3s{ (s16)actor->world.pos.x, (s16)actor->world.pos.y, (s16)actor->world.pos.z });
    std::lock_guard lock(CoopGame::Mutex());
    CoopGame::Session().SendEnemyHit(h);
}

// After the enemy's update: did it lose health because of us? Tell the others.
void OnActorUpdate(void* ptr) {
    Actor* actor = static_cast<Actor*>(ptr);
    auto it = sEnemies.find(actor);
    if (it == sEnemies.end() || gPlayState == nullptr || GetActorListIndex(actor) < 0) {
        return;
    }
    EnemyInfo& info = it->second;
    uint8_t health = actor->colChkInfo.health;
    if (!info.healthKnown) {
        info.prevHealth = health;
        info.healthKnown = true;
        return;
    }
    if (info.expectedRemote > 0 && info.expectedTimer > 0 && --info.expectedTimer == 0) {
        // a hit we were told about never landed (enemy was blocking etc): take it anyway
        health = health > info.expectedRemote ? health - info.expectedRemote : 0;
        actor->colChkInfo.health = health;
        info.prevHealth = health;
        info.expectedRemote = 0;
    }
    if (health < info.prevHealth && SharesHealth(actor, info)) {
        uint8_t drop = info.prevHealth - health;
        uint8_t theirs = std::min(drop, info.expectedRemote);
        info.expectedRemote -= theirs;
        if (drop > theirs) {
            SendHealthDrop(actor, drop - theirs);
        }
    }
    info.prevHealth = health;
}

// --- the per-actor hook: runs right before each actor's update ---

void OnShouldActorUpdate(void* ptr, bool* should) {
    Actor* actor = static_cast<Actor*>(ptr);
    if (!*should || gPlayState == nullptr || !IsSyncedCategory(actor)) {
        return;
    }
    auto it = sEnemies.find(actor);
    if (it == sEnemies.end()) {
        return;
    }
    EnemyInfo& info = it->second;

    // hits sent to us, replayed right before the enemy looks for them
    for (const Coop::EnemyHit& h : info.pendingHits) {
        Vec3s pos = { h.hitPos.x, h.hitPos.y, h.hitPos.z };
        ReplayHit(actor, info, h.damage, h.effect, h.dmgFlags, pos);
        if (SharesHealth(actor, info)) {
            info.expectedRemote = (uint8_t)std::min<int>(info.expectedRemote + h.damage, 255);
            info.expectedTimer = kExpectedDamageFrames;
        }
    }
    info.pendingHits.clear();

    if (sHostStreaming && actor->category == ACTORCAT_ENEMY) {
        AimAtNearestPlayer(actor);
        return;
    }

    if (!sClientCopying || info.released || actor->category != ACTORCAT_ENEMY) {
        if (info.mirrored) {
            info.mirrored = false;
            if (!info.released) { // a dying one stays unculled so its death plays out
                RestoreCull(actor, info);
            }
        }
        return;
    }
    int16_t index = GetActorListIndex(actor);
    auto st = sHostStates.find({ actor->room, index });
    if (index < 0 || st == sHostStates.end() || st->second.actorId != actor->id) {
        // the host doesn't have this one loaded (other room): run it ourselves
        if (info.mirrored) {
            info.mirrored = false;
            RestoreCull(actor, info);
        }
        return;
    }
    if (!info.mirrored) {
        SPDLOG_INFO("[Coop] Copying the host's enemy {:#x} room {} index {} (host at {:.0f} {:.0f} {:.0f}, ours at "
                    "{:.0f} {:.0f} {:.0f})",
                    actor->id, actor->room, index, st->second.pos.x, st->second.pos.y, st->second.pos.z,
                    actor->world.pos.x, actor->world.pos.y, actor->world.pos.z);
    }
    info.mirrored = true;
    ForceUncull(actor, info);
    ForwardLocalHits(actor, info);
    ApplyHostState(actor, info, st->second);
    *should = false; // the host runs it, not us
}

// --- capture colliders and skeletons as enemies get set up ---

void OnColliderSetup(void* actorPtr, void* collider) {
    Actor* actor = static_cast<Actor*>(actorPtr);
    if (actor != nullptr && IsSyncedCategory(actor)) {
        sEnemies[actor].colliders.push_back(static_cast<Collider*>(collider));
    }
}

void OnColliderSubmit(void* colliderPtr, uint8_t kind) {
    Collider* col = static_cast<Collider*>(colliderPtr);
    if (col->actor == nullptr || kind > 2) {
        return;
    }
    auto it = sEnemies.find(col->actor);
    if (it != sEnemies.end()) {
        it->second.submitted |= static_cast<uint8_t>(1 << kind);
    }
}

void OnSkelAnimeInit(void* skel) {
    if (sPendingSkels.size() < 64) {
        sPendingSkels.push_back(static_cast<SkelAnime*>(skel));
    }
}

// a new actor is about to init: whatever we knew about that address is stale
void OnShouldActorInit(void* actorPtr, bool*) {
    sEnemies.erase(static_cast<Actor*>(actorPtr));
}

// init is done: the skeleton it set up lives inside its own memory
void OnActorInit(void* actorPtr) {
    Actor* actor = static_cast<Actor*>(actorPtr);
    if (IsSyncedCategory(actor)) {
        uintptr_t start = reinterpret_cast<uintptr_t>(actor);
        uintptr_t end = start + ActorDB::Instance->RetrieveEntry(actor->id).entry.instanceSize;
        EnemyInfo& info = sEnemies[actor];
        for (SkelAnime* s : sPendingSkels) {
            uintptr_t p = reinterpret_cast<uintptr_t>(s);
            if (p >= start && p < end && info.skel == nullptr) {
                info.skel = s;
            }
        }
    }
    sPendingSkels.clear();
}

void OnActorDestroy(void* actorPtr) {
    sEnemies.erase(static_cast<Actor*>(actorPtr));
}

} // namespace

namespace CoopGame {

void UpdateEnemies() {
    Coop::Session& session = Session();
    std::vector<Coop::EnemyKill> kills = session.TakeEnemyKills();
    std::vector<Coop::EnemyHit> hits = session.TakeEnemyHits();
    std::optional<Coop::EnemyStates> states = session.TakeEnemyStates();
    bool hosting = session.Role() == Coop::SessionRole::Hosting;
    bool client = session.Role() == Coop::SessionRole::Connected;

    if (!SessionActive() || !LocalState().inGame || gPlayState == nullptr) {
        sDead.clear();
        sHostStates.clear();
        sHostStreaming = false;
        sClientCopying = false;
        return;
    }

    // who's around
    bool clientHere = false;
    bool hostHere = false;
    for (const Coop::RemotePlayer& p : session.Players()) {
        if (p.state && p.state->inGame && p.state->sceneNum == gPlayState->sceneNum &&
            p.state->linkAge == LocalState().linkAge) { // different age = different layer = different enemies
            clientHere = clientHere || hosting;
            hostHere = hostHere || client; // a client's only peer is the host
        }
    }

    // host: stream our enemies while a client can see them
    bool wasStreaming = sHostStreaming;
    sHostStreaming = hosting && clientHere;
    if (sHostStreaming != wasStreaming) {
        SPDLOG_INFO("[Coop] {} our enemies to the teammate in this scene",
                    sHostStreaming ? "Now sending" : "Stopped sending");
    }
    if (sHostStreaming) {
        SendHostStates();
    } else if (wasStreaming) {
        for (auto& [actor, info] : sEnemies) {
            RestoreCull(actor, info);
        }
    }

    // client: keep the latest picture of the host's enemies
    sStatesAge++;
    if (states && (states->sceneNum != gPlayState->sceneNum || states->layer != CurrentLayer())) {
        // same scene but not the same version of it (day/night, age...): the
        // enemy lists don't match, everyone runs their own. Say it once.
        if (states->sceneNum == gPlayState->sceneNum && !sLayerMismatchLogged) {
            SPDLOG_INFO("[Coop] Host is in another version of this scene (their layer {}, ours {})", states->layer,
                        CurrentLayer());
            sLayerMismatchLogged = true;
            // we follow the host's time now, so reloading the scene fixes it.
            // Once per scene: if it's an age difference, reloading won't help.
            if (sReloadedInScene != gPlayState->sceneNum) {
                sReloadedInScene = gPlayState->sceneNum;
                RequestTeleportToHost();
            }
        }
    } else if (states) {
        sLayerMismatchLogged = false;
        sHostStates.clear();
        for (const Coop::EnemyState& e : states->enemies) {
            sHostStates[{ e.room, e.listIndex }] = e;
        }
        if (sStatesAge >= kStaleFrames || states->enemies.size() != sLastStateCount) {
            SPDLOG_INFO("[Coop] Host's enemies: {} in scene {:#x} layer {}", states->enemies.size(), states->sceneNum,
                        states->layer);
            sLastStateCount = states->enemies.size();
        }
        sStatesAge = 0;
    }
    sClientCopying = client && hostHere && sStatesAge < kStaleFrames;
    if (!sClientCopying) {
        sHostStates.clear();
    }

    // hits: queued on the enemy, replayed right before its next update
    for (const Coop::EnemyHit& h : hits) {
        if (h.sceneNum != gPlayState->sceneNum) {
            continue;
        }
        if (Actor* actor = FindEnemy(h.room, h.listIndex, h.actorId)) {
            sEnemies[actor].pendingHits.push_back(h);
        }
    }

    for (const Coop::EnemyKill& k : kills) {
        ApplyKill(k);
    }
    TickForcedKills();
    ForgetEmptyRooms();
}

void RegisterEnemyHooks() {
    COND_HOOK(OnActorKill, true, OnActorKill);
    COND_HOOK(OnEnemyDefeat, true, OnEnemyDefeat);
    COND_HOOK(ShouldActorUpdate, true, OnShouldActorUpdate);
    COND_HOOK(OnActorUpdate, true, OnActorUpdate);
    COND_HOOK(ShouldActorInit, true, OnShouldActorInit);
    COND_HOOK(OnActorInit, true, OnActorInit);
    COND_HOOK(OnActorDestroy, true, OnActorDestroy);
    COND_HOOK(OnColliderSetup, true, OnColliderSetup);
    COND_HOOK(OnColliderSubmit, true, OnColliderSubmit);
    COND_HOOK(OnSkelAnimeInit, true, OnSkelAnimeInit);
    // room is loading its actors: skip the ones the group already killed
    COND_VB_SHOULD(VB_SPAWN_ACTOR_ENTRY, true, {
        va_arg(args, ActorContext*);
        ActorEntry* entry = va_arg(args, ActorEntry*);
        PlayState* play = va_arg(args, PlayState*);
        Actor** spawned = va_arg(args, Actor**);
        if (*should && IsDead(play, entry)) {
            SPDLOG_INFO("[Coop] Enemy {:#x} already killed by the group, not spawning it", entry->id);
            *should = false;
            *spawned = nullptr; // the game doesn't set it when we say no
        }
    });
}

} // namespace CoopGame
