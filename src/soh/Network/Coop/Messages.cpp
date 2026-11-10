#include "soh/Network/Coop/Messages.h"

#include "soh/Network/Coop/Handshake.h"
#include "soh/Network/Coop/Wire.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Coop {

namespace {

// Floats travel as their IEEE-754 bit pattern: exact, and the same on every
// platform we target (all little-endian, all IEEE-754).
void WriteFloat(ByteWriter& w, float v) {
    w.U32(std::bit_cast<uint32_t>(v));
}

bool ReadFloat(ByteReader& r, float& out) {
    uint32_t bits = 0;
    if (!r.U32(bits)) {
        return false;
    }
    out = std::bit_cast<float>(bits);
    return true;
}

bool IsSaneCoordinate(float v) {
    return std::isfinite(v) && std::fabs(v) <= kMaxCoordinate;
}

} // namespace

std::optional<MessageType> PeekType(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        return std::nullopt;
    }
    return static_cast<MessageType>(bytes[0]);
}

std::vector<uint8_t> EncodePlayerState(const PlayerState& s) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::PlayerState));
    w.U32(s.frame);
    w.U8(s.inGame ? 1 : 0);
    w.U16(static_cast<uint16_t>(s.sceneNum));
    w.U16(static_cast<uint16_t>(s.roomNum));
    w.U8(s.linkAge);
    WriteFloat(w, s.x);
    WriteFloat(w, s.y);
    WriteFloat(w, s.z);
    w.U16(static_cast<uint16_t>(s.yaw));
    w.U32(static_cast<uint32_t>(s.entranceIndex));
    w.U16(s.dayTime);
    w.U8(s.night ? 1 : 0);
    w.U8(s.colorR);
    w.U8(s.colorG);
    w.U8(s.colorB);
    w.U8(s.team);
    return w.Take();
}

std::optional<PlayerState> DecodePlayerState(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    PlayerState s;
    uint8_t type = 0, inGame = 0, night = 0;
    uint16_t scene = 0, room = 0, yaw = 0;
    uint32_t entrance = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::PlayerState) || !r.U32(s.frame) || !r.U8(inGame) ||
        inGame > 1 || !r.U16(scene) || !r.U16(room) || !r.U8(s.linkAge) || !ReadFloat(r, s.x) || !ReadFloat(r, s.y) ||
        !ReadFloat(r, s.z) || !r.U16(yaw) || !r.U32(entrance) || !r.U16(s.dayTime) || !r.U8(night) || night > 1 ||
        !r.U8(s.colorR) || !r.U8(s.colorG) || !r.U8(s.colorB) || !r.U8(s.team) || r.Remaining() != 0) {
        return std::nullopt;
    }
    if (!IsSaneCoordinate(s.x) || !IsSaneCoordinate(s.y) || !IsSaneCoordinate(s.z)) {
        return std::nullopt;
    }
    s.inGame = inGame == 1;
    s.sceneNum = static_cast<int16_t>(scene);
    s.roomNum = static_cast<int16_t>(room);
    s.yaw = static_cast<int16_t>(yaw);
    s.entranceIndex = static_cast<int32_t>(entrance);
    s.night = night == 1;
    return s;
}

namespace {

void WriteVec(ByteWriter& w, const Vec3i16& v) {
    w.U16(static_cast<uint16_t>(v.x));
    w.U16(static_cast<uint16_t>(v.y));
    w.U16(static_cast<uint16_t>(v.z));
}

bool ReadVec(ByteReader& r, Vec3i16& v) {
    uint16_t x = 0, y = 0, z = 0;
    if (!r.U16(x) || !r.U16(y) || !r.U16(z)) {
        return false;
    }
    v = { static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z) };
    return true;
}

// signed 8-bit helpers, to keep the encode/decode lists readable
bool ReadS8(ByteReader& r, int8_t& out) {
    uint8_t v = 0;
    if (!r.U8(v)) {
        return false;
    }
    out = static_cast<int8_t>(v);
    return true;
}

} // namespace

