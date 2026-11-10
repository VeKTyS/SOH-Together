#include "soh/Network/Coop/Handshake.h"

#include "soh/Network/Coop/Wire.h"

namespace Coop {

namespace {

// Cuts at most kMaxNameBytes bytes, then backs off while the cut lands in the
// middle of a multi-byte UTF-8 sequence (continuation bytes are 10xxxxxx).
size_t Utf8SafeLength(const std::string& s) {
    if (s.size() <= kMaxNameBytes) {
        return s.size();
    }
    size_t n = kMaxNameBytes;
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) {
        n--;
    }
    return n;
}

// Control characters would corrupt logs and the UI.
bool IsValidName(const std::string& name) {
    for (char c : name) {
        auto b = static_cast<uint8_t>(c);
        if (b < 0x20 || b == 0x7F) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<uint8_t> EncodeHello(const Hello& hello) {
    ByteWriter w;
    // Header: magic + version must keep this exact layout in every future
    // protocol version, so an old client can always say "version mismatch"
    // instead of "malformed".
    w.U32(kHelloMagic);
    w.U16(hello.protocolVersion);
    // Body (layout of protocol v1).
    w.U64(hello.buildHash);
    w.U64(hello.seedHash);
    size_t nameLen = Utf8SafeLength(hello.playerName);
    w.U8(static_cast<uint8_t>(nameLen));
    w.Bytes({ reinterpret_cast<const uint8_t*>(hello.playerName.data()), nameLen });
    return w.Take();
}

HelloDecodeResult DecodeHello(std::span<const uint8_t> bytes) {
    HelloDecodeResult result;
    ByteReader r(bytes);

    uint32_t magic = 0;
    if (!r.U32(magic) || magic != kHelloMagic) {
        result.error = HandshakeError::BadMagic;
        return result;
    }
    uint16_t version = 0;
    if (!r.U16(version)) {
        result.error = HandshakeError::Malformed;
        return result;
    }
    result.peerProtocol = version;
    if (version != kProtocolVersion) {
        // Do not try to parse a body whose layout we do not know.
        result.error = HandshakeError::ProtocolMismatch;
        return result;
    }

    Hello& h = result.hello;
    h.protocolVersion = version;
    uint8_t nameLen = 0;
    if (!r.U64(h.buildHash) || !r.U64(h.seedHash) || !r.U8(nameLen) || nameLen > kMaxNameBytes ||
        !r.Bytes(nameLen, h.playerName) || r.Remaining() != 0 || !IsValidName(h.playerName)) {
        result.error = HandshakeError::Malformed;
        result.hello = {};
        return result;
    }
    return result;
}

HandshakeError CheckCompatibility(const Hello& local, const Hello& remote) {
    if (local.protocolVersion != remote.protocolVersion) {
        return HandshakeError::ProtocolMismatch;
    }
    if (local.buildHash != remote.buildHash) {
        return HandshakeError::BuildMismatch;
    }
    if (local.seedHash != remote.seedHash) {
        return HandshakeError::SeedMismatch;
    }
    return HandshakeError::None;
}

const char* Describe(HandshakeError error) {
    switch (error) {
        case HandshakeError::None:
            return "OK";
        case HandshakeError::BadMagic:
            return "Not a SOH-Together connection";
        case HandshakeError::Malformed:
            return "Corrupted handshake message";
        case HandshakeError::ProtocolMismatch:
            return "Different mod version: both players must update";
        case HandshakeError::BuildMismatch:
            return "Different game build: both players must use the same soh.exe";
        case HandshakeError::SeedMismatch:
            return "Different randomizer seed or settings";
    }
    return "Unknown error";
}

uint64_t BuildHash(std::string_view buildIdentity) {
    uint64_t hash = 0xCBF29CE484222325ULL; // FNV-1a 64-bit offset basis
    for (char c : buildIdentity) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x100000001B3ULL; // FNV-1a 64-bit prime
    }
    return hash;
}

} // namespace Coop
