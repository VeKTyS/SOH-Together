// Enemy sync.
//
// Same enemy on every game: enemies placed in a level come from the room's
// actor list, and SoH remembers their position in that list (ActorListIndex).
// Same game data on both sides = same numbers. So an enemy is
// scene + layer + room + list index.
//
// Enemies spawned on the fly by other enemies' AI (Stalchildren, Gohma larvas,
// Peahat larvas, Biris...) don't have a list index. The host gives them a number
// ("dynamic id") and tells the clients to spawn the same one (ActorSpawn event),
// then streams them like the others. On the wire a dynamic id travels in the
// same listIndex field: listIndex -1 = nothing, listIndex <= -2 = dynamic id
// (-2 - id). Copied from TrueCoop EnemySync.cpp (dynamic keys, bit 63 there).
//
// Who runs the enemies: the host. When a client is in the same scene as the
// host, the client's copies are frozen and just copy what the host sends every
// frame (position, skeleton, health, which colliders are on). When the client
// hits one, the hit goes to the host, which replays it on its enemy through the
// enemy's own damage code (so shields, weak spots and i-frames still work).
// Same idea as OOT True Co-op (TrueCoop's EnemySync "M2 mirroring").
//
// When nobody is copying (client alone in a scene, Floormasters, or bosses
// without an adapter in CoopBosses.cpp), everyone runs their own AI and we share
// the health instead: damage done on one game is replayed on the others.
//
// Deaths: when an enemy dies somewhere, it dies everywhere, and everyone gets
// their own death animation and their own loot (loot is per player).
// Two kinds (TrueCoop Packets/EnemyDied.cpp "permanent"):
// - Actor_Kill with 0 health: a real death. Goes in the dead list, and if the
//   other copy refuses to die it's killed by hand after a while.
// - the "defeat" hook alone: enemies like Deku Babas regrow from the same actor,
//   so the others just replay the death, no dead list, no forced kill.
//
// Needs 3 hooks we add to SoH (patches/soh-coop-hooks.patch): OnColliderSetup,
// OnColliderSubmit and OnSkelAnimeInit, to know an enemy's colliders and skeleton.
//
// Menu switches (CVars, like TrueCoop's):
//   Coop.EnemySync        (1) everything in this file
//   Coop.EnemyMirroring   (1) host-runs-everything copying; off = shared health only
//   Coop.EnemySyncVerbose (0) chatty log

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/cvar_prefixes.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;
}

// libultra's r4300.h has a U32(x) macro (pulled in by the headers above), it breaks ByteWriter::U32
#undef U32

// Boss adapters live in CoopBosses.cpp (TrueCoop BossAdapters/*).
namespace CoopBosses {
// is there an adapter for this actor id (Gohma, Gohma larva)
bool HasAdapter(int16_t actorId);
// host: the boss's phase right now (0 if the adapter has no phases), fits in 5 bits
uint8_t GetPhase(Actor* actor);
// client: the host's boss changed phase. True = stop copying it for good, its own code runs now
bool OnPhaseChange(Actor* actor, uint8_t fromPhase, uint8_t toPhase);
// client: copy it this frame? (false during intros etc: everyone runs their own)
bool ShouldMirror(Actor* actor, uint8_t streamedPhase);
// client: after the body copy, force the fields the adapter cares about
void ApplyExtras(Actor* actor, const std::vector<uint8_t>& body);
// body bytes never copied for this actor (offsets from the actor start)
void AddNoCopyRanges(Actor* actor, std::vector<std::pair<size_t, size_t>>& out);
} // namespace CoopBosses

namespace {

// The room an actor spawned in. A pot sets actor.room to -1 while carried and to
// the thrower's room once thrown, so keys use this one (same idea as CoopThings).
struct EnemySpawnRoom {
    int8_t room = 0;
};
ObjectExtension::Register<EnemySpawnRoom> sEnemySpawnRoomRegister;

int8_t SpawnRoom(Actor* a) {
    EnemySpawnRoom* r = ObjectExtension::GetInstance().Get<EnemySpawnRoom>(a);
    return r != nullptr ? r->room : a->room;
}

// --- tuning ---

// a client stops copying an enemy if the host hasn't talked about it for this long
constexpr int kStaleFrames = 30;
// a remote kill that the enemy's own code didn't finish by then gets forced
constexpr int kForceKillFrames = 60;
// shared health: damage we were told about but the enemy didn't take by then is forced
constexpr int kExpectedDamageFrames = 40;
// after a death was sent / received, the same enemy's death isn't sent again (TrueCoop DEATH_COOLDOWN_FRAMES)
constexpr int kDeathCooldownFrames = 60;
// a destroyed dynamic enemy can't be spawned again by a late re-announce (TrueCoop RECENTLY_DEAD_TTL_FRAMES)
constexpr int kRecentlyDeadFrames = 100;
// the host re-announces its living dynamic enemies this often (TrueCoop SPAWN_REBROADCAST_TICKS)
constexpr uint64_t kSpawnRebroadcastFrames = 60;
// dynamic ids go 0..kMaxDynId, then wrap
constexpr int kMaxDynId = 0x7FFD;

// --- switches, read once per frame (reading CVars in every actor hook is slow) ---

bool sActive = false;  // session on, in game, sync switch on
bool sMirrorOn = true; // Coop.EnemyMirroring
bool sMirrorWasOn = true;
bool sVerbose = false; // Coop.EnemySyncVerbose
bool sHosting = false;
bool sClient = false;
bool sHostHere = false; // client: the host is in our scene, same age
uint64_t sTick = 0;

// copied from TrueCoop EnemySync.h
#define ESYNC_LOG(...)                \
    do {                              \
        if (sVerbose) {               \
            SPDLOG_INFO(__VA_ARGS__); \
        }                             \
    } while (0)

// --- what we know about each enemy actor ---

struct EnemyInfo {
    std::vector<Collider*> colliders; // in setup order, same order on every game
    SkelAnime* skel = nullptr;
    uint8_t submitted = 0; // host: colliders used since the last send (1 AT, 2 AC, 4 OC)
    bool cullSaved = false;
    uint32_t savedCullFlags = 0;
    int16_t spawnParams = 0; // params before its init ran (some enemies use params as a timer later)
    bool spawnParamsKnown = false;
    // client copying it
    bool mirrored = false;
    bool released = false; // dying: let its own code play the death
    int forceKillIn = 0;
    uint8_t phase = 0; // boss adapter phase seen last (client) / sent last (host)
    // deaths
    int deathCooldown = 0; // a death was just sent or received (TrueCoop deathCooldown)
    bool killSent = false; // a real death (Actor_Kill) was sent or received
    bool remoteDeath = false; // dying because a teammate's copy died: they report the real death, not us
    // host: hits from clients, applied right before the enemy's update
    std::vector<Coop::EnemyHit> pendingHits;
    // shared health mode
    uint8_t prevHealth = 0;
    bool healthKnown = false;
    uint8_t expectedRemote = 0; // damage we caused because someone told us to
    int expectedTimer = 0;
    uint8_t healthCap = 255; // shared health: someone had at most this much before their hit
};

std::unordered_map<Actor*, EnemyInfo> sEnemies;
// skeletons are set up inside the actor's init, before we know who's who.
// Kept here until OnActorInit says which actor owns them.
std::vector<SkelAnime*> sPendingSkels;

// --- dynamic enemies (spawned at runtime, TrueCoop EnemySync.cpp:1296-1383) ---

struct DynEnemy {
    Actor* actor = nullptr;
    int16_t actorId = 0; // identity at spawn (params change at runtime)
    int16_t params = 0;
    bool needsAnnounce = false; // host: tell the clients about it
};
std::unordered_map<int, DynEnemy> sDyn;     // dynamic id -> actor (host: ours, client: the host's numbers)
std::unordered_map<Actor*, int> sDynIdOf;   // actor -> dynamic id
std::unordered_map<int, int> sRecentlyDead; // dynamic id -> frames left
int sNextDynId = 0;
bool sSpawningRemote = false; // we're spawning something a teammate told us about

// host: enemy projectiles to announce once (Deku nut, Octorok rock)
std::vector<Actor*> sPendingProjectiles;
// client: enemies our own AI spawned while copying the host. Checked at their first update.
std::unordered_set<Actor*> sLocalSpawns;

int16_t DynIndex(int id) {
    return static_cast<int16_t>(-2 - id);
}
int DynIdFromIndex(int16_t index) {
    return -2 - index; // only for index <= -2
}

// copying the enemy's own memory, further down
void ReadBody(Actor* actor, const EnemyInfo& info, Coop::EnemyState& e);
void WriteBody(Actor* actor, const EnemyInfo& info, const Coop::EnemyState& e);

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
int16_t sLastRosterRoom = -1;  // client: room we last asked the host's roster for

// dynamic enemies aren't tied to a room: same key whatever room they wander in
RoomIndex StateKey(int8_t room, int16_t index) {
    return { index <= -2 ? (int8_t)0 : room, index };
}

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

// host: puppets of downed teammates, enemies don't go after them
std::unordered_set<Actor*> sAggroSkip;

// fake "thing that hit you" handed to enemies when we replay a hit. Enemies read
// it during their update (a frame later), so it has to stay alive a bit: ring.
ColliderInfo sFakeHitters[8];
uint8_t sFakeHitterNext = 0;

// --- small helpers ---

bool IsSyncedCategory(Actor* actor) {
    return actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS;
}

// Pots, bushes, rocks, crates: broken by one player = broken for everyone, and
// everyone gets their own loot (they break with their own code on each game).
bool IsBreakableId(int16_t id) {
    switch (id) {
        case ACTOR_OBJ_TSUBO:
        case ACTOR_EN_KUSA:
        case ACTOR_EN_ISHI:
        case ACTOR_OBJ_KIBAKO:
        case ACTOR_OBJ_KIBAKO2:
            return true;
        default:
            return false;
    }
}

bool IsKillSynced(Actor* actor) {
    return IsSyncedCategory(actor) || (IsBreakableId(actor->id) && CoopGame::RoomSyncWorldObjects());
}

// Enemy projectiles both games fire on their own (TrueCoop EnemySync.cpp:213-224):
// the Deku nut, and the Octorok rock (same id as the Octorok, but the rock turns
// itself into a PROP in its init). Spawned once on each side, never copied.
bool IsSyncedProjectile(Actor* actor) {
    return actor->id == ACTOR_EN_NUTSBALL || (actor->id == ACTOR_EN_OKUTA && actor->category == ACTORCAT_PROP);
}

// Gohma larva params >= 6: egg shell pieces and pieces of the dead Gohma, short
// local effects spawned on both games. Never tracked (TrueCoop EnemySync.cpp:226-232).
bool IsTrackingExcluded(Actor* actor) {
    return actor->id == ACTOR_EN_GOMA && (uint16_t)actor->params >= 6;
}

// Floormasters split and merge with pointers between 3 actors: copying them could
// crash when packets get lost. They stay local, health still shared (TrueCoop:234-239).
bool IsMirrorBlocked(Actor* actor) {
    return actor->id == ACTOR_EN_FLOORMAS;
}

// Can the host run it and a client copy it? Enemies yes, bosses only with an
// adapter (CoopBosses.cpp), the others share their health.
bool IsMirrorable(Actor* actor) {
    if (IsMirrorBlocked(actor) || IsTrackingExcluded(actor)) {
        return false;
    }
    return actor->category == ACTORCAT_ENEMY ||
           (actor->category == ACTORCAT_BOSS && CoopBosses::HasAdapter(actor->id));
}

// A boss we don't copy is fighting here: its minions are its own business on
// each game, don't replicate or remove them.
bool LocalBossFight() {
    if (gPlayState == nullptr) {
        return false;
    }
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head; a != nullptr; a = a->next) {
        if (a->update != nullptr && !CoopBosses::HasAdapter(a->id)) {
            return true;
        }
    }
    return false;
}