std::vector<uint8_t> EncodePlayerPose(const PlayerPose& p) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::PlayerPose));
    for (const Vec3i16& joint : p.joints) {
        WriteVec(w, joint);
    }
    WriteVec(w, p.upperLimbRot);
    WriteVec(w, p.prevTransl);
    w.U16(static_cast<uint16_t>(p.rotX));
    w.U16(static_cast<uint16_t>(p.rotZ));
    w.U8(p.movementFlags);
    w.U8(static_cast<uint8_t>(p.currentBoots));
    w.U8(static_cast<uint8_t>(p.currentShield));
    w.U8(static_cast<uint8_t>(p.currentTunic));
    w.U32(p.stateFlags1);
    w.U32(p.stateFlags2);
    w.U8(p.buttonItem0);
    w.U8(static_cast<uint8_t>(p.itemAction));
    w.U8(static_cast<uint8_t>(p.heldItemAction));
    w.U8(p.modelGroup);
    w.U8(static_cast<uint8_t>(p.invincibilityTimer));
    w.U16(static_cast<uint16_t>(p.unk_862));
    WriteFloat(w, p.unk_85C);
    w.U8(static_cast<uint8_t>(p.actionVar1));
    for (uint8_t t : p.flameTimers) {
        w.U8(t);
    }
    return w.Take();
}

std::optional<PlayerPose> DecodePlayerPose(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    PlayerPose p;
    uint8_t type = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::PlayerPose)) {
        return std::nullopt;
    }
    for (Vec3i16& joint : p.joints) {
        if (!ReadVec(r, joint)) {
            return std::nullopt;
        }
    }
    uint16_t unk862 = 0;
    uint16_t rotX = 0, rotZ = 0;
    if (!ReadVec(r, p.upperLimbRot) || !ReadVec(r, p.prevTransl) || !r.U16(rotX) || !r.U16(rotZ)) {
        return std::nullopt;
    }
    p.rotX = static_cast<int16_t>(rotX);
    p.rotZ = static_cast<int16_t>(rotZ);
    if (!r.U8(p.movementFlags) ||
        !ReadS8(r, p.currentBoots) || !ReadS8(r, p.currentShield) || !ReadS8(r, p.currentTunic) ||
        !r.U32(p.stateFlags1) || !r.U32(p.stateFlags2) || !r.U8(p.buttonItem0) || !ReadS8(r, p.itemAction) ||
        !ReadS8(r, p.heldItemAction) || !r.U8(p.modelGroup) || !ReadS8(r, p.invincibilityTimer) || !r.U16(unk862) ||
        !ReadFloat(r, p.unk_85C) || !ReadS8(r, p.actionVar1)) {
        return std::nullopt;
    }
    for (uint8_t& t : p.flameTimers) {
        if (!r.U8(t)) {
            return std::nullopt;
        }
    }
    if (r.Remaining() != 0) {
        return std::nullopt;
    }
    if (!std::isfinite(p.unk_85C)) {
        return std::nullopt;
    }
    p.unk_862 = static_cast<int16_t>(unk862);
    return p;
}

std::vector<uint8_t> EncodeFlagChange(const FlagChange& c) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::FlagChange));
    w.U16(static_cast<uint16_t>(c.sceneNum));
    w.U8(c.flagType);
    w.U16(static_cast<uint16_t>(c.flag));
    w.U8(c.set ? 1 : 0);
    return w.Take();
}

std::optional<FlagChange> DecodeFlagChange(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    FlagChange c;
    uint8_t type = 0, set = 0;
    uint16_t scene = 0, flag = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::FlagChange) || !r.U16(scene) || !r.U8(c.flagType) ||
        !r.U16(flag) || !r.U8(set) || set > 1 || r.Remaining() != 0) {
        return std::nullopt;
    }
    c.sceneNum = static_cast<int16_t>(scene);
    c.flag = static_cast<int16_t>(flag);
    c.set = set == 1;
    return c;
}

std::vector<uint8_t> EncodeFlagSnapshotRequest() {
    return { static_cast<uint8_t>(MessageType::FlagSnapshotRequest) };
}

namespace {

void WriteU16Array(ByteWriter& w, const std::vector<uint16_t>& values) {
    w.U16(static_cast<uint16_t>(values.size()));
    for (uint16_t v : values) {
        w.U16(v);
    }
}

bool ReadU16Array(ByteReader& r, std::vector<uint16_t>& out) {
    uint16_t count = 0;
    // each value is 2 bytes: refuse a count that can't fit in what's left
    // (otherwise a tiny packet could make us reserve a huge vector)
    if (!r.U16(count) || r.Remaining() < static_cast<size_t>(count) * 2) {
        return false;
    }
    out.resize(count);
    for (uint16_t& v : out) {
        r.U16(v);
    }
    return true;
}

} // namespace

