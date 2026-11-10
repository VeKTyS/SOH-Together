#pragma once

// The messages players send each other once connected.
// The first byte says what kind of message it is.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Coop {

struct Vec3i16 {
    int16_t x = 0, y = 0, z = 0;
    bool operator==(const Vec3i16&) const = default;
};

enum class MessageType : uint8_t {
    PlayerState = 1,         // Snapshot channel, every game frame (20 Hz)
    PlayerPose = 2,          // Snapshot channel, every game frame while in game
    FlagChange = 3,          // Ordered channel: someone set/unset a flag (chest opened, switch pressed...)
    FlagSnapshotRequest = 4, // Ordered channel: client -> host, "send me all your flags"
    FlagSnapshot = 5,        // Ordered channel: host -> client, all the flags at once
    EnemyKilled = 6,         // Ordered channel: someone killed an enemy, it dies for everyone
    EnemyStates = 7,         // Snapshot channel, host -> clients: where the host's enemies are, every frame
    EnemyHit = 8,            // Ordered channel: "I hit that enemy" (relayed by the host)
    ItemGiven = 9,           // Ordered channel: someone got an item, everyone gets it (relayed)
    DungeonItems = 10,       // Ordered channel: keys / map / compass of one dungeon (relayed)
};

// Someone got an item. Same ids as SoH's item tables: modId 0 = vanilla
// GetItemID, 1 = randomizer RandomizerGet.
struct ItemGiven {
    uint16_t modId = 0;
    int16_t getItemId = 0;
    bool operator==(const ItemGiven&) const = default;
};

std::vector<uint8_t> EncodeItemGiven(const ItemGiven& item);
std::optional<ItemGiven> DecodeItemGiven(std::span<const uint8_t> bytes);

// Small keys go up AND down (doors eat them), so we send the dungeon's
// current values instead of "+1".
struct DungeonItems {
    uint16_t mapIndex = 0;
    uint8_t items = 0; // boss key / compass / map bits
    int8_t keys = 0;
    bool operator==(const DungeonItems&) const = default;
};

std::vector<uint8_t> EncodeDungeonItems(const DungeonItems& d);
std::optional<DungeonItems> DecodeDungeonItems(std::span<const uint8_t> bytes);

// "That enemy is dead". An enemy is recognized by where it was placed in the
// level: scene + layer (child/adult, day/night...) + room + its position in the
// room's actor list. Same game data on both sides, so same numbers.
// actorId/params are there to double check we're killing the right thing.
struct EnemyKill {
    int16_t sceneNum = 0;
    uint8_t layer = 0;
    int8_t room = 0;
    int16_t listIndex = 0;
    int16_t actorId = 0;
    int16_t params = 0;
    bool operator==(const EnemyKill&) const = default;
};

std::vector<uint8_t> EncodeEnemyKill(const EnemyKill& kill);
std::optional<EnemyKill> DecodeEnemyKill(std::span<const uint8_t> bytes);

struct Vec3f32 {
    float x = 0, y = 0, z = 0;
    bool operator==(const Vec3f32&) const = default;
};

// Limits so a broken packet can't make us allocate or copy silly amounts.
inline constexpr size_t kMaxEnemiesPerPacket = 64;
inline constexpr size_t kMaxEnemyJoints = 128; // biggest boss skeleton is under 90
inline constexpr size_t kMaxEnemyQuads = 8;
inline constexpr size_t kMaxEnemyBody = 8192; // biggest enemy struct is a few KB

// One enemy as the host sees it. The client freezes its own copy and makes it
// look exactly like this.
struct EnemyState {
    int8_t room = 0;
    int16_t listIndex = 0;
    int16_t actorId = 0;
    Vec3f32 pos;
    Vec3f32 focus; // where the lock-on arrow goes
    Vec3f32 scale;
    Vec3i16 shapeRot;
    Vec3i16 worldRot;
    uint8_t health = 0;
    uint8_t submitted = 0; // which colliders the host's enemy used this frame: 1 = AT, 2 = AC, 4 = OC
    // quad colliders (swords, spears...) move with the attack, the client can't
    // guess them: 4 corners per quad, in the enemy's collider order
    std::vector<Vec3f32> quadCorners;
    std::vector<Vec3i16> joints; // skeleton pose, empty if the enemy has no skeleton
    // The rest of the enemy's memory (its own fields after the common Actor
    // part: timers, state, "on fire"...), one kind byte per 8-byte slot saying
    // how to copy it (raw / skip / pointer into the exe). See CoopEnemies.cpp.
    std::vector<uint8_t> body;
    std::vector<uint8_t> slotKinds; // size = (body.size() + 7) / 8
    bool operator==(const EnemyState&) const = default;
};

struct EnemyStates {
    int16_t sceneNum = 0;
    uint8_t layer = 0;
    std::vector<EnemyState> enemies;
    bool operator==(const EnemyStates&) const = default;
};

std::vector<uint8_t> EncodeEnemyStates(const EnemyStates& states);
// nullopt if truncated, too many entries, or a position isn't a sane number.
std::optional<EnemyStates> DecodeEnemyStates(std::span<const uint8_t> bytes);

// A client's sword/arrow/whatever touched the host's enemy. The host replays
// it through the enemy's own damage code, so weak points and i-frames still work.
struct EnemyHit {
    int16_t sceneNum = 0;
    int8_t room = 0;
    int16_t listIndex = 0;
    int16_t actorId = 0;
    uint8_t damage = 0;
    uint8_t effect = 0;    // damage effect from the enemy's table (stun, fire, ice...)
    uint32_t dmgFlags = 0; // what hit it (sword, arrow, fire...), the game's DMG_ flags
    Vec3i16 hitPos;
    bool operator==(const EnemyHit&) const = default;
};

std::vector<uint8_t> EncodeEnemyHit(const EnemyHit& hit);
std::optional<EnemyHit> DecodeEnemyHit(std::span<const uint8_t> bytes);

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
    // the host's inventory and such, as raw bytes: only the game side knows the
    // layout (both games are the same build, the handshake checks it)
    std::vector<uint8_t> saveBlob;
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
    // time of day. Clients follow the host's: day/night changes which version
    // of a scene loads (which enemies, which NPCs), it has to be the same for everyone
    uint16_t dayTime = 0;
    bool night = false;
};

// Link's skeleton has 22 limbs + 2 extra slots the animation code uses.
inline constexpr size_t kPoseJointCount = 24;
inline constexpr size_t kPoseFlameCount = 18; // PLAYER_BODYPART_MAX

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
    // on fire (fire keese, lava...): one flame timer per body part, 0 = no flame
    std::array<uint8_t, kPoseFlameCount> flameTimers{};

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