// Most bushes don't die when cut: they're marked as cut, or (when thrown) go
// back home small, then grow back. Only "type 0" ones die. So for the others
// we watch for "just got cut or small" instead of a kill.
bool IsRegrowingBush(int16_t id, int16_t params) {
    return id == ACTOR_EN_KUSA && (params & 3) != 0;
}

bool IsBushDown(Actor* a) {
    return (a->flags & ACTOR_FLAG_GRASS_DESTROYED) || a->scale.y < 0.3f; // full size is 0.4, small is 0.16
}

std::unordered_map<Actor*, bool> sBushDown;         // bush -> was down last update
std::unordered_map<Actor*, int> sBushCutByTeammate; // bush -> updates left to see the cut we replayed

bool SessionActive() {
    Coop::SessionRole role = CoopGame::Session().Role();
    return role == Coop::SessionRole::Hosting || role == Coop::SessionRole::Connected;
}

uint8_t CurrentLayer() {
    return static_cast<uint8_t>(gSaveContext.sceneLayer);
}

// The network id of an actor: its room list index, or its dynamic id
// (-2 - id), or -1 if it has neither.
int16_t KeyIndex(Actor* actor) {
    int16_t index = GetActorListIndex(actor);
    if (index >= 0) {
        return index;
    }
    auto it = sDynIdOf.find(actor);
    return it != sDynIdOf.end() ? DynIndex(it->second) : -1;
}

// Builds the network id of a live actor. False if it's not one we sync.
bool MakeKey(Actor* actor, Coop::EnemyKill& out) {
    if (gPlayState == nullptr || !IsKillSynced(actor) || CoopGame::IsPuppetFairy(actor)) {
        return false;
    }
    int16_t index = KeyIndex(actor);
    if (index == -1) {
        return false; // spawned on the fly, and nobody numbered it
    }
    out.sceneNum = gPlayState->sceneNum;
    out.layer = CurrentLayer();
    out.room = index <= -2 ? 0 : SpawnRoom(actor);
    out.listIndex = index;
    out.actorId = actor->id;
    out.params = actor->params;
    if (index <= -2) {
        if (auto d = sDyn.find(DynIdFromIndex(index)); d != sDyn.end()) {
            out.params = d->second.params;
        }
    } else if (auto it = sEnemies.find(actor); it != sEnemies.end() && it->second.spawnParamsKnown) {
        out.params = it->second.spawnParams;
    }
    return true;
}

Actor* FindEnemy(int8_t room, int16_t index, int16_t actorId) {
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->id == actorId && GetActorListIndex(a) == index && SpawnRoom(a) == room) {
                return a;
            }
        }
    }
    return nullptr;
}

