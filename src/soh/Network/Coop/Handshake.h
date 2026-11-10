#pragma once

// The "hello" both sides send right after connecting.
// If the game builds, mod versions or seeds don't match, we stop there and
// tell both players why, instead of letting things go weird later.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Coop {

// The 4 letters "SOHT" at the start. If they're not there, it's not us
// (some other program talking on the same port), so don't even try to read on.
inline constexpr uint32_t kHelloMagic = 0x54484F53;

// Bump this every time ANY message format changes.
// 2: PlayerState got entranceIndex (for teleporting)
// 3: new PlayerPose message (to draw the other Link)
// 4: flag sync messages (FlagChange, FlagSnapshotRequest, FlagSnapshot)
// 5: enemies (EnemyKilled, EnemyStates, EnemyHit), items (ItemGiven, DungeonItems), inventory in FlagSnapshot
// 6: time of day in PlayerState
// 7: enemy memory copy in EnemyState, flame timers in PlayerPose
// 8: ThingStates (carried / thrown things, bombs)
inline constexpr uint16_t kProtocolVersion = 8;

// 32 bytes, not 32 letters: an "é" takes 2 bytes.
inline constexpr size_t kMaxNameBytes = 32;

struct Hello {
    uint16_t protocolVersion = kProtocolVersion;
    uint64_t buildHash = 0; // identifies the exact soh.exe build
    uint64_t seedHash = 0;  // randomizer seed + settings; 0 = vanilla
    std::string playerName;
};

// Checked in this order, we only report the first problem found
// (no point comparing seeds if the game builds are different anyway).
enum class HandshakeError : uint8_t {
    None,
    BadMagic,         // not our protocol at all
    Malformed,        // truncated, trailing bytes, or invalid name
    ProtocolMismatch, // different protocol version: body not parsed
    BuildMismatch,    // same protocol, different soh.exe
    SeedMismatch,     // same build, different seed or settings
};

struct HelloDecodeResult {
    HandshakeError error = HandshakeError::None;
    Hello hello;                          // valid only when error == None
    std::optional<uint16_t> peerProtocol; // set as soon as the header was readable
};

// Truncates the name to kMaxNameBytes without splitting a UTF-8 sequence.
std::vector<uint8_t> EncodeHello(const Hello& hello);

HelloDecodeResult DecodeHello(std::span<const uint8_t> bytes);

// For now a different seed = refused. Later the guest should just switch to
// the host's seed instead.
HandshakeError CheckCompatibility(const Hello& local, const Hello& remote);

// Short message for the UI and logs.
const char* Describe(HandshakeError error);

// Turns "our commit + SoH's commit" into one number to compare.
// It's FNV-1a: simple and fast, not meant to be secure.
uint64_t BuildHash(std::string_view buildIdentity);

} // namespace Coop