std::vector<uint8_t> EncodeFlagSnapshot(const FlagSnapshot& s) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::FlagSnapshot));
    w.U16(static_cast<uint16_t>(s.scenes.size()));
    for (const auto& scene : s.scenes) {
        for (uint32_t v : scene) {
            w.U32(v);
        }
    }
    WriteU16Array(w, s.eventChkInf);
    WriteU16Array(w, s.itemGetInf);
    WriteU16Array(w, s.infTable);
    WriteU16Array(w, s.randomizerInf);
    w.U16(static_cast<uint16_t>(s.saveBlob.size()));
    for (uint8_t b : s.saveBlob) {
        w.U8(b);
    }
    return w.Take();
}

std::optional<FlagSnapshot> DecodeFlagSnapshot(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    FlagSnapshot s;
    uint8_t type = 0;
    uint16_t sceneCount = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::FlagSnapshot) || !r.U16(sceneCount) ||
        r.Remaining() < static_cast<size_t>(sceneCount) * 7 * 4) {
        return std::nullopt;
    }
    s.scenes.resize(sceneCount);
    for (auto& scene : s.scenes) {
        for (uint32_t& v : scene) {
            r.U32(v);
        }
    }
    uint16_t blobSize = 0;
    if (!ReadU16Array(r, s.eventChkInf) || !ReadU16Array(r, s.itemGetInf) || !ReadU16Array(r, s.infTable) ||
        !ReadU16Array(r, s.randomizerInf) || !r.U16(blobSize) || r.Remaining() != blobSize) {
        return std::nullopt;
    }
    s.saveBlob.resize(blobSize);
    for (uint8_t& b : s.saveBlob) {
        r.U8(b);
    }
    return s;
}

std::vector<uint8_t> EncodeEnemyKill(const EnemyKill& k) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::EnemyKilled));
    w.U16(static_cast<uint16_t>(k.sceneNum));
    w.U8(k.layer);
    w.U8(static_cast<uint8_t>(k.room));
    w.U16(static_cast<uint16_t>(k.listIndex));
    w.U16(static_cast<uint16_t>(k.actorId));
    w.U16(static_cast<uint16_t>(k.params));
    return w.Take();
}

std::optional<EnemyKill> DecodeEnemyKill(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    EnemyKill k;
    uint8_t type = 0, room = 0;
    uint16_t scene = 0, index = 0, id = 0, params = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::EnemyKilled) || !r.U16(scene) || !r.U8(k.layer) ||
        !r.U8(room) || !r.U16(index) || !r.U16(id) || !r.U16(params) || r.Remaining() != 0) {
        return std::nullopt;
    }
    k.sceneNum = static_cast<int16_t>(scene);
    k.room = static_cast<int8_t>(room);
    k.listIndex = static_cast<int16_t>(index);
    k.actorId = static_cast<int16_t>(id);
    k.params = static_cast<int16_t>(params);
    return k;
}

namespace {

void WriteVecF(ByteWriter& w, const Vec3f32& v) {
    WriteFloat(w, v.x);
    WriteFloat(w, v.y);
    WriteFloat(w, v.z);
}

// only sane positions get through
bool ReadVecF(ByteReader& r, Vec3f32& v) {
    return ReadFloat(r, v.x) && ReadFloat(r, v.y) && ReadFloat(r, v.z) && IsSaneCoordinate(v.x) &&
           IsSaneCoordinate(v.y) && IsSaneCoordinate(v.z);
}

bool ReadEnemyState(ByteReader& r, EnemyState& e) {
    uint8_t room = 0, quadCount = 0, jointCount = 0;
    uint16_t index = 0, id = 0;
    if (!r.U8(room) || !r.U16(index) || !r.U16(id) || !ReadVecF(r, e.pos) || !ReadVecF(r, e.focus) ||
        !ReadVecF(r, e.scale) || !ReadVec(r, e.shapeRot) || !ReadVec(r, e.worldRot) || !r.U8(e.health) ||
        !r.U8(e.submitted) || !r.U8(quadCount) || quadCount > kMaxEnemyQuads) {
        return false;
    }
    e.room = static_cast<int8_t>(room);
    e.listIndex = static_cast<int16_t>(index);
    e.actorId = static_cast<int16_t>(id);
    e.quadCorners.resize(static_cast<size_t>(quadCount) * 4);
    for (Vec3f32& c : e.quadCorners) {
        if (!ReadVecF(r, c)) {
            return false;
        }
    }
    if (!r.U8(jointCount) || jointCount > kMaxEnemyJoints || r.Remaining() < static_cast<size_t>(jointCount) * 6) {
        return false;
    }
    e.joints.resize(jointCount);
    for (Vec3i16& j : e.joints) {
        ReadVec(r, j);
    }
    uint16_t bodySize = 0;
    size_t slots = 0;
    if (!r.U16(bodySize) || bodySize > kMaxEnemyBody ||
        r.Remaining() < static_cast<size_t>(bodySize) + (slots = (bodySize + 7u) / 8u)) {
        return false;
    }
    e.body.resize(bodySize);
    for (uint8_t& b : e.body) {
        r.U8(b);
    }
    e.slotKinds.resize(slots);
    for (uint8_t& k : e.slotKinds) {
        r.U8(k);
    }
    return true;
}

} // namespace