// Same, but also knows dynamic ids. Dynamic ids are exact: no match = it's
// gone (or not spawned here yet), never guess (TrueCoop FindActorForPacket).
Actor* FindSynced(int8_t room, int16_t index, int16_t actorId) {
    if (index == -1) {
        return nullptr;
    }
    if (index <= -2) {
        auto it = sDyn.find(DynIdFromIndex(index));
        if (it == sDyn.end() || it->second.actor->id != actorId) {
            return nullptr;
        }
        return it->second.actor;
    }
    return FindEnemy(room, index, actorId);
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

// Start / stop copying. The health watcher is pinned to the copied health so
// stopping never looks like a hit we did (TrueCoop Begin/EndSuppression).
void StartMirroring(Actor* actor, EnemyInfo& info) {
    info.mirrored = true;
    info.prevHealth = actor->colChkInfo.health;
    info.expectedRemote = 0;
    info.expectedTimer = 0;
    ForceUncull(actor, info);
}

void StopMirroring(Actor* actor, EnemyInfo& info) {
    if (!info.mirrored) {
        return;
    }
    info.mirrored = false;
    info.prevHealth = actor->colChkInfo.health;
    info.expectedRemote = 0;
    info.expectedTimer = 0;
    if (!info.released) { // a dying one stays unculled so its death plays out
        RestoreCull(actor, info);
    }
}

// Everything goes back to normal (mirroring switched off, session over).
// TrueCoop EnemySync.cpp:1067-1080, the "sync-off" sweep.
void StopAllMirroring() {
    for (auto& [actor, info] : sEnemies) {
        StopMirroring(actor, info);
        if (!info.released) {
            RestoreCull(actor, info);
        }
    }
    sHostStates.clear();
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

// Kill one quietly: no death, no loot, nothing sent (ghosts, despawns).
void QuietKill(Actor* actor) {
    EnemyInfo& info = sEnemies[actor];
    info.deathCooldown = kDeathCooldownFrames;
    info.killSent = true;
    sApplyingRemote = true;
    Actor_Kill(actor);
    sApplyingRemote = false;
}

// --- event bytes (our own EventKinds: ActorSpawn, ActorDespawn, EnemyRoster) ---

void WriteF32(Coop::ByteWriter& w, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    w.U32(u);
}

bool ReadF32(Coop::ByteReader& r, float& f) {
    uint32_t u;
    if (!r.U32(u)) {
        return false;
    }
    memcpy(&f, &u, 4);
    return std::isfinite(f) && std::fabs(f) <= Coop::kMaxCoordinate;
}

void WriteKey(Coop::ByteWriter& w, const Coop::EnemyKill& k) {
    w.U16(static_cast<uint16_t>(k.sceneNum));
    w.U8(k.layer);
    w.U8(static_cast<uint8_t>(k.room));
    w.U16(static_cast<uint16_t>(k.listIndex));
    w.U16(static_cast<uint16_t>(k.actorId));
    w.U16(static_cast<uint16_t>(k.params));
}

bool ReadKey(Coop::ByteReader& r, Coop::EnemyKill& k) {
    uint16_t scene, index, id, params;
    uint8_t room;
    if (!r.U16(scene) || !r.U8(k.layer) || !r.U8(room) || !r.U16(index) || !r.U16(id) || !r.U16(params)) {
        return false;
    }
    k.sceneNum = static_cast<int16_t>(scene);
    k.room = static_cast<int8_t>(room);
    k.listIndex = static_cast<int16_t>(index);
    k.actorId = static_cast<int16_t>(id);
    k.params = static_cast<int16_t>(params);
    return true;
}

bool IsHereKey(const Coop::EnemyKill& k) {
    return gPlayState != nullptr && k.sceneNum == gPlayState->sceneNum && k.layer == CurrentLayer();
}

// ActorSpawn event: first byte
enum SpawnOp : uint8_t { kSpawnEnemy = 0, kSpawnProjectile = 1 };
// ActorDespawn event: first byte
enum DespawnOp : uint8_t { kDespawn = 0, kDefeat = 1 };
// EnemyRoster event: first byte
enum RosterOp : uint8_t { kRosterRequest = 0, kRosterReply = 1, kHealthCap = 2 };

// --- deaths ---

// A death to tell the others about. permanent = Actor_Kill with 0 health (it's
// really gone), else the defeat hook (it may regrow). TrueCoop OnEnemyActorKill /
// OnEnemyDefeated + Packets/EnemyDied.cpp.
void SendDeath(Actor* actor, bool permanent) {
    if (sApplyingRemote || !sActive) {
        return;
    }
    Coop::EnemyKill kill;
    if (!MakeKey(actor, kill)) {
        return; // not ours to sync
    }
    EnemyInfo& info = sEnemies[actor];
    if (info.killSent || (permanent && info.remoteDeath)) {
        return; // already gone for good, or the teammate who killed it tells everyone
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (!SessionActive()) {
        return;
    }
    if (permanent) {
        // dead list for room ones only: dynamic ones don't come back with the room
        if (kill.listIndex >= 0 && !sDead.insert(kill).second) {
            return;
        }
        info.killSent = true;
        info.deathCooldown = kDeathCooldownFrames;
        CoopGame::Session().SendEnemyKill(kill);
        SPDLOG_INFO("[Coop] Enemy killed: actor {:#x} room {} index {}", kill.actorId, kill.room, kill.listIndex);
        return;
    }
    if (info.deathCooldown > 0) {
        return; // defeat caused by a death someone sent us (or sent twice): don't echo
    }
    info.deathCooldown = kDeathCooldownFrames;
    Coop::ByteWriter w;
    w.U8(kDefeat);
    WriteKey(w, kill);
    CoopGame::SendEvent(Coop::EventKind::ActorDespawn, w.Take());
    ESYNC_LOG("[Coop] Enemy defeated (may regrow): actor {:#x} room {} index {}", kill.actorId, kill.room,
              kill.listIndex);
}

// Host: a dynamic enemy left without dying (room unloaded, burrowed away...):
// the clients remove their copy too (TrueCoop Packets/EnemyDespawn.cpp).
void SendDespawn(Actor* actor) {
    Coop::EnemyKill key;
    if (sApplyingRemote || !MakeKey(actor, key) || key.listIndex > -2) {
        return;
    }
    EnemyInfo& info = sEnemies[actor];
    if (info.deathCooldown > 0 || info.killSent) {
        return;
    }
    info.deathCooldown = kDeathCooldownFrames;
    Coop::ByteWriter w;
    w.U8(kDespawn);
    WriteKey(w, key);
    CoopGame::SendEvent(Coop::EventKind::ActorDespawn, w.Take());
    ESYNC_LOG("[Coop] Dynamic enemy {} despawned", DynIdFromIndex(key.listIndex));
}

// Leaving a room also calls Actor_Kill on its enemies: that's not a death.
bool IsBeingUnloaded(Actor* actor) {
    const RoomContext& rooms = gPlayState->roomCtx;
    return actor->room >= 0 && actor->room != rooms.curRoom.num && actor->room != rooms.prevRoom.num;
}

void SendBushCut(Actor* bush) {
    Coop::EnemyKill cut;
    if (sApplyingRemote || !MakeKey(bush, cut)) {
        return;
    }
    std::lock_guard lock(CoopGame::Mutex());
    if (SessionActive()) {
        CoopGame::Session().SendEnemyKill(cut); // no dead list: it grows back
    }
}

// After a bush's update: did it just get cut? Tell the others (unless it's the
// cut a teammate sent us).
void TrackBush(Actor* bush) {
    bool down = IsBushDown(bush);
    bool& wasDown = sBushDown.try_emplace(bush, down).first->second;
    auto teammate = sBushCutByTeammate.find(bush);
    if (down && !wasDown) {
        if (teammate != sBushCutByTeammate.end()) {
            bush->world.pos = bush->home.pos; // a teammate may have thrown ours away: grow back at home
            sBushCutByTeammate.erase(teammate);
        } else {
            SendBushCut(bush);
        }
    } else if (teammate != sBushCutByTeammate.end() && --teammate->second <= 0) {
        sBushCutByTeammate.erase(teammate); // the hit never took
    }
    wasDown = down;
}

void OnActorKill(void* ptr) {
    Actor* actor = static_cast<Actor*>(ptr);
    // most enemies only call Actor_Kill at the end of their death animation,
    // with 0 health. Anything else is a despawn, not a death.
    if (!sActive || gPlayState == nullptr || !IsKillSynced(actor) || CoopGame::IsPuppetFairy(actor)) {
        return;
    }
    bool dynamic = sDynIdOf.contains(actor);
    if (IsSyncedCategory(actor) && actor->colChkInfo.health != 0) {
        // an enemy leaving without dying. A dynamic one on the host: the clients drop theirs.
        if (dynamic && sHosting && sMirrorOn) {
            SendDespawn(actor);
        }
        return;
    }
    if (!dynamic && IsBeingUnloaded(actor)) {
        return;
    }
    SendDeath(actor, true); // a thrown "type 2" bush lands here: the others cut theirs (ApplyKill)
}

// Fired by ~50 enemies right when they're beaten (before the death animation),
// so the others see it sooner. Not a "for good" death: some regrow (Deku Babas).
void OnEnemyDefeat(void* ptr) {
    Actor* actor = static_cast<Actor*>(ptr);
    if (actor == nullptr || !sActive) {
        return;
    }
    if (auto it = sEnemies.find(actor); it != sEnemies.end()) {
        it->second.forceKillIn = 0; // it's visibly dying now, never force-kill on top (TrueCoop:1575)
    }
    SendDeath(actor, false);
}

// Someone else killed this one: if our copy is loaded, kill it with a big hit
// so it plays its own death (and drops its own loot, for us).
void ApplyBushCut(const Coop::EnemyKill& cut) {
    if (gPlayState->sceneNum != cut.sceneNum || CurrentLayer() != cut.layer) {
        return;
    }
    Actor* bush = FindEnemy(cut.room, cut.listIndex, cut.actorId);
    if (bush == nullptr || bush->update == nullptr || IsBushDown(bush)) {
        return; // not loaded, or already cut here too
    }
    sBushCutByTeammate[bush] = kForceKillFrames;
    Vec3s pos = { (s16)bush->world.pos.x, (s16)bush->world.pos.y, (s16)bush->world.pos.z };
    ReplayHit(bush, sEnemies[bush], 1, 0, DMG_SLASH, pos); // it cuts itself: pieces, sound, its own loot
}

// Stop copying and hit it hard enough to die, its own code plays the death.
void ReleaseAndKill(Actor* actor, EnemyInfo& info) {
    StopMirroring(actor, info);
    info.released = true;     // stop copying, its own code has to run the death
    ForceUncull(actor, info); // even if it's far from us, the death must play
    uint8_t health = actor->colChkInfo.health;
    actor->colChkInfo.health = 1; // whatever it had, the next hit kills
    info.prevHealth = 1;
    Vec3s pos = { (s16)actor->world.pos.x, (s16)actor->world.pos.y, (s16)actor->world.pos.z };
    uint32_t dmg = IsBreakableId(actor->id) ? (DMG_EXPLOSIVE | DMG_HAMMER | DMG_SLASH) : DMG_SLASH;
    ReplayHit(actor, info, std::max<uint8_t>(health, 1), 0, dmg, pos);
}

// A real death (Actor_Kill) somewhere else.
void ApplyKill(const Coop::EnemyKill& kill) {
    if (IsRegrowingBush(kill.actorId, kill.params)) {
        ApplyBushCut(kill);
        return;
    }
    if (kill.listIndex >= 0) {
        sDead.insert(kill);
    }
    if (gPlayState->sceneNum != kill.sceneNum || CurrentLayer() != kill.layer) {
        return; // not here: the dead list stops it from spawning when we get there
    }
    Actor* actor = FindSynced(kill.room, kill.listIndex, kill.actorId);
    if (actor == nullptr || actor->update == nullptr) {
        return;
    }
    EnemyInfo& info = sEnemies[actor];
    int16_t params = info.spawnParamsKnown ? info.spawnParams : actor->params;
    if (kill.listIndex >= 0 && params != kill.params) {
        return;
    }
    info.killSent = true; // our own Actor_Kill later won't send it back
    info.remoteDeath = true;
    info.deathCooldown = kDeathCooldownFrames;
    if (!info.mirrored && actor->colChkInfo.health == 0) {
        return; // already dying here on its own: let it finish
    }
    // A boss that's already playing its defeat (adapter handoff, or beaten here
    // too): leave it alone, a forced kill would skip the heart and the blue warp.
    // TrueCoop Packets/EnemyDied.cpp:73-79.
    if (actor->category == ACTORCAT_BOSS && (info.released || actor->colChkInfo.health == 0)) {
        return;
    }
    if (!info.released) {
        ReleaseAndKill(actor, info);
        SPDLOG_INFO("[Coop] Enemy killed by a teammate: actor {:#x} room {} index {}", kill.actorId, kill.room,
                    kill.listIndex);
    }
    // Only real deaths get forced in the end (TrueCoop ApplyRemoteDeath "permanent").
    // Bosses never: their own death sequence finishes the job.
    if (info.forceKillIn == 0 && actor->category != ACTORCAT_BOSS) {
        info.forceKillIn = kForceKillFrames;
    }
}

// A "defeat hook" death somewhere else: replay it, but no dead list and no
// forced kill (it may regrow over there). TrueCoop ApplyRemoteDeath(permanent=false).
void ApplyDefeat(const Coop::EnemyKill& kill) {
    if (!IsHereKey(kill)) {
        return;
    }
    Actor* actor = FindSynced(kill.room, kill.listIndex, kill.actorId);
    if (actor == nullptr) {
        return;
    }
    EnemyInfo& info = sEnemies[actor];
    if (info.deathCooldown > 0 || info.killSent) {
        return; // already died here (or got it already)
    }
    info.deathCooldown = kDeathCooldownFrames; // our own defeat hook won't send it back
    if (actor->update == nullptr || (actor->category == ACTORCAT_BOSS && info.released)) {
        return;
    }
    if (!info.mirrored && actor->colChkInfo.health == 0) {
        return; // already dying here on its own
    }
    info.remoteDeath = true;
    ReleaseAndKill(actor, info);
    ESYNC_LOG("[Coop] Enemy defeated by a teammate: actor {:#x} room {} index {}", kill.actorId, kill.room,
              kill.listIndex);
}

// Some enemies ignore hits in some states (blocking, burrowed...). If the
// remote kill didn't take, finish it by hand. Also counts down the death cooldowns.
void TickDeaths() {
    for (auto& [actor, info] : sEnemies) {
        if (info.deathCooldown > 0) {
            info.deathCooldown--;
        }
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

// --- dynamic spawns (TrueCoop EnemySync.cpp:1318-1396, Packets/EnemySpawn.cpp) ---

// Fires inside Actor_Spawn. Room list actors spawn while numSetupActors != 0;
// anything else is spawned by some AI at runtime.
void OnActorSpawn(void* ptr) {
    Actor* actor = static_cast<Actor*>(ptr);
    if (!sActive || !sMirrorOn || gPlayState == nullptr || sSpawningRemote || CoopGame::IsPuppetFairy(actor)) {
        return;
    }
    if (gPlayState->numSetupActors != 0) {
        return;
    }
    if (IsSyncedProjectile(actor)) {
        if (sHosting) {
            sPendingProjectiles.push_back(actor); // announced once, at the end of the frame
        }
        return;
    }
    if (!IsSyncedCategory(actor) || !IsMirrorable(actor) || LocalBossFight()) {
        return;
    }
    if (sHosting) {
        int id = sNextDynId;
        sNextDynId = sNextDynId >= kMaxDynId ? 0 : sNextDynId + 1;
        sDyn[id] = DynEnemy{ actor, actor->id, actor->params, true };
        sDynIdOf[actor] = id;
        ESYNC_LOG("[Coop] Dynamic enemy {:#x} got id {}", actor->id, id);
    } else if (sClient && sClientCopying) {
        sLocalSpawns.insert(actor); // decided at its first update, once its parent is known
    }
}

// Client: our own game spawned an enemy while we copy the host. The host spawns
// the same one and sends it to us, so ours would be a double. Only when its
// spawner is something the host runs too (a copied enemy, a frozen En_Encount1,
// or a plain Actor_Spawn with no parent, like a Bari splitting into Biris).
bool ShouldDropLocalSpawn(Actor* actor) {
    if (!sClientCopying || LocalBossFight()) {
        return false;
    }
    Actor* parent = actor->parent;
    if (parent == nullptr || parent->id == ACTOR_EN_ENCOUNT1) {
        return true;
    }
    auto it = sEnemies.find(parent);
    return it != sEnemies.end() && (it->second.mirrored || it->second.released) && !IsMirrorBlocked(parent);
}

void SendSpawnEvent(Actor* actor, const DynEnemy& d, int id) {
    Coop::EnemyKill key;
    key.sceneNum = gPlayState->sceneNum;
    key.layer = CurrentLayer();
    key.room = actor->room;
    key.listIndex = DynIndex(id);
    key.actorId = d.actorId;
    key.params = d.params;
    Coop::ByteWriter w;
    w.U8(kSpawnEnemy);
    WriteKey(w, key);
    WriteF32(w, actor->world.pos.x);
    WriteF32(w, actor->world.pos.y);
    WriteF32(w, actor->world.pos.z);
    w.U16(static_cast<uint16_t>(actor->shape.rot.x));
    w.U16(static_cast<uint16_t>(actor->shape.rot.y));
    w.U16(static_cast<uint16_t>(actor->shape.rot.z));
    // parent: some children read it without checking (Gohma larvas write into their mother)
    int16_t parentIndex = actor->parent != nullptr ? KeyIndex(actor->parent) : -1;
    if (parentIndex != -1) {
        w.U8(1);
        w.U8(static_cast<uint8_t>(parentIndex <= -2 ? 0 : actor->parent->room));
        w.U16(static_cast<uint16_t>(parentIndex));
        w.U16(static_cast<uint16_t>(actor->parent->id));
    } else {
        w.U8(0);
    }
    CoopGame::SendEvent(Coop::EventKind::ActorSpawn, w.Take());
}

void SendProjectileEvent(Actor* actor) {
    Coop::EnemyKill key;
    key.sceneNum = gPlayState->sceneNum;
    key.layer = CurrentLayer();
    key.room = actor->room;
    key.listIndex = -1;
    key.actorId = actor->id;
    key.params = actor->params;
    Coop::ByteWriter w;
    w.U8(kSpawnProjectile);
    WriteKey(w, key);
    WriteF32(w, actor->world.pos.x);
    WriteF32(w, actor->world.pos.y);
    WriteF32(w, actor->world.pos.z);
    // world.rot, not shape.rot: the flight direction. Both projectiles zero their
    // shape.rot.y on their first update (TrueCoop EnemySync.cpp:1167-1174).
    w.U16(static_cast<uint16_t>(actor->world.rot.x));
    w.U16(static_cast<uint16_t>(actor->world.rot.y));
    w.U16(static_cast<uint16_t>(actor->world.rot.z));
    w.U8(0);
    CoopGame::SendEvent(Coop::EventKind::ActorSpawn, w.Take());
}

// Host, once per frame while a client watches: announce new dynamic enemies,
// re-announce the living ones every 60 frames so a client that walked in late
// gets them (TrueCoop EnemySync.cpp:1146-1177), and announce new projectiles.
void AnnounceSpawns() {
    if (!sHostStreaming) {
        sPendingProjectiles.clear(); // nobody to tell, and a late rock would come out of nowhere
        return;
    }
    bool rebroadcast = (sTick % kSpawnRebroadcastFrames) == 0;
    for (auto& [id, d] : sDyn) {
        Actor* a = d.actor;
        if (a->update == nullptr || !(d.needsAnnounce || rebroadcast) || a->colChkInfo.health == 0) {
            continue;
        }
        auto info = sEnemies.find(a);
        if (info != sEnemies.end() && (info->second.deathCooldown > 0 || info->second.killSent)) {
            continue; // dying: don't bring it back on the clients
        }
        d.needsAnnounce = false;
        SendSpawnEvent(a, d, id);
    }
    for (Actor* a : sPendingProjectiles) {
        if (a->update != nullptr) {
            SendProjectileEvent(a);
        }
    }
    sPendingProjectiles.clear();
}

bool IsSpawnableId(int16_t actorId, bool projectile) {
    if (projectile) {
        return actorId == ACTOR_EN_NUTSBALL || actorId == ACTOR_EN_OKUTA;
    }
    // never spawn whatever a packet says: only real enemies / bosses
    const ActorDBEntry& entry = ActorDB::Instance->RetrieveEntry(actorId).entry;
    return entry.valid && (entry.category == ACTORCAT_ENEMY || entry.category == ACTORCAT_BOSS) &&
           actorId != ACTOR_EN_FLOORMAS && actorId != ACTOR_EN_ELF;
}

// Client: the host spawned something. Copied from TrueCoop HandleRemoteSpawn.
void OnSpawnEvent(const Coop::Event& e) {
    if (!sActive || !sMirrorOn || !sClient || e.origin != Coop::kHostPlayerId || gPlayState == nullptr) {
        return;
    }
    Coop::ByteReader r(e.data);
    uint8_t op = 0, hasParent = 0, parentRoom = 0;
    uint16_t rx, ry, rz, parentIndex = 0, parentId = 0;
    Coop::EnemyKill key;
    Vec3f pos;
    if (!r.U8(op) || op > kSpawnProjectile || !ReadKey(r, key) || !ReadF32(r, pos.x) || !ReadF32(r, pos.y) ||
        !ReadF32(r, pos.z) || !r.U16(rx) || !r.U16(ry) || !r.U16(rz) || !r.U8(hasParent)) {
        return;
    }
    if (hasParent != 0 && (!r.U8(parentRoom) || !r.U16(parentIndex) || !r.U16(parentId))) {
        return;
    }
    if (r.Remaining() != 0 || !IsHereKey(key) || !IsSpawnableId(key.actorId, op == kSpawnProjectile)) {
        return;
    }
    if (key.room != gPlayState->roomCtx.curRoom.num && key.room >= 0) {
        ESYNC_LOG("[Coop] Spawn of {:#x} dropped, room {} not loaded here", key.actorId, key.room);
        return;
    }
    if (op == kSpawnProjectile) {
        // only when we're copying the host: else our own shooters fire their own
        if (!sClientCopying) {
            return;
        }
        sSpawningRemote = true;
        Actor_Spawn(&gPlayState->actorCtx, gPlayState, key.actorId, pos.x, pos.y, pos.z, (s16)rx, (s16)ry, (s16)rz,
                    key.params);
        sSpawningRemote = false;
        return;
    }
    if (key.listIndex > -2) {
        return;
    }
    int id = DynIdFromIndex(key.listIndex);
    if (sDyn.contains(id) || sRecentlyDead.contains(id)) {
        return; // already have it (re-announce), or it just died here
    }
    Actor* parent = nullptr;
    if (hasParent != 0) {
        parent = FindSynced(static_cast<int8_t>(parentRoom), static_cast<int16_t>(parentIndex),
                            static_cast<int16_t>(parentId));
        if (parent == nullptr || parent->update == nullptr) {
            // Gohma larvas write into their mother without checking: no parent, no
            // spawn. The host re-announces every 60 frames, we'll try again then.
            ESYNC_LOG("[Coop] Spawn of {:#x} waits for its parent", key.actorId);
            return;
        }
    }
    sSpawningRemote = true;
    Actor* actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, key.actorId, pos.x, pos.y, pos.z, (s16)rx, (s16)ry,
                               (s16)rz, key.params);
    sSpawningRemote = false;
    if (actor == nullptr) {
        SPDLOG_WARN("[Coop] Couldn't spawn the host's enemy {:#x} (id {})", key.actorId, id);
        return;
    }
    actor->parent = parent;
    sDyn[id] = DynEnemy{ actor, key.actorId, key.params, false };
    sDynIdOf[actor] = id;
    ESYNC_LOG("[Coop] Spawned the host's enemy {:#x} id {} parent {}", key.actorId, id, parent != nullptr);
}

// ActorDespawn: a dynamic enemy removed on the host (op 0), or a "may regrow"
// death (op 1, from anyone).
void OnDespawnEvent(const Coop::Event& e) {
    if (!sActive || gPlayState == nullptr) {
        return;
    }
    Coop::ByteReader r(e.data);
    uint8_t op = 0;
    Coop::EnemyKill key;
    if (!r.U8(op) || op > kDefeat || !ReadKey(r, key) || r.Remaining() != 0) {
        return;
    }
    if (op == kDefeat) {
        ApplyDefeat(key);
        return;
    }
    if (!sClient || e.origin != Coop::kHostPlayerId || !IsHereKey(key) || key.listIndex > -2) {
        return;
    }
    Actor* actor = FindSynced(key.room, key.listIndex, key.actorId);
    if (actor == nullptr || actor->update == nullptr || CoopGame::IsPuppetFairy(actor)) {
        return;
    }
    ESYNC_LOG("[Coop] Host removed dynamic enemy {}", DynIdFromIndex(key.listIndex));
    QuietKill(actor); // a removal, not a death: no loot, nothing sent back
}

// --- roster repair (TrueCoop EnemySync.cpp:1217-1225, 1409-1453, Packets/EnemyRoster.cpp) ---

bool InRoster(Actor* a, int8_t room) {
    return a->room == room && IsSyncedCategory(a) && !IsMirrorBlocked(a) && !IsTrackingExcluded(a) &&
           !CoopGame::IsPuppetFairy(a) && KeyIndex(a) != -1;
}

// Client, on room entry: ask the host what's still alive in here.
void SendRosterRequest(int8_t room) {
    Coop::ByteWriter w;
    w.U8(kRosterRequest);
    w.U16(static_cast<uint16_t>(gPlayState->sceneNum));
    w.U8(CurrentLayer());
    w.U8(static_cast<uint8_t>(room));
    CoopGame::SendEvent(Coop::EventKind::EnemyRoster, w.Take());
    ESYNC_LOG("[Coop] Asked the host for room {}'s enemies", room);
}

// Host: answer for a room we have loaded.
void SendRosterReply(uint32_t target, int8_t room) {
    struct Entry {
        int16_t index, actorId;
        uint8_t alive, hp;
    };
    std::vector<Entry> entries;
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr && entries.size() < 255; a = a->next) {
            if (!InRoster(a, room)) {
                continue;
            }
            bool alive = a->update != nullptr;
            if (auto it = sEnemies.find(a); it != sEnemies.end()) {
                alive = alive && it->second.deathCooldown == 0 && !it->second.killSent && !it->second.released;
            }
            entries.push_back({ KeyIndex(a), a->id, (uint8_t)(alive ? 1 : 0), a->colChkInfo.health });
        }
    }
    Coop::ByteWriter w;
    w.U8(kRosterReply);
    w.U32(target);
    w.U16(static_cast<uint16_t>(gPlayState->sceneNum));
    w.U8(CurrentLayer());
    w.U8(static_cast<uint8_t>(room));
    w.U8(static_cast<uint8_t>(entries.size()));
    for (const Entry& e : entries) {
        w.U16(static_cast<uint16_t>(e.index));
        w.U16(static_cast<uint16_t>(e.actorId));
        w.U8(e.alive);
        w.U8(e.hp);
    }
    CoopGame::SendEvent(Coop::EventKind::EnemyRoster, w.Take());
    ESYNC_LOG("[Coop] Sent room {}'s enemies ({}) to player {}", room, entries.size(), target);
}

// Client: kill our ghosts (enemies the host doesn't have anymore: they died
// before we were listening) quietly, and fix the health of the others.
void ReconcileRoster(int8_t room, const std::map<std::pair<int16_t, int16_t>, std::pair<bool, uint8_t>>& host) {
    std::vector<Actor*> ghosts;
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->update == nullptr || !InRoster(a, room)) {
                continue;
            }
            EnemyInfo& info = sEnemies[a];
            if (info.released || info.killSent) {
                continue;
            }
            auto it = host.find({ KeyIndex(a), a->id });
            if (it == host.end() || !it->second.first) {
                ghosts.push_back(a);
            } else if (it->second.second != a->colChkInfo.health) {
                a->colChkInfo.health = it->second.second;
                info.prevHealth = it->second.second;
            }
        }
    }
    for (Actor* a : ghosts) {
        ESYNC_LOG("[Coop] Ghost enemy {:#x} index {} removed (dead on the host)", a->id, KeyIndex(a));
        QuietKill(a);
    }
}

