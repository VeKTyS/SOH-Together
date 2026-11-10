// Moving things in the world: platforms, push blocks, windmill, elevators...
// and doors opening.
//
// Neither Anchor nor TrueCoop syncs these, so this is our own, built like the
// enemy sync (CoopEnemies.cpp):
//
// 1. Platforms & co. The host runs them. Every frame it sends the ones that
//    moved lately (position, rotation, size, and every few frames the rest of
//    their memory, same copy as enemies), plus once in a while the ones that
//    moved since the room loaded (a block pushed earlier). A client freezes its
//    copy (no update) and copies the host's while the host talks about it.
//    The ones the host never moved run on the client as usual.
//    Collision follows by itself: the game rebuilds a platform's collision
//    from its position/rotation/size every frame (DynaPoly_Setup and
//    DynaPoly_UpdateBgActorTransforms read actor->world / shape.rot / scale),
//    whether its update ran or not.
//    A client standing on a platform or pushing a block: the game writes that
//    into the platform (interactFlags "player on top", unk_150 push force).
//    The client sends it to the host, which writes it into its own copy right
//    before its update: the platform falls / sinks / moves on the host, and
//    the host's stream brings it back to everyone. Same trick as replaying
//    enemy hits.
//    Left out: anything that can hurt (has an attack collider), carried things
//    (CoopThings.cpp), switches, doors, and what flags already handle.
// 2. Doors. When our Link opens a door we tell everyone, and their copy of the
//    door plays its opening. Locked ones are unlocked by the flags (CoopWorld).

#include "soh/Network/Coop/CoopGame.h"
#include "soh/Network/Coop/Wire.h"

#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ActorListIndex.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

extern "C" {
#include "z64.h"
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "src/overlays/actors/ovl_Door_Gerudo/z_door_gerudo.h"
#include "src/overlays/actors/ovl_Door_Shutter/z_door_shutter.h"
#include "src/overlays/actors/ovl_En_Door/z_en_door.h"
extern PlayState* gPlayState;
// non-static in their actor files, we compare against them / switch to them
void EnDoor_Idle(EnDoor* enDoor, PlayState* play);
void DoorShutter_Idle(DoorShutter* doorShutter, PlayState* play);
void DoorShutter_UnbarredCheckSwitchFlag(DoorShutter* doorShutter, PlayState* play);
void func_8099485C(DoorGerudo* doorGerudo, PlayState* play); // gerudo cell door: closed, waiting for a key
void func_8099496C(DoorGerudo* doorGerudo, PlayState* play); // gerudo cell door: starts going up
}

// libultra's r4300.h has a U32(x) macro (pulled in by the headers above), it breaks ByteWriter::U32
#undef U32

