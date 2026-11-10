#pragma once

// The messages players send each other once connected.
// The first byte says what kind of message it is.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Coop {

enum class MessageType : uint8_t {
    PlayerState = 1,         // Snapshot channel, every game frame (20 Hz)
    PlayerPose = 2,          // Snapshot channel, every game frame while in game
    FlagChange = 3,          // Ordered channel: someone set/unset a flag (chest opened, switch pressed...)
    FlagSnapshotRequest = 4, // Ordered channel: client -> host, "send me all your flags"
    FlagSnapshot = 5,        // Ordered channel: host -> client, all the flags at once
};

// One flag going up or down. sceneNum = -1 for the global ones (story events etc).
// flagType uses SoH's FlagType enum, the game side checks it's one we sync.
struct FlagChange {
    int16_t sceneNum = -1;
    uint8_t flagType = 0;
    int16_t flag = 0;
    bool set = true;
    bool operator==(const FlagChange&) const = default;
};

// Every flag that makes up "where we are in the game", sent when a client
// loads their save so they end up in the host's world. Arrays are sent with
// their length: the game side refuses it if the lengths don't match its own.
struct FlagSnapshot {
    // per scene: chest, swch, clear, collect, unk, rooms, floors (SoH's SavedSceneFlags)
    std::vector<std::array<uint32_t, 7>> scenes;
    std::vector<uint16_t> eventChkInf;
    std::vector<uint16_t> itemGetInf;
    std::vector<uint16_t> infTable;
    std::vector<uint16_t> randomizerInf;
    bool operator==(const FlagSnapshot&) const = default;
};

std::vector<uint8_t> EncodeFlagChange(const FlagChange& change);
std::optional<FlagChange> DecodeFlagChange(std::span<const uint8_t> bytes);

std::vector<uint8_t> EncodeFlagSnapshotRequest();

std::vector<uint8_t> EncodeFlagSnapshot(const FlagSnapshot& snapshot);
std::optional<FlagSnapshot> DecodeFlagSnapshot(std::span<const uint8_t> bytes);

// Where my Link is. Sent every frame, so if one gets lost on the way it's no
// big deal, the next one is already coming.
struct PlayerState {
    uint32_t frame = 0;   // sender's frame counter
    bool inGame = false;  // false on title screen / file select: other fields unused
    int16_t sceneNum = 0; // SceneID
    int16_t roomNum = 0;  // current room, -1 during some transitions
    uint8_t linkAge = 0;  // LINK_AGE_ADULT (0) / LINK_AGE_CHILD (1)
    float x = 0, y = 0, z = 0;
    int16_t yaw = 0;            // binary angle, 0x10000 = full turn
    int32_t entranceIndex = -1; // the door/loading zone they came in by, needed to teleport to them
};

struct Vec3i16 {
    int16_t x = 0, y = 0, z = 0;
    bool operator==(const Vec3i16&) const = default;
};

// Link's skeleton has 22 limbs + 2 extra slots the animation code uses.
inline constexpr size_t kPoseJointCount = 24;

// Everything needed to draw someone else's Link exactly like they see it:
// skeleton angles, what they wear and hold. Field names follow SoH's Player
// struct (same set as Anchor sends). Values aren't range-checked here, the
// game side clamps them because it knows the limits (enum sizes).
struct PlayerPose {
    Vec3i16 joints[kPoseJointCount];
    Vec3i16 upperLimbRot;
    Vec3i16 prevTransl;
    uint8_t movementFlags = 0;
    int8_t currentBoots = 0;
    int8_t currentShield = 0;
    int8_t currentTunic = 0;
    uint32_t stateFlags1 = 0;
    uint32_t stateFlags2 = 0;
    uint8_t buttonItem0 = 0; // item on B
    int8_t itemAction = 0;
    int8_t heldItemAction = 0;
    uint8_t modelGroup = 0;
    int8_t invincibilityTimer = 0;
    int16_t unk_862 = 0; // item shown above Link's head when getting something
    float unk_85C = 0;   // deku stick length and such
    int8_t actionVar1 = 0;

    bool operator==(const PlayerPose&) const = default;
};

std::vector<uint8_t> EncodePlayerPose(const PlayerPose& pose);
// nullopt if truncated, trailing bytes, wrong type or unk_85C not a normal number.
std::optional<PlayerPose> DecodePlayerPose(std::span<const uint8_t> bytes);

// Real OoT positions stay way below this. A bigger number means a broken (or
// nasty) packet, and we don't want that anywhere near the game.
inline constexpr float kMaxCoordinate = 100000.0f;

std::optional<MessageType> PeekType(std::span<const uint8_t> bytes);

std::vector<uint8_t> EncodePlayerState(const PlayerState& state);
// nullopt if truncated, trailing bytes, wrong type, or position not finite / out of range.
std::optional<PlayerState> DecodePlayerState(std::span<const uint8_t> bytes);

} // namespace Coop