// Shared health: someone hit it and had `post` left. Ours can't have more than
// post + damage (TrueCoop ApplyRemoteHit drift clamp, EnemySync.cpp:500-505).
void SendHealthCap(Actor* actor, uint8_t damage, uint8_t post) {
    Coop::EnemyKill key;
    if (!MakeKey(actor, key)) {
        return;
    }
    Coop::ByteWriter w;
    w.U8(kHealthCap);
    WriteKey(w, key);
    w.U8(damage);
    w.U8(post);
    CoopGame::SendEvent(Coop::EventKind::EnemyRoster, w.Take());
}

void OnRosterEvent(const Coop::Event& e) {
    if (!sActive || gPlayState == nullptr) {
        return;
    }
    Coop::ByteReader r(e.data);
    uint8_t op = 0;
    if (!r.U8(op)) {
        return;
    }
    if (op == kHealthCap) {
        Coop::EnemyKill key;
        uint8_t damage, post;
        if (!ReadKey(r, key) || !r.U8(damage) || !r.U8(post) || r.Remaining() != 0 || !IsHereKey(key)) {
            return;
        }
        if (Actor* a = FindSynced(key.room, key.listIndex, key.actorId)) {
            EnemyInfo& info = sEnemies[a];
            info.healthCap = (uint8_t)std::min<int>({ (int)info.healthCap, post + damage, 255 });
        }
        return;
    }
    uint32_t target = 0;
    if (op == kRosterReply && !r.U32(target)) {
        return;
    }
    uint16_t scene;
    uint8_t layer, room;
    if (op > kRosterReply || !r.U16(scene) || !r.U8(layer) || !r.U8(room)) {
        return;
    }
    if (static_cast<int16_t>(scene) != gPlayState->sceneNum || layer != CurrentLayer()) {
        return;
    }
    if (op == kRosterRequest) {
        // only the one running the enemies answers, and only for a room it has loaded
        if (r.Remaining() == 0 && sHosting && sMirrorOn && gPlayState->roomCtx.curRoom.num == (int8_t)room) {
            SendRosterReply(e.origin, static_cast<int8_t>(room));
        }
        return;
    }
    uint8_t count = 0;
    if (target != CoopGame::SelfId() || e.origin != Coop::kHostPlayerId || !sClient || !sMirrorOn || !r.U8(count) ||
        r.Remaining() != static_cast<size_t>(count) * 6) {
        return;
    }
    std::map<std::pair<int16_t, int16_t>, std::pair<bool, uint8_t>> host;
    for (uint8_t i = 0; i < count; i++) {
        uint16_t index = 0, id = 0;
        uint8_t alive = 0, hp = 0;
        r.U16(index);
        r.U16(id);
        r.U8(alive);
        r.U8(hp);
        host[{ static_cast<int16_t>(index), static_cast<int16_t>(id) }] = { alive != 0, hp };
    }
    ReconcileRoster(static_cast<int8_t>(room), host);
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
    for (int cat : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            int16_t index = KeyIndex(a);
            if (index == -1 || a->update == nullptr || out.enemies.size() >= Coop::kMaxEnemiesPerPacket ||
                !IsMirrorable(a)) {
                continue; // Floormasters and bosses without an adapter: everyone runs their own
            }
            EnemyInfo& info = sEnemies[a];
            ForceUncull(a, info);
            Coop::EnemyState e;
            e.room = index <= -2 ? 0 : a->room;
            e.listIndex = index;
            e.actorId = a->id;
            e.pos = ToNet(a->world.pos);
            e.focus = ToNet(a->focus.pos);
            e.scale = ToNet(a->scale);
            e.shapeRot = ToNet(a->shape.rot);
            e.worldRot = ToNet(a->world.rot);
            e.health = a->colChkInfo.health;
            // low 3 bits: colliders used. High 5 bits: boss adapter phase (TrueCoop "phase")
            e.submitted = info.submitted & 7;
            if (CoopBosses::HasAdapter(a->id)) {
                info.phase = CoopBosses::GetPhase(a) & 0x1F;
                e.submitted |= static_cast<uint8_t>(info.phase << 3);
            }
            info.submitted = 0;
            for (Collider* col : info.colliders) {
                if (col->actor == a && col->shape == COLSHAPE_QUAD &&
                    e.quadCorners.size() / 4 < Coop::kMaxEnemyQuads) {
                    for (const Vec3f& corner : ((ColliderQuad*)col)->dim.quad) {
                        e.quadCorners.push_back(ToNet(corner));
                    }
                }
            }
            if (info.skel != nullptr && info.skel->jointTable != nullptr &&
                info.skel->limbCount <= Coop::kMaxEnemyJoints) {
                e.joints.resize(info.skel->limbCount);
                for (size_t i = 0; i < e.joints.size(); i++) {
                    e.joints[i] = ToNet(info.skel->jointTable[i]);
                }
            }
            ReadBody(a, info, e);
            out.enemies.push_back(std::move(e));
        }
    }
    CoopGame::Session().SendEnemyStates(out);
}

