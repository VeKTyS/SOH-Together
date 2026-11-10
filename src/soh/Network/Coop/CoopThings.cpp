// Things players carry, throw, and their bombs / arrows / boomerang.
//
// - a pot / rock / bush / crate / cucco someone picks up: everyone else's copy
//   of it follows it (it's from the room's actor list, so everyone has one).
//   While it's in their hands it sits in the hands of their puppet, the same
//   way the game puts a held pot in Link's hands (Player_PostLimbDrawGameplay
//   writes heldActor's position from the hand matrix, we just give the puppet
//   a heldActor). When it breaks, the kill sync in CoopEnemies.cpp breaks it
//   everywhere.
// - bombs, bombchus, bomb flower bombs, arrows, slingshot seeds, deku nuts and
//   the boomerang only exist in the game of whoever made them. Others get a
//   look-alike ("ghost") that follows the real one. Its own code never runs (it
//   would explode / fly / hurt on its own), we copy the few values its draw
//   needs (size, fuse timer, flashing...). A bomb that explodes switches to
//   its "explosion" params: that's when we go boom.
//   The boom is just the visual + sound: the real bomb already did the damage
//   (walls go through flags, enemies through the host). Otherwise everything
//   would get hit twice.
//
// Each game sends the list of its things every frame (snapshot channel), the
// host passes it on to the others. No Anchor / TrueCoop code for this: neither
// syncs carried things, it's our own, built like the enemy sync.
//
// Why it didn't work before (found while reading the game code):
// - a prop picked up sets its own actor.room to -1 (ObjTsubo_SetupLiftedUp,
//   EnIshi_SetupLiftedUp, EnKusa_SetupLiftedUp, ObjKibako...), and to the
//   thrower's current room once thrown. We sent that room and the others
//   looked for "room -1, index N": never found, so nothing moved. Now we send
//   the room it spawned in (remembered at init).
// - a bomb starts with scale 0 (EnBom's init chain) and only gets its size in
//   EnBom_Update when the fuse reaches 67. The ghost's update never runs and
//   scale lives in the common Actor part, which the old memory copy skipped:
//   ghost bombs were there but invisible. Now we send the scale.
// - the old raw memory copy also copied effect slot numbers (bombchu trails):
//   when the ghost died it deleted OUR effects with their numbers. We now send
//   a few chosen values instead.

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <spdlog/spdlog.h>

#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_En_Arrow/z_en_arrow.h"
#include "src/overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "src/overlays/actors/ovl_En_Bom_Chu/z_en_bom_chu.h"
#include "src/overlays/actors/ovl_En_Bombf/z_en_bombf.h"
#include "src/overlays/actors/ovl_En_Boom/z_en_boom.h"
extern PlayState* gPlayState;
// non-static in z_en_arrow.c: a ghost arrow "flying" draws its trail
void EnArrow_Fly(EnArrow* enArrow, PlayState* play);
}

// libultra's r4300.h has a U32(x) macro (pulled in by the headers above), it breaks ByteWriter::U32
#undef U32

