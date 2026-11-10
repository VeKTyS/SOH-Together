#include "soh/Network/Coop/Messages.h"

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
    return w.Take();
}

std::optional<PlayerState> DecodePlayerState(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    PlayerState s;
    uint8_t type = 0, inGame = 0;
    uint16_t scene = 0, room = 0, yaw = 0;
    uint32_t entrance = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::PlayerState) || !r.U32(s.frame) || !r.U8(inGame) ||
        inGame > 1 || !r.U16(scene) || !r.U16(room) || !r.U8(s.linkAge) || !ReadFloat(r, s.x) || !ReadFloat(r, s.y) ||
        !ReadFloat(r, s.z) || !r.U16(yaw) || !r.U32(entrance) || r.Remaining() != 0) {
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
    if (!ReadVec(r, p.upperLimbRot) || !ReadVec(r, p.prevTransl) || !r.U8(p.movementFlags) ||
        !ReadS8(r, p.currentBoots) || !ReadS8(r, p.currentShield) || !ReadS8(r, p.currentTunic) ||
        !r.U32(p.stateFlags1) || !r.U32(p.stateFlags2) || !r.U8(p.buttonItem0) || !ReadS8(r, p.itemAction) ||
        !ReadS8(r, p.heldItemAction) || !r.U8(p.modelGroup) || !ReadS8(r, p.invincibilityTimer) || !r.U16(unk862) ||
        !ReadFloat(r, p.unk_85C) || !ReadS8(r, p.actionVar1) || r.Remaining() != 0) {
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

} // namespace Coop