// --- client: copy the host's enemies ---

// Position, health and skeleton only (also used alone on a boss phase change).
void ApplyPose(Actor* actor, EnemyInfo& info, const Coop::EnemyState& s) {
    actor->world.pos = { s.pos.x, s.pos.y, s.pos.z };
    actor->focus.pos = { s.focus.x, s.focus.y, s.focus.z };
    actor->scale = { s.scale.x, s.scale.y, s.scale.z };
    actor->shape.rot = { s.shapeRot.x, s.shapeRot.y, s.shapeRot.z };
    actor->world.rot = { s.worldRot.x, s.worldRot.y, s.worldRot.z };
    actor->colChkInfo.health = s.health;
    info.prevHealth = s.health;
    if (info.skel != nullptr && info.skel->jointTable != nullptr && s.joints.size() == info.skel->limbCount) {
        for (size_t i = 0; i < s.joints.size(); i++) {
            info.skel->jointTable[i] = { s.joints[i].x, s.joints[i].y, s.joints[i].z };
        }
    }
}

// Freeze our copy and make it look like the host's.
void ApplyHostState(Actor* actor, EnemyInfo& info, const Coop::EnemyState& s) {
    WriteBody(actor, info, s);
    if (CoopBosses::HasAdapter(actor->id)) {
        CoopBosses::ApplyExtras(actor, s.body);
    }
    ApplyPose(actor, info, s);

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
    int16_t index = KeyIndex(actor);
    Coop::EnemyHit h;
    h.sceneNum = gPlayState->sceneNum;
    h.room = index <= -2 ? 0 : actor->room;
    h.listIndex = index;
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

// Enemies only know about OUR Link: they read its position directly (GET_PLAYER)
// to chase, aim, dive... So right before an enemy's update, if a teammate is
// closer to it, we move our Link to the teammate's spot, and put it back right
// after the update. Nothing is drawn in between, our Link doesn't really move.
struct PlayerSwap {
    bool active = false;
    Player* player = nullptr;
    PosRot world;
    Vec3f focus;
    Vec3s shapeRot;
};
PlayerSwap sSwap;

void EndAggroSwap() {
    if (!sSwap.active) {
        return;
    }
    sSwap.player->actor.world = sSwap.world;
    sSwap.player->actor.focus.pos = sSwap.focus;
    sSwap.player->actor.shape.rot = sSwap.shapeRot;
    sSwap.active = false;
}

void BeginAggroSwap(Actor* enemy) {
    std::vector<Actor*> puppets = CoopGame::VisiblePuppets();
    Player* player = GET_PLAYER(gPlayState);
    if (puppets.empty() || player == nullptr) {
        return;
    }
    Actor* best = nullptr;
    float bestSq = enemy->xyzDistToPlayerSq; // distance to our Link, computed by the game
    for (Actor* p : puppets) {
        // dead or downed teammates aren't targets (TrueCoop OverridePlayerPerception:779-781)
        if ((((Player*)p)->stateFlags1 & PLAYER_STATE1_DEAD) || sAggroSkip.contains(p)) {
            continue;
        }
        float xz = Actor_WorldDistXZToActor(enemy, p);
        float y = Actor_HeightDiff(enemy, p);
        float sq = SQ(xz) + SQ(y);
        if (sq < bestSq) {
            best = p;
            bestSq = sq;
        }
    }
    if (best == nullptr) {
        return; // we're the closest, nothing to do
    }
    sSwap.active = true;
    sSwap.player = player;
    sSwap.world = player->actor.world;
    sSwap.focus = player->actor.focus.pos;
    sSwap.shapeRot = player->actor.shape.rot;

    player->actor.world = best->world;
    player->actor.shape.rot = best->shape.rot;
    player->actor.focus.pos = best->world.pos;
    player->actor.focus.pos.y +=
        (best->focus.pos.y > best->world.pos.y) ? best->focus.pos.y - best->world.pos.y : 50.0f; // about head height
    // the numbers the game measured before calling us, redone against the teammate
    enemy->xzDistToPlayer = Actor_WorldDistXZToActor(enemy, best);
    enemy->yDistToPlayer = Actor_HeightDiff(enemy, best);
    enemy->xyzDistToPlayerSq = SQ(enemy->xzDistToPlayer) + SQ(enemy->yDistToPlayer);
    enemy->yawTowardsPlayer = Actor_WorldYawTowardActor(enemy, best);
}

// --- copying the rest of an enemy's memory ---
//
// Every enemy type has its own fields after the common Actor part (timers,
// "on fire", current action...). Writing code for each of the ~100 enemies
// would take forever, so we copy that memory as is: both games are the same
// build, the layout is the same. What we must NOT copy as is:
// - pointers to other things in memory (other actors, buffers): addresses are
//   different in each game, so those slots are skipped
// - pointers into the exe (current action function, static tables): sent as an
//   offset from the start of the exe and rebuilt on the other side
// - colliders and the skeleton struct: handled on their own above
// - fields a boss adapter says to leave alone (CoopBosses.cpp)

enum SlotKind : uint8_t { kSlotRaw = 0, kSlotSkip = 1, kSlotExe = 2 };

// Upper 32 bits of heap addresses we've seen this frame. Heap pointers in an
// enemy point near other actors, so a value with the same upper bits is
// treated as a pointer. Ints and floats almost never look like that.
std::vector<uint32_t> sHeapHighs;

void RefreshHeapHighs() {
    sHeapHighs.clear();
    auto add = [](uintptr_t addr) {
        uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(addr) >> 32);
        if (std::find(sHeapHighs.begin(), sHeapHighs.end(), hi) == sHeapHighs.end()) {
            sHeapHighs.push_back(hi);
        }
    };
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            add(reinterpret_cast<uintptr_t>(a));
        }
    }
    static std::vector<uint8_t> probe(64); // a fresh heap block, for the CRT heap
    add(reinterpret_cast<uintptr_t>(probe.data()));
    add(reinterpret_cast<uintptr_t>(gPlayState));
}