namespace {

// The room an actor spawned in. Props change their actor.room while carried
// (-1) and when thrown (thrower's room), the spawn room never changes.
struct ThingSpawnRoom {
    int8_t room = 0;
};
ObjectExtension::Register<ThingSpawnRoom> sSpawnRoomRegister;

int8_t SpawnRoom(Actor* a) {
    ThingSpawnRoom* r = ObjectExtension::GetInstance().Get<ThingSpawnRoom>(a);
    return r != nullptr ? r->room : a->room;
}

// --- the few values a ghost needs, our own little format in ThingState::body ---

constexpr uint8_t kExtraVersion = 1; // first byte: a garbage body is easy to spot
constexpr uint8_t kExtraHeld = 1;    // in its owner's hands

struct Extra {
    uint8_t flags = 0;
    float scale = 0.0f;
    float speedXZ = 0.0f;
    Vec3s worldRot = { 0, 0, 0 };
    int16_t timer = 0;  // fuse / blink / spin timer
    int16_t timer2 = 0; // bomb flash speed
    float value = 0.0f; // bomb flash intensity, bombchu jitter
};

void WriteF(Coop::ByteWriter& w, float v) {
    w.U32(std::bit_cast<uint32_t>(v));
}

bool ReadF(Coop::ByteReader& r, float& v, float limit) {
    uint32_t bits = 0;
    if (!r.U32(bits)) {
        return false;
    }
    v = std::bit_cast<float>(bits);
    return std::isfinite(v) && std::fabs(v) <= limit; // never feed the game a crazy number
}

void PackExtra(const Extra& e, Coop::ThingState& t) {
    Coop::ByteWriter w;
    w.U8(kExtraVersion);
    w.U8(e.flags);
    WriteF(w, e.scale);
    WriteF(w, e.speedXZ);
    w.U16(static_cast<uint16_t>(e.worldRot.x));
    w.U16(static_cast<uint16_t>(e.worldRot.y));
    w.U16(static_cast<uint16_t>(e.worldRot.z));
    w.U16(static_cast<uint16_t>(e.timer));
    w.U16(static_cast<uint16_t>(e.timer2));
    WriteF(w, e.value);
    t.body = w.Take();
    t.slotKinds.assign((t.body.size() + 7) / 8, 0); // the message wants one byte per 8, unused here
}

bool UnpackExtra(const Coop::ThingState& t, Extra& e) {
    Coop::ByteReader r(t.body);
    uint8_t version = 0;
    uint16_t rx = 0, ry = 0, rz = 0, timer = 0, timer2 = 0;
    if (!r.U8(version) || version != kExtraVersion || !r.U8(e.flags) || !ReadF(r, e.scale, 10.0f) ||
        !ReadF(r, e.speedXZ, 1000.0f) || !r.U16(rx) || !r.U16(ry) || !r.U16(rz) || !r.U16(timer) ||
        !r.U16(timer2) || !ReadF(r, e.value, 100000.0f) || r.Remaining() != 0) {
        return false;
    }
    e.worldRot = { static_cast<s16>(rx), static_cast<s16>(ry), static_cast<s16>(rz) };
    e.timer = static_cast<int16_t>(timer);
    e.timer2 = static_cast<int16_t>(timer2);
    return true;
}

// --- our things ---

std::unordered_map<Actor*, uint16_t> sOurIds; // our dynamic things (bombs, arrows...) -> their number
uint16_t sNextId = 1;
std::unordered_map<Actor*, int> sFollowed; // props we picked up -> frames they've been still

// --- their things ---

struct OwnerThings {
    std::map<uint16_t, Actor*> ghosts; // their localId -> our look-alike
    std::set<uint16_t> boomed;         // their bombs we already blew up
    int quietFrames = 0;               // frames since their last list
};
std::map<uint32_t, OwnerThings> sOwners;          // by player id
std::set<Actor*> sGhosts;                         // every look-alike, any owner
std::unordered_map<Actor*, int> sMovedByTeammate; // room props a teammate holds -> frames since last move
// puppets we gave something to hold -> the thing (cleared before it can dangle)
std::unordered_map<Actor*, Actor*> sPuppetHolds;

// a room prop a teammate moved stays frozen this long after their last update
constexpr int kTeammateHoldFrames = 5;
// a thrown prop stops being followed once it's been still this long
constexpr int kStillFrames = 20;
// an owner that went quiet this long left the scene (or the game): drop their ghosts
constexpr int kOwnerGoneFrames = 30;

bool SessionActive() {
    Coop::SessionRole role = CoopGame::Session().Role();
    return role == Coop::SessionRole::Hosting || role == Coop::SessionRole::Connected;
}

// Anything from the room's actor list a player can hold: pots, rocks, bushes,
// crates, cuccos, the big golden-gauntlets block... not a puppet, not Link.
bool IsCarryable(Actor* a) {
    return a != nullptr && a->id != ACTOR_PLAYER && GetActorListIndex(a) >= 0 && CoopGame::PuppetOwner(a) == nullptr;
}

// Things that only live in their owner's game and get a ghost on the others.
bool IsGhostable(int16_t actorId, int16_t params) {
    switch (actorId) {
        case ACTOR_EN_BOM:
        case ACTOR_EN_BOM_CHU:
        case ACTOR_EN_BOOM:
            return true;
        case ACTOR_EN_BOMBF:
            return params == BOMBFLOWER_BODY || params == BOMBFLOWER_EXPLOSION; // not the planted flower
        case ACTOR_EN_ARROW:
            return params >= ARROW_NORMAL_LIT && params <= ARROW_NUT; // arrows, seeds, nuts
        default:
            return false; // hookshot: its chain is drawn from OUR Link's hand (ArmsHook_Draw), can't ghost it
    }
}

bool IsExplosion(int16_t actorId, int16_t params) {
    return (actorId == ACTOR_EN_BOM && params == BOMB_EXPLOSION) ||
           (actorId == ACTOR_EN_BOMBF && params == BOMBFLOWER_EXPLOSION);
}

// Room props: matched by the room they spawned in, not actor.room (see the top)
Actor* FindListActor(int8_t room, int16_t index, int16_t actorId) {
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->id == actorId && GetActorListIndex(a) == index && SpawnRoom(a) == room) {
                return a;
            }
        }
    }
    return nullptr;
}

