#pragma once

// The messages players send each other once connected.
// The first byte says what kind of message it is.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Coop {

enum class MessageType : uint8_t {
    PlayerState = 1, // Snapshot channel, every game frame (20 Hz)
};

// Where my Link is. Sent every frame, so if one gets lost on the way it's no
// big deal, the next one is already coming.
struct PlayerState {
    uint32_t frame = 0;   // sender's frame counter
    bool inGame = false;  // false on title screen / file select: other fields unused
    int16_t sceneNum = 0; // SceneID
    int16_t roomNum = 0;  // current room, -1 during some transitions
    uint8_t linkAge = 0;  // LINK_AGE_ADULT (0) / LINK_AGE_CHILD (1)
    float x = 0, y = 0, z = 0;
    int16_t yaw = 0; // binary angle, 0x10000 = full turn
};

// Real OoT positions stay way below this. A bigger number means a broken (or
// nasty) packet, and we don't want that anywhere near the game.
inline constexpr float kMaxCoordinate = 100000.0f;

std::optional<MessageType> PeekType(std::span<const uint8_t> bytes);

std::vector<uint8_t> EncodePlayerState(const PlayerState& state);
// nullopt if truncated, trailing bytes, wrong type, or position not finite / out of range.
std::optional<PlayerState> DecodePlayerState(std::span<const uint8_t> bytes);

} // namespace Coop