SlotKind Classify(uint64_t v) {
    if (v == 0) {
        return kSlotRaw;
    }
    uintptr_t exeStart = 0, exeEnd = 0;
    if (CoopGame::ExeRange(exeStart, exeEnd) && v >= exeStart && v < exeEnd) {
        return kSlotExe;
    }
    uint32_t hi = static_cast<uint32_t>(v >> 32);
    if (hi == 0) {
        return kSlotRaw;
    }
    for (uint32_t h : sHeapHighs) {
        if (hi + 1 >= h && hi <= h + 1) {
            return kSlotSkip;
        }
    }
    return kSlotRaw;
}

// Byte ranges inside the actor we never copy raw (relative to the actor start).
std::vector<std::pair<size_t, size_t>> OwnRanges(Actor* actor, const EnemyInfo& info, size_t size) {
    std::vector<std::pair<size_t, size_t>> out;
    uintptr_t base = reinterpret_cast<uintptr_t>(actor);
    auto add = [&](const void* p, size_t len) {
        uintptr_t a = reinterpret_cast<uintptr_t>(p);
        if (a >= base && a < base + size) {
            out.emplace_back(a - base, std::min(a - base + len, size));
        }
    };
    if (info.skel != nullptr) {
        add(info.skel, sizeof(SkelAnime));
    }
    for (Collider* col : info.colliders) {
        switch (col->shape) {
            case COLSHAPE_CYLINDER:
                add(col, sizeof(ColliderCylinder));
                break;
            case COLSHAPE_QUAD:
                add(col, sizeof(ColliderQuad));
                break;
            case COLSHAPE_JNTSPH: {
                ColliderJntSph* c = (ColliderJntSph*)col;
                add(col, sizeof(ColliderJntSph));
                add(c->elements, sizeof(ColliderJntSphElement) * std::max(c->count, 0));
                break;
            }
            case COLSHAPE_TRIS: {
                ColliderTris* c = (ColliderTris*)col;
                add(col, sizeof(ColliderTris));
                add(c->elements, sizeof(ColliderTrisElement) * std::max(c->count, 0));
                break;
            }
            default:
                break;
        }
    }
    if (CoopBosses::HasAdapter(actor->id)) {
        CoopBosses::AddNoCopyRanges(actor, out);
    }
    return out;
}