Coop::ThingState Describe(Actor* a, uint16_t localId, bool held) {
    Coop::ThingState t;
    t.localId = localId;
    t.room = SpawnRoom(a);
    t.listIndex = GetActorListIndex(a);
    t.actorId = a->id;
    t.params = a->params;
    t.pos = { a->world.pos.x, a->world.pos.y, a->world.pos.z };
    t.rot = { a->shape.rot.x, a->shape.rot.y, a->shape.rot.z };

    Extra e;
    e.flags = held ? kExtraHeld : 0;
    e.scale = a->scale.x;
    e.speedXZ = a->speedXZ;
    e.worldRot = a->world.rot;
    switch (a->id) {
        case ACTOR_EN_BOM: {
            EnBom* b = reinterpret_cast<EnBom*>(a);
            e.timer = b->timer;
            e.timer2 = b->flashSpeedScale;
            e.value = b->flashIntensity;
            break;
        }
        case ACTOR_EN_BOMBF: {
            EnBombf* b = reinterpret_cast<EnBombf*>(a);
            e.timer = b->timer;
            e.timer2 = b->flashSpeedScale;
            e.value = b->flashIntensity;
            break;
        }
        case ACTOR_EN_BOM_CHU: {
            EnBomChu* c = reinterpret_cast<EnBomChu*>(a);
            e.timer = c->timer;
            e.value = c->visualJitter;
            break;
        }
        case ACTOR_EN_ARROW:
            e.timer = reinterpret_cast<EnArrow*>(a)->timer;
            break;
        case ACTOR_EN_BOOM:
            e.timer = reinterpret_cast<EnBoom*>(a)->activeTimer;
            break;
        default:
            break;
    }
    PackExtra(e, t);
    return t;
}

// Our dynamic things in one category list (ours only, not the ghosts of other players' things)
void AddDynamic(int category, Player* player, Coop::ThingStates& out) {
    for (Actor* a = gPlayState->actorCtx.actorLists[category].head; a != nullptr; a = a->next) {
        if (sGhosts.contains(a) || a->update == nullptr || !IsGhostable(a->id, a->params) ||
            GetActorListIndex(a) >= 0 || out.things.size() >= Coop::kMaxThings) {
            continue;
        }
        auto [it, isNew] = sOurIds.try_emplace(a, sNextId);
        if (isNew) {
            sNextId = sNextId == 0xFFFF ? 1 : sNextId + 1;
        }
        out.things.push_back(Describe(a, it->second, player->heldActor == a));
    }
}