std::vector<uint8_t> EncodeEnemyStates(const EnemyStates& s) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::EnemyStates));
    w.U16(static_cast<uint16_t>(s.sceneNum));
    w.U8(s.layer);
    size_t count = std::min(s.enemies.size(), kMaxEnemiesPerPacket);
    w.U8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; i++) {
        const EnemyState& e = s.enemies[i];
        w.U8(static_cast<uint8_t>(e.room));
        w.U16(static_cast<uint16_t>(e.listIndex));
        w.U16(static_cast<uint16_t>(e.actorId));
        WriteVecF(w, e.pos);
        WriteVecF(w, e.focus);
        WriteVecF(w, e.scale);
        WriteVec(w, e.shapeRot);
        WriteVec(w, e.worldRot);
        w.U8(e.health);
        w.U8(e.submitted);
        size_t quads = std::min(e.quadCorners.size() / 4, kMaxEnemyQuads);
        w.U8(static_cast<uint8_t>(quads));
        for (size_t q = 0; q < quads * 4; q++) {
            WriteVecF(w, e.quadCorners[q]);
        }
        size_t joints = std::min(e.joints.size(), kMaxEnemyJoints);
        w.U8(static_cast<uint8_t>(joints));
        for (size_t j = 0; j < joints; j++) {
            WriteVec(w, e.joints[j]);
        }
        bool bodyOk = e.body.size() <= kMaxEnemyBody && e.slotKinds.size() == (e.body.size() + 7) / 8;
        w.U16(static_cast<uint16_t>(bodyOk ? e.body.size() : 0));
        if (bodyOk) {
            for (uint8_t b : e.body) {
                w.U8(b);
            }
            for (uint8_t k : e.slotKinds) {
                w.U8(k);
            }
        }
    }
    return w.Take();
}

std::optional<EnemyStates> DecodeEnemyStates(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    EnemyStates s;
    uint8_t type = 0, count = 0;
    uint16_t scene = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::EnemyStates) || !r.U16(scene) || !r.U8(s.layer) ||
        !r.U8(count) || count > kMaxEnemiesPerPacket) {
        return std::nullopt;
    }
    s.sceneNum = static_cast<int16_t>(scene);
    s.enemies.resize(count);
    for (EnemyState& e : s.enemies) {
        if (!ReadEnemyState(r, e)) {
            return std::nullopt;
        }
    }
    if (r.Remaining() != 0) {
        return std::nullopt;
    }
    return s;
}

std::vector<uint8_t> EncodeEnemyHit(const EnemyHit& h) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::EnemyHit));
    w.U16(static_cast<uint16_t>(h.sceneNum));
    w.U8(static_cast<uint8_t>(h.room));
    w.U16(static_cast<uint16_t>(h.listIndex));
    w.U16(static_cast<uint16_t>(h.actorId));
    w.U8(h.damage);
    w.U8(h.effect);
    w.U32(h.dmgFlags);
    WriteVec(w, h.hitPos);
    return w.Take();
}

std::optional<EnemyHit> DecodeEnemyHit(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    EnemyHit h;
    uint8_t type = 0, room = 0;
    uint16_t scene = 0, index = 0, id = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::EnemyHit) || !r.U16(scene) || !r.U8(room) ||
        !r.U16(index) || !r.U16(id) || !r.U8(h.damage) || !r.U8(h.effect) || !r.U32(h.dmgFlags) ||
        !ReadVec(r, h.hitPos) || r.Remaining() != 0) {
        return std::nullopt;
    }
    h.sceneNum = static_cast<int16_t>(scene);
    h.room = static_cast<int8_t>(room);
    h.listIndex = static_cast<int16_t>(index);
    h.actorId = static_cast<int16_t>(id);
    return h;
}