bool InRanges(const std::vector<std::pair<size_t, size_t>>& ranges, size_t from, size_t to) {
    for (const auto& r : ranges) {
        if (from < r.second && to > r.first) {
            return true;
        }
    }
    return false;
}

size_t InstanceSize(Actor* actor) {
    return ActorDB::Instance->RetrieveEntry(actor->id).entry.instanceSize;
}

// host side: the enemy's own memory, ready to send
void ReadBody(Actor* actor, const EnemyInfo& info, Coop::EnemyState& e) {
    size_t size = InstanceSize(actor);
    if (sizeof(Actor) % 8 != 0 || size <= sizeof(Actor) || size - sizeof(Actor) > Coop::kMaxEnemyBody) {
        return;
    }
    auto ranges = OwnRanges(actor, info, size);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(actor);
    uintptr_t exeStart = 0, exeEnd = 0;
    CoopGame::ExeRange(exeStart, exeEnd);
    e.body.assign(bytes + sizeof(Actor), bytes + size);
    e.slotKinds.assign((e.body.size() + 7) / 8, kSlotRaw);
    for (size_t slot = 0; slot < e.slotKinds.size(); slot++) {
        size_t off = sizeof(Actor) + slot * 8;
        if (InRanges(ranges, off, std::min(off + 8, size))) {
            e.slotKinds[slot] = kSlotSkip;
            continue;
        }
        if (off + 8 > size) {
            continue; // last few bytes, too short to be a pointer
        }
        uint64_t v;
        memcpy(&v, bytes + off, 8);
        SlotKind kind = Classify(v);
        e.slotKinds[slot] = kind;
        if (kind == kSlotExe) {
            uint64_t offset = v - exeStart;
            memcpy(&e.body[slot * 8], &offset, 8);
        }
    }
}

// client side: write the host's values into our copy
void WriteBody(Actor* actor, const EnemyInfo& info, const Coop::EnemyState& e) {
    size_t size = InstanceSize(actor);
    if (e.body.empty() || sizeof(Actor) + e.body.size() != size || e.slotKinds.size() != (e.body.size() + 7) / 8) {
        return; // not the same actor layout: leave it alone
    }
    auto ranges = OwnRanges(actor, info, size);
    uint8_t* bytes = reinterpret_cast<uint8_t*>(actor);
    uintptr_t exeStart = 0, exeEnd = 0;
    bool haveExe = CoopGame::ExeRange(exeStart, exeEnd);
    for (size_t slot = 0; slot < e.slotKinds.size(); slot++) {
        size_t off = sizeof(Actor) + slot * 8;
        size_t len = std::min<size_t>(8, size - off);
        if (e.slotKinds[slot] == kSlotSkip || InRanges(ranges, off, off + len)) {
            continue;
        }
        if (len == 8) {
            uint64_t mine;
            memcpy(&mine, bytes + off, 8);
            if (Classify(mine) == kSlotSkip) {
                continue; // we hold a pointer here: keep ours
            }
            if (e.slotKinds[slot] == kSlotExe) {
                uint64_t offset;
                memcpy(&offset, &e.body[slot * 8], 8);
                if (!haveExe || offset >= exeEnd - exeStart) {
                    continue;
                }
                uint64_t v = exeStart + offset;
                memcpy(bytes + off, &v, 8);
                continue;
            }
            if (Classify(mine) == kSlotExe) {
                uint64_t theirs;
                memcpy(&theirs, &e.body[slot * 8], 8);
                if (theirs != 0) {
                    continue; // our function pointer vs their plain number: don't break ours
                }
            }
        }
        memcpy(bytes + off, &e.body[slot * 8], len);
    }
}

// --- shared health (bosses without adapter, Floormasters, or enemies nobody is copying) ---

bool SharesHealth(Actor* actor, const EnemyInfo& info) {
    if (actor->category == ACTORCAT_BOSS && !CoopBosses::HasAdapter(actor->id)) {
        return true;
    }
    if (IsMirrorBlocked(actor)) {
        return true; // local on every game (TrueCoop: "M1 shared-HP still applies")
    }
    return !sHostStreaming && !info.mirrored;
}

void SendHealthDrop(Actor* actor, uint8_t damage) {
    int16_t index = KeyIndex(actor);
    Coop::EnemyHit h;
    h.sceneNum = gPlayState->sceneNum;
    h.room = index <= -2 ? 0 : actor->room;
    h.listIndex = index;
    h.actorId = actor->id;
    h.damage = damage;
    h.effect = actor->colChkInfo.damageEffect;
    h.hitPos = ToNet(Vec3s{ (s16)actor->world.pos.x, (s16)actor->world.pos.y, (s16)actor->world.pos.z });
    std::lock_guard lock(CoopGame::Mutex());
    CoopGame::Session().SendEnemyHit(h);
    SendHealthCap(actor, damage, actor->colChkInfo.health);
}