namespace {

// --- tuning ---

constexpr int kDirtyFrames = 20;   // host: keep sending something every frame this long after it last moved
constexpr int kKeepAliveEvery = 15; // host: something moved since load but still now is resent this often
constexpr int kBodyEvery = 10;      // host: its memory goes along this often (the position every frame)
constexpr int kStaleFrames = 30;    // client: stop copying what the host hasn't talked about for this long
constexpr int kInteractFrames = 3;  // host: "a client stands on it" lasts this long after the last message
constexpr size_t kMaxInteracts = 16;
constexpr int kShutterOpenFrames = 25; // a sliding door a teammate went through stays up this long

// event sub-types inside EventKind::ObjectState
constexpr uint8_t kEventInteract = 1; // client -> host: I stand on / push these
constexpr uint8_t kEventDoorOpen = 2; // anyone -> everyone: I opened this door

// --- small helpers ---

bool Active() {
    Coop::SessionRole role = CoopGame::Session().Role();
    return gPlayState != nullptr && CoopGame::LocalState().inGame &&
           (role == Coop::SessionRole::Hosting || role == Coop::SessionRole::Connected);
}

uint8_t Layer() {
    return static_cast<uint8_t>(gSaveContext.sceneLayer);
}

// Where it spawned. A few platforms set actor.room to -1 to follow you between
// rooms, the spawn room never changes, so both games agree on it.
struct ObjSpawnRoom {
    int8_t room = 0;
};
ObjectExtension::Register<ObjSpawnRoom> sSpawnRoomRegister;

int8_t SpawnRoom(Actor* a) {
    ObjSpawnRoom* r = ObjectExtension::GetInstance().Get<ObjSpawnRoom>(a);
    return r != nullptr ? r->room : a->room;
}

struct Key {
    int8_t room;
    int16_t index;
    int16_t actorId;
    bool operator<(const Key& o) const {
        return std::tie(room, index, actorId) < std::tie(o.room, o.index, o.actorId);
    }
};

Key KeyOf(Actor* a) {
    return { SpawnRoom(a), GetActorListIndex(a), a->id };
}

// Stays local: doors (below), carried / breakable things (CoopThings, kills),
// what CoopWorld pokes on flags (bomb walls, webs, ice...), the water level
// (driven by flags, moves water boxes not itself), Song of Time blocks.
bool IsExcluded(int16_t id) {
    switch (id) {
        case ACTOR_EN_DOOR:
        case ACTOR_DOOR_SHUTTER:
        case ACTOR_DOOR_GERUDO:
        case ACTOR_DOOR_KILLER:
        case ACTOR_DOOR_ANA:
        case ACTOR_DOOR_TOKI:
        case ACTOR_DOOR_WARP1:
        case ACTOR_EN_HOLL:
        case ACTOR_BG_HEAVY_BLOCK:
        case ACTOR_BG_MIZU_WATER:
        case ACTOR_BG_BOMBWALL:
        case ACTOR_BG_BREAKWALL:
        case ACTOR_BG_HAKA_ZOU:
        case ACTOR_BG_HIDAN_HAMSTEP:
        case ACTOR_BG_HIDAN_HROCK:
        case ACTOR_BG_ICE_SHELTER:
        case ACTOR_BG_JYA_BOMBCHUIWA:
        case ACTOR_BG_JYA_BOMBIWA:
        case ACTOR_BG_MIZU_BWALL:
        case ACTOR_BG_SPOT08_BAKUDANKABE:
        case ACTOR_BG_SPOT11_BAKUDANKABE:
        case ACTOR_BG_SPOT17_BAKUDANKABE:
        case ACTOR_BG_YDAN_MARUTA:
        case ACTOR_BG_YDAN_SP:
        case ACTOR_BG_HIDAN_DALM:
        case ACTOR_BG_HIDAN_KOWARERUKABE:
        case ACTOR_OBJ_BOMBIWA:
        case ACTOR_OBJ_HAMISHI:
        case ACTOR_OBJ_TIMEBLOCK:
        case ACTOR_OBJ_WARP2BLOCK:
        case ACTOR_BG_TOKI_SWD:
            return true;
        default:
            return CoopGame::IsBreakable(id);
    }
}

// --- colliders, recorded as things get set up (our OnColliderSetup hook) ---

std::unordered_map<Actor*, std::vector<Collider*>> sColliders;
std::set<Actor*> sHurts; // has a collider that attacks: spikes, blades, fire... stays local

bool IsCandidateCategory(Actor* a) {
    return a->category == ACTORCAT_BG || a->category == ACTORCAT_PROP;
}

void OnColliderSetup(void* actorPtr, void* colliderPtr) {
    Actor* a = static_cast<Actor*>(actorPtr);
    Collider* c = static_cast<Collider*>(colliderPtr);
    if (a == nullptr || c == nullptr || !IsCandidateCategory(a)) {
        return;
    }
    sColliders[a].push_back(c);
    if (c->atFlags & AT_ON) {
        sHurts.insert(a);
    }
}

bool Hurts(Actor* a) {
    if (sHurts.contains(a)) {
        return true;
    }
    auto it = sColliders.find(a);
    if (it != sColliders.end()) {
        for (Collider* c : it->second) {
            if (c->atFlags & AT_ON) {
                sHurts.insert(a);
                return true;
            }
        }
    }
    return false;
}

// --- which platforms we sync ---

// Actors with collision right now (the game's own list of moving collision).
std::set<Actor*> DynaActors() {
    std::set<Actor*> out;
    DynaCollisionContext& dyna = gPlayState->colCtx.dyna;
    for (s32 i = 0; i < BG_ACTOR_MAX; i++) {
        if ((dyna.bgActorFlags[i] & 1) && dyna.bgActors[i].actor != nullptr) {
            out.insert(dyna.bgActors[i].actor);
        }
    }
    return out;
}

bool Eligible(Actor* a) {
    return a->update != nullptr && IsCandidateCategory(a) && GetActorListIndex(a) >= 0 && !IsExcluded(a->id) &&
           a->parent == nullptr && !Hurts(a);
}

size_t InstanceSize(Actor* a) {
    return ActorDB::Instance->RetrieveEntry(a->id).entry.instanceSize;
}

// The host's memory goes into ours, except what must stay ours: the collision
// slot (bgId, push force, "who stands on me") and the colliders (they hold
// pointers and our own hit state). Copied back after the generic copy.
void WriteBodyKeepingOurs(Actor* a, const std::vector<uint8_t>& body, const std::vector<uint8_t>& kinds) {
    size_t size = InstanceSize(a);
    if (size < sizeof(DynaPolyActor)) {
        return; // not a DynaPolyActor: never happens for things in the collision list
    }
    uint8_t* base = reinterpret_cast<uint8_t*>(a);
    std::vector<std::pair<size_t, size_t>> keep; // [from, to) inside the actor
    keep.emplace_back(offsetof(DynaPolyActor, bgId), sizeof(DynaPolyActor));
    auto add = [&](const void* p, size_t len) {
        uintptr_t at = reinterpret_cast<uintptr_t>(p);
        uintptr_t start = reinterpret_cast<uintptr_t>(base);
        if (at >= start && at < start + size) {
            keep.emplace_back(at - start, std::min<size_t>(at - start + len, size));
        }
    };
    if (auto it = sColliders.find(a); it != sColliders.end()) {
        for (Collider* c : it->second) {
            switch (c->shape) {
                case COLSHAPE_CYLINDER:
                    add(c, sizeof(ColliderCylinder));
                    break;
                case COLSHAPE_QUAD:
                    add(c, sizeof(ColliderQuad));
                    break;
                case COLSHAPE_JNTSPH: {
                    ColliderJntSph* j = reinterpret_cast<ColliderJntSph*>(c);
                    add(c, sizeof(ColliderJntSph));
                    add(j->elements, sizeof(ColliderJntSphElement) * std::max(j->count, 0));
                    break;
                }
                case COLSHAPE_TRIS: {
                    ColliderTris* t = reinterpret_cast<ColliderTris*>(c);
                    add(c, sizeof(ColliderTris));
                    add(t->elements, sizeof(ColliderTrisElement) * std::max(t->count, 0));
                    break;
                }
                default:
                    break;
            }
        }
    }
    std::vector<std::vector<uint8_t>> saved;
    for (const auto& [from, to] : keep) {
        saved.emplace_back(base + from, base + to);
    }
    CoopGame::WriteActorBody(a, body, kinds);
    for (size_t i = 0; i < keep.size(); i++) {
        memcpy(base + keep[i].first, saved[i].data(), saved[i].size());
    }
}

// ============================================================================
// host side
// ============================================================================

struct HostObj {
    Vec3f lastPos{};
    Vec3s lastRot{};
    Vec3f lastScale{};
    Vec3s firstRot{};
    Vec3f firstScale{};
    int dirty = 0;            // frames left of "moved lately"
    int sinceSent = 1000;     // frames since we last sent it
    int sinceBody = 1000;     // frames since we last sent its memory
    bool cullSaved = false;   // we forced it to update far from us
    uint32_t savedCullFlags = 0;
};
std::unordered_map<Actor*, HostObj> sHostObjs;

// a client stands on / pushes one of ours
struct Interact {
    uint8_t flags = 0;
    float push = 0.0f;
    s16 pushRot = 0;
    bool pushFresh = false; // the push goes in once per message, the standing lasts a few frames
    int framesLeft = 0;
};
std::map<Key, Interact> sInteracts;

bool SameVec(const Vec3f& a, const Vec3f& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
bool SameRot(const Vec3s& a, const Vec3s& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Far from our own Link, things don't update (culling). A teammate may be on it.
void ForceUncull(Actor* a, HostObj& h) {
    if (!h.cullSaved) {
        h.savedCullFlags = a->flags & ACTOR_FLAG_UPDATE_CULLING_DISABLED;
        h.cullSaved = true;
    }
    a->flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
}

void RestoreCull(Actor* a, HostObj& h) {
    if (h.cullSaved) {
        a->flags &= ~ACTOR_FLAG_UPDATE_CULLING_DISABLED;
        a->flags |= h.savedCullFlags;
        h.cullSaved = false;
    }
}

void RestoreAllCulls() {
    for (auto& [a, h] : sHostObjs) {
        RestoreCull(a, h);
    }
}

bool ClientInOurScene() {
    for (const Coop::RemotePlayer& p : CoopGame::Session().Players()) {
        // different age = different layer = different objects (same rule as enemies)
        if (p.state && p.state->inGame && p.state->sceneNum == gPlayState->sceneNum &&
            p.state->linkAge == CoopGame::LocalState().linkAge) {
            return true;
        }
    }
    return false;
}

Coop::ObjectState Describe(Actor* a, bool withBody) {
    Coop::ObjectState o;
    o.room = SpawnRoom(a);
    o.listIndex = GetActorListIndex(a);
    o.actorId = a->id;
    o.pos = { a->world.pos.x, a->world.pos.y, a->world.pos.z };
    o.shapeRot = { a->shape.rot.x, a->shape.rot.y, a->shape.rot.z };
    o.worldRot = { a->world.rot.x, a->world.rot.y, a->world.rot.z };
    o.scale = { a->scale.x, a->scale.y, a->scale.z };
    if (withBody) {
        CoopGame::ReadActorBody(a, o.body, o.slotKinds);
    }
    return o;
}

void HostUpdate() {
    for (auto& [key, in] : sInteracts) {
        in.framesLeft--;
    }
    std::erase_if(sInteracts, [](const auto& kv) { return kv.second.framesLeft <= 0; });

    if (!ClientInOurScene()) {
        RestoreAllCulls();
        return;
    }
    std::vector<Coop::ObjectState> moving, keepAlive;
    for (Actor* a : DynaActors()) {
        if (!Eligible(a)) {
            continue;
        }
        auto [it, isNew] = sHostObjs.try_emplace(a);
        HostObj& h = it->second;
        if (isNew) {
            h.lastPos = a->world.pos;
            h.lastRot = a->shape.rot;
            h.lastScale = a->scale;
            h.firstRot = a->shape.rot;
            h.firstScale = a->scale;
        }
        bool moved = !SameVec(a->world.pos, h.lastPos) || !SameRot(a->shape.rot, h.lastRot) ||
                     !SameVec(a->scale, h.lastScale);
        h.lastPos = a->world.pos;
        h.lastRot = a->shape.rot;
        h.lastScale = a->scale;
        h.dirty = moved ? kDirtyFrames : std::max(h.dirty - 1, 0);
        h.sinceSent++;
        h.sinceBody++;

        // keep moving things moving (and things a client stands on) even far from us
        if (h.dirty > 0 || sInteracts.contains(KeyOf(a))) {
            ForceUncull(a, h);
        } else {
            RestoreCull(a, h);
        }

        bool displaced = Math_Vec3f_DistXYZ(&a->world.pos, &a->home.pos) > 1.0f || !SameRot(a->shape.rot, h.firstRot) ||
                         !SameVec(a->scale, h.firstScale);
        if (h.dirty > 0) {
            bool withBody = h.sinceBody >= kBodyEvery;
            moving.push_back(Describe(a, withBody));
            h.sinceSent = 0;
            if (withBody) {
                h.sinceBody = 0;
            }
        } else if (displaced && h.sinceSent >= kKeepAliveEvery) {
            keepAlive.push_back(Describe(a, true));
            h.sinceSent = 0;
            h.sinceBody = 0;
        }
    }
    if (moving.empty() && keepAlive.empty()) {
        return;
    }
    Coop::ObjectStates out;
    out.sceneNum = gPlayState->sceneNum;
    out.layer = Layer();
    for (auto* list : { &moving, &keepAlive }) { // moving ones first if there are too many
        for (Coop::ObjectState& o : *list) {
            if (out.objects.size() < Coop::kMaxObjects) {
                out.objects.push_back(std::move(o));
            }
        }
    }
    CoopGame::Session().SendObjectStates(out);
}

// Host, right before one of our platforms updates: what the clients did to it
// last frame goes in, as if our own Link did it.
void HostApplyInteract(Actor* a) {
    // only platforms we track (they're DynaPolyActors): the key comes from the
    // network, it must never make us write into some other kind of actor
    if (sInteracts.empty() || GetActorListIndex(a) < 0 || !sHostObjs.contains(a)) {
        return;
    }
    auto it = sInteracts.find(KeyOf(a));
    if (it == sInteracts.end()) {
        return;
    }
    DynaPolyActor* dyna = reinterpret_cast<DynaPolyActor*>(a);
    dyna->interactFlags |= it->second.flags;
    if (it->second.pushFresh) {
        dyna->unk_150 += it->second.push; // same as func_8002DFA4, what Link calls while pushing
        dyna->unk_158 = it->second.pushRot;
        it->second.pushFresh = false;
    }
}

void OnInteractEvent(Coop::ByteReader& r) {
    if (CoopGame::Session().Role() != Coop::SessionRole::Hosting) {
        return; // the host runs the platforms, the others just pass it on
    }
    uint16_t scene = 0;
    uint8_t layer = 0, count = 0;
    if (!r.U16(scene) || !r.U8(layer) || !r.U8(count) || count > kMaxInteracts) {
        return;
    }
    bool here = static_cast<int16_t>(scene) == gPlayState->sceneNum && layer == Layer();
    for (uint8_t i = 0; i < count; i++) {
        uint8_t room = 0, flags = 0;
        uint16_t index = 0, id = 0, rot = 0;
        uint32_t pushBits = 0;
        if (!r.U8(room) || !r.U16(index) || !r.U16(id) || !r.U8(flags) || !r.U32(pushBits) || !r.U16(rot)) {
            return;
        }
        float push = std::bit_cast<float>(pushBits);
        if (!here || !std::isfinite(push) || std::fabs(push) > 100.0f) {
            continue;
        }
        Interact& in = sInteracts[{ static_cast<int8_t>(room), static_cast<int16_t>(index), static_cast<int16_t>(id) }];
        in.flags = flags & (DYNA_INTERACT_PLAYER_ON_TOP | DYNA_INTERACT_PLAYER_ABOVE);
        if (push != 0.0f) {
            in.push = push;
            in.pushRot = static_cast<s16>(rot);
            in.pushFresh = true;
        }
        in.framesLeft = kInteractFrames;
    }
}

// ============================================================================
// client side
// ============================================================================

std::unordered_map<Actor*, int> sMirrored; // client: platform we copy -> frames since the host last mentioned it

bool HostInOurScene() {
    const auto& players = CoopGame::Session().Players();
    if (players.empty() || !players[0].state) {
        return false; // a client's first player is always the host
    }
    const Coop::PlayerState& host = *players[0].state;
    return host.inGame && host.sceneNum == gPlayState->sceneNum && host.linkAge == CoopGame::LocalState().linkAge;
}

void ApplyHostObject(Actor* a, const Coop::ObjectState& o) {
    a->world.pos = { o.pos.x, o.pos.y, o.pos.z };
    a->shape.rot = { o.shapeRot.x, o.shapeRot.y, o.shapeRot.z };
    a->world.rot = { o.worldRot.x, o.worldRot.y, o.worldRot.z };
    a->scale = { o.scale.x, o.scale.y, o.scale.z };
    if (!o.body.empty()) {
        WriteBodyKeepingOurs(a, o.body, o.slotKinds);
    }
}

// What our Link did to the platforms this frame (the game writes it into them
// while Link updates, they read it on their next update), sent to the host.
void SendInteracts(const std::set<Actor*>& dyna) {
    Coop::ByteWriter w;
    w.U8(kEventInteract);
    w.U16(static_cast<uint16_t>(gPlayState->sceneNum));
    w.U8(Layer());
    std::vector<Actor*> touched;
    for (Actor* a : dyna) {
        if (touched.size() >= kMaxInteracts || !Eligible(a)) {
            continue;
        }
        DynaPolyActor* d = reinterpret_cast<DynaPolyActor*>(a);
        uint8_t flags = d->interactFlags & (DYNA_INTERACT_PLAYER_ON_TOP | DYNA_INTERACT_PLAYER_ABOVE);
        if (flags != 0 || d->unk_150 != 0.0f) {
            touched.push_back(a);
        }
    }
    if (touched.empty()) {
        return;
    }
    w.U8(static_cast<uint8_t>(touched.size()));
    for (Actor* a : touched) {
        DynaPolyActor* d = reinterpret_cast<DynaPolyActor*>(a);
        Key k = KeyOf(a);
        w.U8(static_cast<uint8_t>(k.room));
        w.U16(static_cast<uint16_t>(k.index));
        w.U16(static_cast<uint16_t>(k.actorId));
        w.U8(d->interactFlags & (DYNA_INTERACT_PLAYER_ON_TOP | DYNA_INTERACT_PLAYER_ABOVE));
        w.U32(std::bit_cast<uint32_t>(d->unk_150));
        w.U16(static_cast<uint16_t>(d->unk_158));
        if (sMirrored.contains(a)) {
            d->unk_150 = 0.0f; // a frozen block never eats its push: the host's does
        }
    }
    CoopGame::SendEvent(Coop::EventKind::ObjectState, w.Take());
}

void ClientUpdate() {
    std::optional<Coop::ObjectStates> states = CoopGame::Session().TakeObjectStates();
    if (!HostInOurScene()) {
        sMirrored.clear(); // the host isn't here: everything runs on our side
        return;
    }
    std::set<Actor*> dyna = DynaActors();
    if (states && states->sceneNum == gPlayState->sceneNum && states->layer == Layer()) {
        std::map<Key, Actor*> ours;
        for (Actor* a : dyna) {
            if (Eligible(a)) {
                ours[KeyOf(a)] = a;
            }
        }
        Player* me = GET_PLAYER(gPlayState);
        for (const Coop::ObjectState& o : states->objects) {
            auto it = ours.find({ o.room, o.listIndex, o.actorId });
            if (it == ours.end() || (me != nullptr && me->heldActor == it->second)) {
                continue;
            }
            ApplyHostObject(it->second, o);
            sMirrored[it->second] = 0;
        }
    }
    for (auto it = sMirrored.begin(); it != sMirrored.end();) {
        if (++it->second > kStaleFrames) {
            it = sMirrored.erase(it); // the host stopped talking about it: ours runs again from there
        } else {
            ++it;
        }
    }
    SendInteracts(dyna);
}

// ============================================================================
// doors
// ============================================================================

std::set<Actor*> sReplayedDoors; // doors we're opening because of a teammate (don't echo them back)
Actor* sOpenSent = nullptr;      // the door we told everyone we opened

struct ShutterAnim {
    int phase = 0; // 0 going up, 1 staying up, 2 going down
    int timer = 0;
};
std::unordered_map<Actor*, ShutterAnim> sShutterAnims;

bool IsAlive(Actor* a, std::initializer_list<int> categories) {
    for (int cat : categories) {
        for (Actor* it = gPlayState->actorCtx.actorLists[cat].head; it != nullptr; it = it->next) {
            if (it == a) {
                return a->update != nullptr;
            }
        }
    }
    return false;
}

// Our Link is opening this door right now (Player_ActionHandler_1 sets these)
bool IsOpening(Actor* door) {
    switch (door->id) {
        case ACTOR_EN_DOOR:
            return reinterpret_cast<EnDoor*>(door)->playerIsOpening != 0;
        case ACTOR_DOOR_SHUTTER:
            return reinterpret_cast<DoorShutter*>(door)->unk_164 != 0;
        case ACTOR_DOOR_GERUDO:
            return reinterpret_cast<DoorGerudo*>(door)->unk_164 != 0;
        default:
            return false; // DoorKiller: a trap that attacks whoever opens it, stays with them
    }
}

void WatchOurDoor() {
    Player* player = GET_PLAYER(gPlayState);
    Actor* door = player != nullptr ? player->doorActor : nullptr;
    // doorActor can be left pointing at a door from a room that's gone
    if (door != nullptr && !IsAlive(door, { ACTORCAT_DOOR, ACTORCAT_ITEMACTION })) {
        door = nullptr;
    }
    bool opening = door != nullptr && IsOpening(door) && !sReplayedDoors.contains(door);
    if (!opening) {
        sOpenSent = nullptr;
        return;
    }
    if (door == sOpenSent) {
        return; // already told them
    }
    sOpenSent = door;
    Coop::ByteWriter w;
    w.U8(kEventDoorOpen);
    w.U16(static_cast<uint16_t>(gPlayState->sceneNum));
    w.U16(static_cast<uint16_t>(door->id));
    w.U16(static_cast<uint16_t>(static_cast<s16>(door->home.pos.x)));
    w.U16(static_cast<uint16_t>(static_cast<s16>(door->home.pos.y)));
    w.U16(static_cast<uint16_t>(static_cast<s16>(door->home.pos.z)));
    w.U8(door->id == ACTOR_EN_DOOR ? reinterpret_cast<EnDoor*>(door)->animStyle : 0);
    CoopGame::SendEvent(Coop::EventKind::ObjectState, w.Take());
}

// Doors aren't in the room's actor list (they're "transition actors"), but
// their spot comes from the scene data: same on every game.
Actor* FindDoor(int16_t id, s16 x, s16 y, s16 z) {
    for (int cat : { ACTORCAT_DOOR, ACTORCAT_ITEMACTION }) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (a->id == id && a->update != nullptr && static_cast<s16>(a->home.pos.x) == x &&
                static_cast<s16>(a->home.pos.y) == y && static_cast<s16>(a->home.pos.z) == z) {
                return a;
            }
        }
    }
    return nullptr;
}

void ReplayDoorOpen(Actor* door, uint8_t animStyle) {
    switch (door->id) {
        case ACTOR_EN_DOOR: {
            // the door's own code plays the opening (EnDoor_Idle -> EnDoor_Open), like
            // when Link opens it. Unlocked first: the key was used on their side,
            // otherwise EnDoor_Idle would eat one of ours.
            EnDoor* d = reinterpret_cast<EnDoor*>(door);
            if (d->actionFunc != EnDoor_Idle || d->playerIsOpening != 0) {
                return;
            }
            d->lockTimer = 0;
            d->animStyle = std::min<uint8_t>(animStyle, 3); // 4 knob animations
            d->playerIsOpening = 1;
            sReplayedDoors.insert(door);
            break;
        }
        case ACTOR_DOOR_SHUTTER: {
            // its own opening code moves OUR camera and checks OUR Link's side
            // (DoorShutter_InitOpeningDoorCam), so we play the slide ourselves
            DoorShutter* d = reinterpret_cast<DoorShutter*>(door);
            if ((d->actionFunc != DoorShutter_Idle && d->actionFunc != DoorShutter_UnbarredCheckSwitchFlag) ||
                d->unk_164 != 0 || sShutterAnims.contains(door)) {
                return;
            }
            d->unlockTimer = 0; // same as above: their key, the switch flag follows
            d->dyna.actor.velocity.y = 0.0f;
            sShutterAnims[door] = ShutterAnim{};
            sReplayedDoors.insert(door);
            break;
        }
        case ACTOR_DOOR_GERUDO: {
            // gerudo cell door: goes up and stays up (func_8099496C plays the sound)
            DoorGerudo* d = reinterpret_cast<DoorGerudo*>(door);
            if (d->actionFunc == func_8099485C) {
                d->actionFunc = func_8099496C;
                sReplayedDoors.insert(door);
            }
            break;
        }
        default:
            break;
    }
}

void OnDoorEvent(Coop::ByteReader& r) {
    uint16_t scene = 0, id = 0, x = 0, y = 0, z = 0;
    uint8_t animStyle = 0;
    if (!r.U16(scene) || !r.U16(id) || !r.U16(x) || !r.U16(y) || !r.U16(z) || !r.U8(animStyle) ||
        r.Remaining() != 0 || static_cast<int16_t>(scene) != gPlayState->sceneNum) {
        return;
    }
    int16_t actorId = static_cast<int16_t>(id);
    if (actorId != ACTOR_EN_DOOR && actorId != ACTOR_DOOR_SHUTTER && actorId != ACTOR_DOOR_GERUDO) {
        return;
    }
    if (Actor* door = FindDoor(actorId, static_cast<s16>(x), static_cast<s16>(y), static_cast<s16>(z))) {
        ReplayDoorOpen(door, animStyle);
    }
}

// Sliding door going up, waiting, coming down. Same steps and sounds as
// DoorShutter_UpdateOpening / DoorShutter_Close, minus the camera and the quake.
void StepShutters() {
    for (auto it = sShutterAnims.begin(); it != sShutterAnims.end();) {
        DoorShutter* d = reinterpret_cast<DoorShutter*>(it->first);
        Actor* a = &d->dyna.actor;
        ShutterAnim& anim = it->second;
        bool jabu = d->gfxType == 3; // Jabu's fleshy door squishes instead of sliding
        bool done = false;
        if (anim.phase == 0) {
            if (jabu) {
                if (d->jabuDoorClosedAmount == 100) {
                    Audio_PlayActorSound2(a, NA_SE_EV_BUYODOOR_OPEN);
                }
                if (Math_StepToS(&d->jabuDoorClosedAmount, 0, 10)) {
                    anim = { 1, kShutterOpenFrames };
                }
            } else {
                if (a->velocity.y == 0.0f) {
                    Audio_PlayActorSound2(a, NA_SE_EV_SLIDE_DOOR_OPEN);
                }
                Math_StepToF(&a->velocity.y, 15.0f, 3.0f);
                if (Math_StepToF(&a->world.pos.y, a->home.pos.y + 200.0f, a->velocity.y)) {
                    anim = { 1, kShutterOpenFrames };
                }
            }
        } else if (anim.phase == 1) {
            if (--anim.timer <= 0) {
                anim.phase = 2;
                a->velocity.y = 0.0f;
                Audio_PlayActorSound2(a, jabu ? NA_SE_EV_BUYODOOR_CLOSE : NA_SE_EV_SLIDE_DOOR_CLOSE);
            }
        } else if (jabu) {
            done = Math_StepToS(&d->jabuDoorClosedAmount, 100, 10);
        } else {
            Math_StepToF(&a->velocity.y, 20.0f, 8.0f);
            if (Math_StepToF(&a->world.pos.y, a->home.pos.y, a->velocity.y)) {
                Audio_PlayActorSound2(a, NA_SE_EV_STONE_BOUND);
                done = true;
            }
        }
        if (done) {
            a->velocity.y = 0.0f;
            sReplayedDoors.erase(a);
            it = sShutterAnims.erase(it);
        } else {
            ++it;
        }
    }
}

// Gerudo cell doors only check their flag when the room loads: if a teammate
// opened one (the switch flag came over), open ours too.
void OpenFlaggedGerudoDoors() {
    for (Actor* a = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; a != nullptr; a = a->next) {
        if (a->id != ACTOR_DOOR_GERUDO || a->update == nullptr) {
            continue;
        }
        DoorGerudo* d = reinterpret_cast<DoorGerudo*>(a);
        if (d->actionFunc == func_8099485C && a->world.pos.y < a->home.pos.y + 199.0f &&
            Flags_GetSwitch(gPlayState, a->params & 0x3F)) {
            d->actionFunc = func_8099496C;
            sReplayedDoors.insert(a);
        }
    }
}

void ForgetFinishedDoors() {
    std::erase_if(sReplayedDoors, [](Actor* a) {
        if (sShutterAnims.contains(a)) {
            return false;
        }
        if (a->id == ACTOR_EN_DOOR) {
            return reinterpret_cast<EnDoor*>(a)->playerIsOpening == 0; // EnDoor_Open clears it at the end
        }
        return false; // gerudo doors stay open: keep them so they're never sent back
    });
}

// ============================================================================
// hooks and registration
// ============================================================================

void ResetAll() {
    if (gPlayState != nullptr) {
        RestoreAllCulls();
    }
    sHostObjs.clear();
    sInteracts.clear();
    sMirrored.clear();
    sShutterAnims.clear();
    sReplayedDoors.clear();
    sOpenSent = nullptr;
}

void OnFrame() {
    if (!Active()) {
        ResetAll();
        return;
    }
    if (CoopGame::Session().Role() == Coop::SessionRole::Hosting) {
        sMirrored.clear();
        HostUpdate();
    } else {
        if (!sHostObjs.empty()) {
            RestoreAllCulls();
            sHostObjs.clear();
        }
        ClientUpdate();
    }
    WatchOurDoor();
    StepShutters();
    OpenFlaggedGerudoDoors();
    ForgetFinishedDoors();
}

void OnObjectEvent(const Coop::Event& e) {
    if (!Active()) {
        return;
    }
    Coop::ByteReader r(e.data);
    uint8_t sub = 0;
    if (!r.U8(sub)) {
        return;
    }
    if (sub == kEventInteract) {
        OnInteractEvent(r);
    } else if (sub == kEventDoorOpen) {
        OnDoorEvent(r);
    }
}

void OnShouldActorUpdate(void* ptr, bool* should) {
    Actor* a = static_cast<Actor*>(ptr);
    if (sShutterAnims.contains(a)) {
        *should = false; // a teammate's door: we're playing its slide
        return;
    }
    if (sMirrored.contains(a)) {
        *should = false; // the host runs it, we copy it
        return;
    }
    HostApplyInteract(a);
}

void OnShouldActorInit(void* ptr, bool*) {
    Actor* a = static_cast<Actor*>(ptr);
    sColliders.erase(a); // a new actor at an old address: whatever we knew is stale
    sHurts.erase(a);
}

void OnActorInit(void* ptr) {
    Actor* a = static_cast<Actor*>(ptr);
    if (GetActorListIndex(a) >= 0) {
        ObjectExtension::GetInstance().Set<ObjSpawnRoom>(a, ObjSpawnRoom{ a->room });
    }
}

void OnActorDestroy(void* ptr) {
    Actor* a = static_cast<Actor*>(ptr);
    sColliders.erase(a);
    sHurts.erase(a);
    sHostObjs.erase(a);
    sMirrored.erase(a);
    sShutterAnims.erase(a);
    sReplayedDoors.erase(a);
    if (sOpenSent == a) {
        sOpenSent = nullptr;
    }
}

void Register() {
    COND_HOOK(ShouldActorUpdate, true, OnShouldActorUpdate);
    COND_HOOK(ShouldActorInit, true, OnShouldActorInit);
    COND_HOOK(OnActorInit, true, OnActorInit);
    COND_HOOK(OnActorDestroy, true, OnActorDestroy);
    COND_HOOK(OnColliderSetup, true, OnColliderSetup);
    CoopGame::OnEveryFrame("objects", OnFrame);
    CoopGame::OnEvent(Coop::EventKind::ObjectState, "objects", OnObjectEvent);
}

} // namespace

static RegisterShipInitFunc sInit(Register);