void SendOurThings() {
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr || CoopGame::SelfId() == 0) {
        return; // no player id yet (just connected): others couldn't tell whose things these are
    }
    if (IsCarryable(player->heldActor)) {
        sFollowed[player->heldActor] = 0;
    }

    Coop::ThingStates out;
    out.owner = CoopGame::SelfId(); // so the others know whose puppet holds what
    out.sceneNum = gPlayState->sceneNum;
    out.layer = static_cast<uint8_t>(gSaveContext.sceneLayer);

    // props we hold or threw: follow them until they settle down
    for (auto it = sFollowed.begin(); it != sFollowed.end();) {
        Actor* a = it->first;
        bool held = player->heldActor == a;
        bool still = a->speedXZ == 0.0f && a->velocity.y == 0.0f;
        it->second = (held || !still) ? 0 : it->second + 1;
        if (a->update == nullptr || it->second > kStillFrames) {
            it = sFollowed.erase(it);
            continue;
        }
        if (out.things.size() < Coop::kMaxThings) {
            out.things.push_back(Describe(a, 0, held));
        }
        ++it;
    }

    AddDynamic(ACTORCAT_EXPLOSIVE, player, out);  // bombs, bombchus, bomb flower bombs
    AddDynamic(ACTORCAT_ITEMACTION, player, out); // arrows, seeds, deku nuts
    AddDynamic(ACTORCAT_MISC, player, out);       // boomerang
    CoopGame::Session().SendThingStates(out);
}

// --- applying the others' things ---

// Same effects as EnBom_Explode, minus the damage.
void Boom(Vec3f pos) {
    Vec3f zero = { 0.0f, 0.0f, 0.0f };
    Vec3f accel = { 0.0f, 0.6f, 0.0f };
    Vec3f shockAccel = { 0.0f, 0.0f, 0.0f };
    EffectSsBomb2_SpawnLayered(gPlayState, &pos, &zero, &accel, 100, 19);
    SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 50, NA_SE_IT_BOMB_EXPLOSION);
    Camera_AddQuake(&gPlayState->mainCamera, 2, 0xB, 8);
    Vec3f floor = pos;
    floor.y -= 10.0f;
    EffectSsBlast_SpawnWhiteShockwave(gPlayState, &floor, &zero, &shockAccel);
}

void ReleaseHold(Actor* puppet) {
    auto it = sPuppetHolds.find(puppet);
    if (it != sPuppetHolds.end()) {
        reinterpret_cast<Player*>(puppet)->heldActor = nullptr;
        sPuppetHolds.erase(it);
    }
}

// Forget every puppet hold pointing at this thing (it's going away)
void ReleaseHoldsOf(Actor* thing) {
    for (auto it = sPuppetHolds.begin(); it != sPuppetHolds.end();) {
        if (it->second == thing) {
            reinterpret_cast<Player*>(it->first)->heldActor = nullptr;
            it = sPuppetHolds.erase(it);
        } else {
            ++it;
        }
    }
}

void RemoveGhost(Actor* ghost) {
    ReleaseHoldsOf(ghost);
    sGhosts.erase(ghost);
    Actor_Kill(ghost);
}

void DropOwner(OwnerThings& owner) {
    for (auto& [id, ghost] : owner.ghosts) {
        RemoveGhost(ghost);
    }
    owner.ghosts.clear();
    owner.boomed.clear();
}

Actor* PuppetOf(uint32_t playerId) {
    for (Actor* puppet : CoopGame::VisiblePuppets()) {
        const Coop::RemotePlayer* p = CoopGame::PuppetOwner(puppet);
        if (p != nullptr && p->id == playerId) {
            return puppet;
        }
    }
    return nullptr;
}

// The thing is in the owner's hands: their puppet holds it and Link's draw code
// (run for the puppet) puts it in the hands. True if it was already attached
// last frame: then the draw already placed it and we leave its position alone.
bool AttachToPuppet(Actor* puppet, Actor* thing, const Coop::ThingState& t) {
    if (puppet == nullptr) {
        return false;
    }
    Player* p = reinterpret_cast<Player*>(puppet);
    bool wasHeld = p->heldActor == thing;
    if (!wasHeld) {
        ReleaseHold(puppet);
        p->heldActor = thing;
        sPuppetHolds[puppet] = thing;
    }
    // the game turns a carried pot with Link plus this offset (unk_3BC.y, set when lifting)
    p->unk_3BC.x = 0;
    p->unk_3BC.y = static_cast<s16>(t.rot.y - puppet->shape.rot.y);
    p->unk_3BC.z = 0;
    return wasHeld;
}