// After the enemy's update: did it lose health because of us? Tell the others.
void OnActorUpdate(void* ptr) {
    EndAggroSwap();
    Actor* actor = static_cast<Actor*>(ptr);
    if (!sActive || gPlayState == nullptr) {
        return;
    }
    if (IsRegrowingBush(actor->id, actor->params) && GetActorListIndex(actor) >= 0) {
        TrackBush(actor);
        return;
    }
    auto it = sEnemies.find(actor);
    if (it == sEnemies.end() || !IsSyncedCategory(actor) || KeyIndex(actor) == -1) {
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

// Hits teammates sent us, replayed right before the enemy looks for them.
void ReplayPendingHits(Actor* actor, EnemyInfo& info) {
    for (const Coop::EnemyHit& h : info.pendingHits) {
        Vec3s pos = { h.hitPos.x, h.hitPos.y, h.hitPos.z };
        ReplayHit(actor, info, h.damage, h.effect, h.dmgFlags, pos);
        if (SharesHealth(actor, info)) {
            info.expectedRemote = (uint8_t)std::min<int>(info.expectedRemote + h.damage, 255);
            info.expectedTimer = kExpectedDamageFrames;
        }
    }
    info.pendingHits.clear();
}

// --- the per-actor hook: runs right before each actor's update ---

void OnShouldActorUpdate(void* ptr, bool* should) {
    EndAggroSwap(); // in case the previous enemy's update got skipped by someone else
    Actor* actor = static_cast<Actor*>(ptr);
    if (!*should || gPlayState == nullptr || !sActive) {
        return;
    }

    // client: an enemy our own game spawned while the host is sending us its own
    if (!sLocalSpawns.empty()) {
        if (auto ls = sLocalSpawns.find(actor); ls != sLocalSpawns.end()) {
            sLocalSpawns.erase(ls);
            if (ShouldDropLocalSpawn(actor)) {
                ESYNC_LOG("[Coop] Dropped our own spawn of {:#x}, the host sends its own", actor->id);
                QuietKill(actor);
                *should = false;
                return;
            }
        }
    }

    if (!IsSyncedCategory(actor)) {
        return;
    }
    auto it = sEnemies.find(actor);
    if (it == sEnemies.end()) {
        return;
    }
    EnemyInfo& info = it->second;

    // shared health: never more than what the last hitter had before their hit
    if (info.healthCap != 255) {
        if (actor->colChkInfo.health > info.healthCap) {
            actor->colChkInfo.health = info.healthCap;
            info.prevHealth = info.healthCap;
        }
        info.healthCap = 255;
    }

    // not copying anything: run it ourselves, after the teammates' hits. The host
    // also makes it chase the nearest player.
    if (!sClientCopying) {
        StopMirroring(actor, info);
        ReplayPendingHits(actor, info);
        bool fighting =
            !CoopBosses::HasAdapter(actor->id) || CoopBosses::ShouldMirror(actor, CoopBosses::GetPhase(actor));
        if (sHostStreaming && IsMirrorable(actor) && !info.released && fighting) {
            BeginAggroSwap(actor); // undone in OnActorUpdate, right after its update
        }
        return;
    }

    // client copying the host
    int16_t index = KeyIndex(actor);
    auto st = sHostStates.end();
    if (!info.released && IsMirrorable(actor) && index != -1) {
        st = sHostStates.find(StateKey(actor->room, index));
    }
    if (st == sHostStates.end() || st->second.actorId != actor->id) {
        // the host doesn't have this one loaded (other room), or it's dying /
        // local only: run it ourselves, health shared
        StopMirroring(actor, info);
        ReplayPendingHits(actor, info);
        return;
    }
    const Coop::EnemyState& s = st->second;

    // Boss adapters (CoopBosses.cpp, TrueCoop EnemySync.cpp:860-886). Phase edges
    // first: the defeat edge comes with a phase we wouldn't copy, the handoff must still run.
    if (CoopBosses::HasAdapter(actor->id)) {
        uint8_t phase = s.submitted >> 3;
        if (phase != info.phase) {
            uint8_t from = info.phase;
            info.phase = phase;
            ApplyPose(actor, info, s); // hand off from the last pose the host had
            if (CoopBosses::OnPhaseChange(actor, from, phase)) {
                StopMirroring(actor, info);
                info.released = true;
                ForceUncull(actor, info); // the local sequence must play even far away
                info.pendingHits.clear();
                return; // our own game runs it from now on
            }
        }
        if (!CoopBosses::ShouldMirror(actor, phase)) {
            StopMirroring(actor, info); // e.g. intro cutscene: runs locally on every game
            info.pendingHits.clear();
            return;
        }
    }

    // other teammates' hits: the host already has them, the health comes in the
    // stream. Replaying them here would also send them again (TrueCoop EnemyHit.cpp:67-73).
    info.pendingHits.clear();
    if (!info.mirrored) {
        SPDLOG_INFO("[Coop] Copying the host's enemy {:#x} room {} index {} (host at {:.0f} {:.0f} {:.0f}, ours at "
                    "{:.0f} {:.0f} {:.0f})",
                    actor->id, actor->room, index, s.pos.x, s.pos.y, s.pos.z, actor->world.pos.x, actor->world.pos.y,
                    actor->world.pos.z);
        StartMirroring(actor, info);
    }
    ForceUncull(actor, info);
    ForwardLocalHits(actor, info);
    ApplyHostState(actor, info, s);
    *should = false; // the host runs it, not us
}

// --- capture colliders and skeletons as enemies get set up ---

void OnColliderSetup(void* actorPtr, void* collider) {
    Actor* actor = static_cast<Actor*>(actorPtr);
    if (actor != nullptr && IsKillSynced(actor)) {
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
    Actor* actor = static_cast<Actor*>(actorPtr);
    sEnemies.erase(actor);
    if (IsKillSynced(actor)) {
        // params as spawned: some enemies reuse params as a state later (TrueCoop spawnParams)
        EnemyInfo& info = sEnemies[actor];
        info.spawnParams = actor->params;
        info.spawnParamsKnown = true;
    }
}

// init is done: the skeleton it set up lives inside its own memory
void OnActorInit(void* actorPtr) {
    Actor* actor = static_cast<Actor*>(actorPtr);
    if (GetActorListIndex(actor) >= 0) {
        ObjectExtension::GetInstance().Set<EnemySpawnRoom>(actor, EnemySpawnRoom{ actor->room });
    }
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
    Actor* actor = static_cast<Actor*>(actorPtr);
    sEnemies.erase(actor);
    sBushDown.erase(actor);
    sBushCutByTeammate.erase(actor);
    sLocalSpawns.erase(actor);
    std::erase(sPendingProjectiles, actor);
    if (auto it = sDynIdOf.find(actor); it != sDynIdOf.end()) {
        sRecentlyDead[it->second] = kRecentlyDeadFrames; // a late re-announce must not bring it back
        auto d = sDyn.find(it->second);
        if (d != sDyn.end() && d->second.actor == actor) {
            sDyn.erase(d);
        }
        sDynIdOf.erase(it);
    }
}

void AgeRecentlyDead() {
    for (auto it = sRecentlyDead.begin(); it != sRecentlyDead.end();) {
        if (--it->second <= 0) {
            it = sRecentlyDead.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

namespace CoopGame {

void UpdateEnemies() {
    EndAggroSwap();
    if (gPlayState != nullptr) {
        RefreshHeapHighs();
    }
    sTick++;
    Coop::Session& session = Session();
    std::vector<Coop::EnemyKill> kills = session.TakeEnemyKills();
    std::vector<Coop::EnemyHit> hits = session.TakeEnemyHits();
    std::optional<Coop::EnemyStates> states = session.TakeEnemyStates();
    bool hosting = session.Role() == Coop::SessionRole::Hosting;
    bool client = session.Role() == Coop::SessionRole::Connected;

    bool syncOn = CVarGetInteger(CVAR_COOP("EnemySync"), 1) != 0;
    sMirrorOn = CVarGetInteger(CVAR_COOP("EnemyMirroring"), 1) != 0;
    sVerbose = CVarGetInteger(CVAR_COOP("EnemySyncVerbose"), 0) != 0;

    if (!SessionActive() || !LocalState().inGame || gPlayState == nullptr || !syncOn) {
        if (sActive && gPlayState != nullptr) {
            StopAllMirroring(); // switched off mid-scene: don't leave enemies frozen
        }
        sActive = false;
        sDead.clear();
        sHostStates.clear();
        sHostStreaming = false;
        sClientCopying = false;
        sHosting = sClient = sHostHere = false;
        sDyn.clear();
        sDynIdOf.clear();
        sRecentlyDead.clear();
        sLocalSpawns.clear();
        sPendingProjectiles.clear();
        sLastRosterRoom = -1;
        return;
    }
    sActive = true;
    sHosting = hosting;
    sClient = client;

    // Coop.EnemyMirroring switched off mid-scene: one cleanup pass (TrueCoop:1067-1080)
    if (!sMirrorOn && sMirrorWasOn) {
        SPDLOG_INFO("[Coop] Enemy copying switched off, everyone runs their own enemies now");
        StopAllMirroring();
    }
    sMirrorWasOn = sMirrorOn;

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
    sHostHere = hostHere;

    // downed teammates: enemies leave them alone
    sAggroSkip.clear();
    for (Actor* p : VisiblePuppets()) {
        if (const Coop::RemotePlayer* owner = PuppetOwner(p)) {
            LifeState life = PlayerLifeState(owner->id);
            if (life == LifeState::Downed || life == LifeState::GameOver) {
                sAggroSkip.insert(p);
            }
        }
    }

    // host: stream our enemies while a client can see them
    bool wasStreaming = sHostStreaming;
    sHostStreaming = hosting && clientHere && sMirrorOn;
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
    AnnounceSpawns();

    // client: keep the latest picture of the host's enemies
    sStatesAge++;
    if (!sMirrorOn) {
        states.reset(); // copying switched off here: ignore the host's stream
    }
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
            sHostStates[StateKey(e.room, e.listIndex)] = e;
        }
        if (sStatesAge >= kStaleFrames || states->enemies.size() != sLastStateCount) {
            SPDLOG_INFO("[Coop] Host's enemies: {} in scene {:#x} layer {}", states->enemies.size(), states->sceneNum,
                        states->layer);
            sLastStateCount = states->enemies.size();
        }
        sStatesAge = 0;
    }
    sClientCopying = client && hostHere && sMirrorOn && sStatesAge < kStaleFrames;
    if (!sClientCopying) {
        sHostStates.clear();
    }

    // client: ghost repair on room entry (TrueCoop EnemySync.cpp:1217-1225)
    if (client && hostHere && sMirrorOn) {
        int8_t room = gPlayState->roomCtx.curRoom.num;
        if (room != sLastRosterRoom) {
            sLastRosterRoom = room;
            if (room >= 0) {
                SendRosterRequest(room);
            }
        }
    } else {
        sLastRosterRoom = -1; // ask again once the host is with us
    }

    // hits: queued on the enemy, replayed right before its next update
    for (const Coop::EnemyHit& h : hits) {
        if (h.sceneNum != gPlayState->sceneNum) {
            continue;
        }
        if (Actor* actor = FindSynced(h.room, h.listIndex, h.actorId)) {
            sEnemies[actor].pendingHits.push_back(h);
        }
    }

    for (const Coop::EnemyKill& k : kills) {
        ApplyKill(k);
    }
    TickDeaths();
    AgeRecentlyDead();
    ForgetEmptyRooms();
}

void ReadActorBody(Actor* actor, std::vector<uint8_t>& body, std::vector<uint8_t>& kinds) {
    Coop::EnemyState e;
    auto it = sEnemies.find(actor);
    ReadBody(actor, it != sEnemies.end() ? it->second : EnemyInfo{}, e);
    body = std::move(e.body);
    kinds = std::move(e.slotKinds);
}

void WriteActorBody(Actor* actor, const std::vector<uint8_t>& body, const std::vector<uint8_t>& kinds) {
    Coop::EnemyState e;
    e.body = body;
    e.slotKinds = kinds;
    auto it = sEnemies.find(actor);
    WriteBody(actor, it != sEnemies.end() ? it->second : EnemyInfo{}, e);
}

bool IsBreakable(int16_t actorId) {
    return IsBreakableId(actorId);
}

void RegisterEnemyHooks() {
    COND_HOOK(OnActorKill, true, OnActorKill);
    COND_HOOK(OnEnemyDefeat, true, OnEnemyDefeat);
    COND_HOOK(OnActorSpawn, true, OnActorSpawn);
    COND_HOOK(ShouldActorUpdate, true, OnShouldActorUpdate);
    COND_HOOK(OnActorUpdate, true, OnActorUpdate);
    COND_HOOK(ShouldActorInit, true, OnShouldActorInit);
    COND_HOOK(OnActorInit, true, OnActorInit);
    COND_HOOK(OnActorDestroy, true, OnActorDestroy);
    COND_HOOK(OnColliderSetup, true, OnColliderSetup);
    COND_HOOK(OnColliderSubmit, true, OnColliderSubmit);
    COND_HOOK(OnSkelAnimeInit, true, OnSkelAnimeInit);
    // En_Encount1 (Stalchild / Leever / Wolfos spawner) is a PROP, nobody copies
    // it: on a client it would spawn its own children on top of the host's.
    // Frozen while the host is in our scene (TrueCoop EnemySync.cpp:1658-1668).
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_ENCOUNT1, true, [](void*, bool* should) {
        if (sActive && sMirrorOn && sClient && sHostHere) {
            *should = false;
        }
    });
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
    OnEvent(Coop::EventKind::ActorSpawn, "enemies", OnSpawnEvent);
    OnEvent(Coop::EventKind::ActorDespawn, "enemies", OnDespawnEvent);
    OnEvent(Coop::EventKind::EnemyRoster, "enemies", OnRosterEvent);
}

} // namespace CoopGame
