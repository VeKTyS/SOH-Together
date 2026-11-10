#include "soh/Network/Coop/Messages.h"

#include "soh/Network/Coop/Wire.h"

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

} // namespace Coop