void ApplyGhostLook(Actor* ghost, const Extra& e) {
    Actor_SetScale(ghost, e.scale);
    ghost->speedXZ = e.speedXZ;
    ghost->world.rot = e.worldRot;
    switch (ghost->id) {
        case ACTOR_EN_BOM: {
            EnBom* b = reinterpret_cast<EnBom*>(ghost);
            b->timer = e.timer;
            b->flashSpeedScale = e.timer2;
            b->flashIntensity = e.value;
            break;
        }
        case ACTOR_EN_BOMBF: {
            EnBombf* b = reinterpret_cast<EnBombf*>(ghost);
            b->timer = e.timer;
            b->flashSpeedScale = e.timer2;
            b->flashIntensity = e.value;
            break;
        }
        case ACTOR_EN_BOM_CHU: {
            EnBomChu* c = reinterpret_cast<EnBomChu*>(ghost);
            c->timer = e.timer;
            c->visualJitter = e.value;
            break;
        }
        case ACTOR_EN_ARROW: {
            EnArrow* a = reinterpret_cast<EnArrow*>(ghost);
            a->timer = static_cast<u8>(e.timer);
            if (ghost->speedXZ != 0.0f && (e.flags & kExtraHeld) == 0) {
                a->actionFunc = EnArrow_Fly; // only read by its draw (trail), its update never runs
            }
            break;
        }
        case ACTOR_EN_BOOM:
            reinterpret_cast<EnBoom*>(ghost)->activeTimer = static_cast<u8>(e.timer);
            break;
        default:
            break;
    }
}

