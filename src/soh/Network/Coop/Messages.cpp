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
    return w.Take();
}

std::optional<PlayerState> DecodePlayerState(std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    PlayerState s;
    uint8_t type = 0, inGame = 0;
    uint16_t scene = 0, room = 0, yaw = 0;
    if (!r.U8(type) || type != static_cast<uint8_t>(MessageType::PlayerState) || !r.U32(s.frame) || !r.U8(inGame) ||
        inGame > 1 || !r.U16(scene) || !r.U16(room) || !r.U8(s.linkAge) || !ReadFloat(r, s.x) || !ReadFloat(r, s.y) ||
        !ReadFloat(r, s.z) || !r.U16(yaw) || r.Remaining() != 0) {
        return std::nullopt;
    }
    if (!IsSaneCoordinate(s.x) || !IsSaneCoordinate(s.y) || !IsSaneCoordinate(s.z)) {
        return std::nullopt;
    }
    s.inGame = inGame == 1;
    s.sceneNum = static_cast<int16_t>(scene);
    s.roomNum = static_cast<int16_t>(room);
    s.yaw = static_cast<int16_t>(yaw);
    return s;
}

} // namespace Coop