std::vector<uint8_t> EncodeItemGiven(const ItemGiven& item) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::ItemGiven));
    w.U16(item.modId);
    w.U16(static_cast<uint16_t>(item.getItemId));
    return w.Take();
}

std::optional<ItemGiven> DecodeItemGiven(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    ItemGiven item;
    uint8_t type = 0;
    uint16_t id = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::ItemGiven) || !r.U16(item.modId) || !r.U16(id) ||
        r.Remaining() != 0) {
        return std::nullopt;
    }
    item.getItemId = static_cast<int16_t>(id);
    return item;
}

std::vector<uint8_t> EncodeDungeonItems(const DungeonItems& d) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::DungeonItems));
    w.U16(d.mapIndex);
    w.U8(d.items);
    w.U8(static_cast<uint8_t>(d.keys));
    return w.Take();
}

std::optional<DungeonItems> DecodeDungeonItems(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    DungeonItems d;
    uint8_t type = 0, keys = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::DungeonItems) || !r.U16(d.mapIndex) ||
        !r.U8(d.items) || !r.U8(keys) || r.Remaining() != 0) {
        return std::nullopt;
    }
    d.keys = static_cast<int8_t>(keys);
    return d;
}

namespace {

void WriteBody(ByteWriter& w, const std::vector<uint8_t>& body, const std::vector<uint8_t>& kinds) {
    bool ok = body.size() <= kMaxEnemyBody && kinds.size() == (body.size() + 7) / 8;
    w.U16(static_cast<uint16_t>(ok ? body.size() : 0));
    if (ok) {
        for (uint8_t b : body) {
            w.U8(b);
        }
        for (uint8_t k : kinds) {
            w.U8(k);
        }
    }
}

bool ReadBody(ByteReader& r, std::vector<uint8_t>& body, std::vector<uint8_t>& kinds) {
    uint16_t size = 0;
    if (!r.U16(size) || size > kMaxEnemyBody) {
        return false;
    }
    size_t slots = (size + 7u) / 8u;
    if (r.Remaining() < static_cast<size_t>(size) + slots) {
        return false;
    }
    body.resize(size);
    for (uint8_t& b : body) {
        r.U8(b);
    }
    kinds.resize(slots);
    for (uint8_t& k : kinds) {
        r.U8(k);
    }
    return true;
}

} // namespace

std::vector<uint8_t> EncodeThingStates(const ThingStates& s) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::ThingStates));
    w.U32(s.owner);
    w.U16(static_cast<uint16_t>(s.sceneNum));
    w.U8(s.layer);
    size_t count = std::min(s.things.size(), kMaxThings);
    w.U8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; i++) {
        const ThingState& t = s.things[i];
        w.U16(t.localId);
        w.U8(static_cast<uint8_t>(t.room));
        w.U16(static_cast<uint16_t>(t.listIndex));
        w.U16(static_cast<uint16_t>(t.actorId));
        w.U16(static_cast<uint16_t>(t.params));
        WriteVecF(w, t.pos);
        WriteVec(w, t.rot);
        WriteBody(w, t.body, t.slotKinds);
    }
    return w.Take();
}

std::optional<ThingStates> DecodeThingStates(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    ThingStates s;
    uint8_t type = 0, count = 0;
    uint16_t scene = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::ThingStates) || !r.U32(s.owner) || !r.U16(scene) ||
        !r.U8(s.layer) || !r.U8(count) || count > kMaxThings) {
        return std::nullopt;
    }
    s.sceneNum = static_cast<int16_t>(scene);
    s.things.resize(count);
    for (ThingState& t : s.things) {
        uint8_t room = 0;
        uint16_t index = 0, id = 0, params = 0;
        if (!r.U16(t.localId) || !r.U8(room) || !r.U16(index) || !r.U16(id) || !r.U16(params) || !ReadVecF(r, t.pos) ||
            !ReadVec(r, t.rot) || !ReadBody(r, t.body, t.slotKinds)) {
            return std::nullopt;
        }
        t.room = static_cast<int8_t>(room);
        t.listIndex = static_cast<int16_t>(index);
        t.actorId = static_cast<int16_t>(id);
        t.params = static_cast<int16_t>(params);
    }
    if (r.Remaining() != 0) {
        return std::nullopt;
    }
    return s;
}

// --- player ids, relayed players, events ---

std::vector<uint8_t> EncodeWelcome(const Welcome& m) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::Welcome));
    w.U32(m.playerId);
    return w.Take();
}

