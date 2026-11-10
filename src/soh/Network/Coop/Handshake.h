#pragma once

// First message each side sends after connecting.
// A session only starts if both sides run the same build, the same
// protocol version and the same seed/settings (see docs: ref.md §4.2).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Coop {

// "SOHT", read as little-endian. Rejects anything that is not our protocol
// (another program on the same port, a stray packet) before parsing further.
inline constexpr uint32_t kHelloMagic = 0x54484F53;

// Bump on ANY change to any message layout.
inline constexpr uint16_t kProtocolVersion = 1;

// Player names are capped in bytes (UTF-8), not characters.
inline constexpr size_t kMaxNameBytes = 32;

struct Hello {
    uint16_t protocolVersion = kProtocolVersion;
    uint64_t buildHash = 0; // identifies the exact soh.exe build
    uint64_t seedHash = 0;  // randomizer seed + settings; 0 = vanilla
    std::string playerName;
};

// Ordered by check priority: the first mismatch found is the one reported.
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

// Compares two decoded Hellos. Does not decide what to do on SeedMismatch:
// for now the session is refused; later the guest will switch to the host's
// seed (ref.md §4.10).
HandshakeError CheckCompatibility(const Hello& local, const Hello& remote);

// Short message for the UI and logs.
const char* Describe(HandshakeError error);

} // namespace Coop