void ApplyTheirThings(uint32_t ownerId, const Coop::ThingStates& list) {
    OwnerThings& owner = sOwners[ownerId];
    owner.quietFrames = 0;
    Actor* puppet = PuppetOf(ownerId);
    if (list.sceneNum != gPlayState->sceneNum || list.layer != static_cast<uint8_t>(gSaveContext.sceneLayer)) {
        DropOwner(owner); // they're somewhere else
        if (puppet != nullptr) {
            ReleaseHold(puppet);
        }
        return;
    }
    Player* me = GET_PLAYER(gPlayState);
    Actor* heldNow = nullptr; // what their puppet holds this frame
    std::set<uint16_t> seen;
    for (const Coop::ThingState& t : list.things) {
        Extra e;
        if (!UnpackExtra(t, e)) {
            continue; // broken (or another build's format): skip it
        }
        bool held = (e.flags & kExtraHeld) != 0;
        if (t.listIndex >= 0) {
            // a pot / rock from the room they're holding or threw: move ours there
            Actor* a = FindListActor(t.room, t.listIndex, t.actorId);
            if (a == nullptr || a->update == nullptr || (me != nullptr && me->heldActor == a)) {
                continue; // not loaded here, or WE hold it too: ours wins on our screen
            }
            bool attached = held && AttachToPuppet(puppet, a, t);
            if (held && puppet != nullptr) {
                heldNow = a;
            }
            if (!attached) {
                a->world.pos = { t.pos.x, t.pos.y, t.pos.z };
                a->shape.rot = { t.rot.x, t.rot.y, t.rot.z };
                a->world.rot.y = t.rot.y;
            }
            a->speedXZ = 0.0f;
            a->velocity = { 0.0f, 0.0f, 0.0f };
            sMovedByTeammate[a] = 0;
            continue;
        }
        if (!IsGhostable(t.actorId, t.params)) {
            continue; // we only ever spawn these few actors from the network
        }
        seen.insert(t.localId);
        if (IsExplosion(t.actorId, t.params)) {
            // it just went off: boom once, the ghost isn't needed anymore
            if (owner.boomed.insert(t.localId).second) {
                Boom({ t.pos.x, t.pos.y, t.pos.z });
            }
            if (auto g = owner.ghosts.find(t.localId); g != owner.ghosts.end()) {
                RemoveGhost(g->second);
                owner.ghosts.erase(g);
            }
            continue;
        }
        Actor*& ghost = owner.ghosts[t.localId];
        if (ghost == nullptr) {
            ghost = Actor_Spawn(&gPlayState->actorCtx, gPlayState, t.actorId, t.pos.x, t.pos.y, t.pos.z, t.rot.x,
                                t.rot.y, t.rot.z, t.params);
            if (ghost == nullptr) {
                owner.ghosts.erase(t.localId);
                continue;
            }
            ghost->room = -1; // don't vanish when we change rooms, it follows its owner's thing
            sGhosts.insert(ghost);
        }
        if (ghost->id != t.actorId) {
            continue; // their number got reused for another kind of thing: wait for the list to settle
        }
        bool attached = held && AttachToPuppet(puppet, ghost, t);
        if (held && puppet != nullptr) {
            heldNow = ghost;
        }
        if (!attached) {
            ghost->world.pos = { t.pos.x, t.pos.y, t.pos.z };
            ghost->shape.rot = { t.rot.x, t.rot.y, t.rot.z };
        }
        ApplyGhostLook(ghost, e);
    }
    // they let go of what they held (threw it, put it down, it blew up)
    if (puppet != nullptr && heldNow == nullptr) {
        ReleaseHold(puppet);
    }
    // gone from their list: already blew up, sank in water, arrow vanished...
    for (auto it = owner.ghosts.begin(); it != owner.ghosts.end();) {
        if (!seen.contains(it->first)) {
            RemoveGhost(it->second);
            it = owner.ghosts.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(owner.boomed, [&](uint16_t id) { return !seen.contains(id); });
}

void OnShouldActorUpdate(void* ptr, bool* should) {
    Actor* a = static_cast<Actor*>(ptr);
    if (sGhosts.contains(a)) {
        *should = false; // a ghost's own code never runs: it would explode / fly / hurt on its own
        return;
    }
    auto it = sMovedByTeammate.find(a);
    if (it != sMovedByTeammate.end() && it->second < kTeammateHoldFrames) {
        *should = false; // in a teammate's hands: don't let it fall / break on its own
    }
}

// Remember where room props spawned (index and room are set before init runs)
void OnActorInit(void* ptr) {
    Actor* a = static_cast<Actor*>(ptr);
    if (GetActorListIndex(a) >= 0) {
        ObjectExtension::GetInstance().Set<ThingSpawnRoom>(a, ThingSpawnRoom{ a->room });
    }
}

void OnActorDestroy(void* ptr) {
    Actor* a = static_cast<Actor*>(ptr);
    sOurIds.erase(a);
    sFollowed.erase(a);
    sMovedByTeammate.erase(a);
    ReleaseHoldsOf(a);     // a puppet must never draw a freed actor
    sPuppetHolds.erase(a); // or it's the puppet itself going away
    if (sGhosts.erase(a) > 0) {
        for (auto& [id, owner] : sOwners) {
            std::erase_if(owner.ghosts, [&](const auto& g) { return g.second == a; });
        }
    }
}

void ResetAll() {
    // puppets still alive (session ended mid-scene): let go. After a scene unload
    // they're already gone, and OnActorDestroy took them out of this list
    for (auto& [puppet, thing] : sPuppetHolds) {
        reinterpret_cast<Player*>(puppet)->heldActor = nullptr;
    }
    sPuppetHolds.clear();
    for (auto& [id, owner] : sOwners) {
        for (auto& [localId, ghost] : owner.ghosts) {
            sGhosts.erase(ghost); // the scene is gone anyway, actors are freed with it
        }
    }
    sOwners.clear();
    sFollowed.clear();
    sMovedByTeammate.clear();
}

} // namespace

namespace CoopGame {

void UpdateThings() {
    std::vector<Coop::ThingStates> lists = Session().TakeThingStates();
    if (!SessionActive() || !LocalState().inGame || gPlayState == nullptr) {
        ResetAll();
        return;
    }
    SendOurThings();

    for (const Coop::ThingStates& list : lists) {
        // owner = the sender's player id (0 = an old build that sent a random number)
        if (list.owner != 0 && list.owner != SelfId()) {
            ApplyTheirThings(list.owner, list);
        }
    }
    for (auto it = sOwners.begin(); it != sOwners.end();) {
        if (++it->second.quietFrames > kOwnerGoneFrames) {
            DropOwner(it->second);
            if (Actor* puppet = PuppetOf(it->first)) {
                ReleaseHold(puppet);
            }
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
    COND_HOOK(OnActorInit, true, OnActorInit);
    COND_HOOK(OnActorDestroy, true, OnActorDestroy);
}

} // namespace CoopGame