std::optional<Welcome> DecodeWelcome(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    Welcome m;
    uint8_t type = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::Welcome) || !r.U32(m.playerId) || m.playerId == 0 ||
        r.Remaining() != 0) {
        return std::nullopt;
    }
    return m;
}

std::vector<uint8_t> EncodeForwarded(const Forwarded& f) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::Forwarded));
    w.U32(f.origin);
    size_t nameLen = std::min<size_t>(f.name.size(), kMaxNameBytes);
    w.U8(static_cast<uint8_t>(nameLen));
    w.Bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(f.name.data()), nameLen));
    w.Bytes(f.inner);
    return w.Take();
}

std::optional<Forwarded> DecodeForwarded(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    Forwarded f;
    uint8_t type = 0, nameLen = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::Forwarded) || !r.U32(f.origin) || f.origin == 0 ||
        !r.U8(nameLen) || nameLen > kMaxNameBytes || !r.Bytes(nameLen, f.name) || r.Remaining() == 0) {
        return std::nullopt;
    }
    f.inner.assign(bytes.end() - static_cast<std::ptrdiff_t>(r.Remaining()), bytes.end());
    return f;
}

std::vector<uint8_t> EncodePeerLeft(const PeerLeft& m) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::PeerLeft));
    w.U32(m.playerId);
    return w.Take();
}

std::optional<PeerLeft> DecodePeerLeft(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    PeerLeft m;
    uint8_t type = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::PeerLeft) || !r.U32(m.playerId) || r.Remaining() != 0) {
        return std::nullopt;
    }
    return m;
}

std::vector<uint8_t> EncodeEvent(const Event& e) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::Event));
    w.U32(e.origin);
    w.U8(static_cast<uint8_t>(e.kind));
    size_t len = std::min(e.data.size(), kMaxEventData);
    w.U16(static_cast<uint16_t>(len));
    w.Bytes(std::span<const uint8_t>(e.data.data(), len));
    return w.Take();
}

std::optional<Event> DecodeEvent(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    Event e;
    uint8_t type = 0, kind = 0;
    uint16_t len = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::Event) || !r.U32(e.origin) || !r.U8(kind) ||
        kind == 0 || !r.U16(len) || len > kMaxEventData || r.Remaining() != len) {
        return std::nullopt;
    }
    e.kind = static_cast<EventKind>(kind);
    e.data.assign(bytes.end() - len, bytes.end());
    return e;
}

std::vector<uint8_t> EncodeObjectStates(const ObjectStates& s) {
    ByteWriter w;
    w.U8(static_cast<uint8_t>(MessageType::ObjectStates));
    w.U16(static_cast<uint16_t>(s.sceneNum));
    w.U8(s.layer);
    size_t count = std::min(s.objects.size(), kMaxObjects);
    w.U8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; i++) {
        const ObjectState& o = s.objects[i];
        w.U8(static_cast<uint8_t>(o.room));
        w.U16(static_cast<uint16_t>(o.listIndex));
        w.U16(static_cast<uint16_t>(o.actorId));
        WriteVecF(w, o.pos);
        WriteVec(w, o.shapeRot);
        WriteVec(w, o.worldRot);
        WriteVecF(w, o.scale);
        WriteBody(w, o.body, o.slotKinds);
    }
    return w.Take();
}

std::optional<ObjectStates> DecodeObjectStates(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    ObjectStates s;
    uint8_t type = 0, count = 0;
    uint16_t scene = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::ObjectStates) || !r.U16(scene) || !r.U8(s.layer) ||
        !r.U8(count) || count > kMaxObjects) {
        return std::nullopt;
    }
    s.sceneNum = static_cast<int16_t>(scene);
    s.objects.resize(count);
    for (ObjectState& o : s.objects) {
        uint8_t room = 0;
        uint16_t index = 0, id = 0;
        if (!r.U8(room) || !r.U16(index) || !r.U16(id) || !ReadVecF(r, o.pos) || !ReadVec(r, o.shapeRot) ||
            !ReadVec(r, o.worldRot) || !ReadVecF(r, o.scale) || !ReadBody(r, o.body, o.slotKinds)) {
            return std::nullopt;
        }
        o.room = static_cast<int8_t>(room);
        o.listIndex = static_cast<int16_t>(index);
        o.actorId = static_cast<int16_t>(id);
    }
    if (r.Remaining() != 0) {
        return std::nullopt;
    }
    return s;
}

} // namespace Coop
