#include "soh/Network/Coop/Handshake.h"

#include <gtest/gtest.h>

using namespace Coop;

namespace {

Hello MakeHello() {
    Hello h;
    h.buildHash = 0x1122334455667788ULL;
    h.seedHash = 0xA1B2C3D4E5F60718ULL;
    h.playerName = "Link";
    return h;
}

} // namespace

TEST(Handshake, RoundTrip) {
    Hello in = MakeHello();
    HelloDecodeResult out = DecodeHello(EncodeHello(in));

    ASSERT_EQ(out.error, HandshakeError::None);
    EXPECT_EQ(out.hello.protocolVersion, kProtocolVersion);
    EXPECT_EQ(out.hello.buildHash, in.buildHash);
    EXPECT_EQ(out.hello.seedHash, in.seedHash);
    EXPECT_EQ(out.hello.playerName, in.playerName);
}

TEST(Handshake, WireLayoutIsLittleEndian) {
    std::vector<uint8_t> bytes = EncodeHello(MakeHello());
    // Magic "SOHT" then protocol version 1, lowest byte first.
    ASSERT_GE(bytes.size(), 6u);
    EXPECT_EQ(bytes[0], 'S');
    EXPECT_EQ(bytes[1], 'O');
    EXPECT_EQ(bytes[2], 'H');
    EXPECT_EQ(bytes[3], 'T');
    EXPECT_EQ(bytes[4], 0x01);
    EXPECT_EQ(bytes[5], 0x00);
}

// Every possible truncation must fail cleanly, never read out of bounds.
TEST(Handshake, EveryTruncationIsRejected) {
    std::vector<uint8_t> full = EncodeHello(MakeHello());
    for (size_t len = 0; len < full.size(); len++) {
        HelloDecodeResult out = DecodeHello(std::span<const uint8_t>(full.data(), len));
        EXPECT_NE(out.error, HandshakeError::None) << "accepted a message cut at " << len << " bytes";
    }
}

TEST(Handshake, TrailingBytesAreRejected) {
    std::vector<uint8_t> bytes = EncodeHello(MakeHello());
    bytes.push_back(0);
    EXPECT_EQ(DecodeHello(bytes).error, HandshakeError::Malformed);
}

TEST(Handshake, ForeignDataIsBadMagic) {
    std::vector<uint8_t> http = { 'G', 'E', 'T', ' ', '/', ' ' };
    EXPECT_EQ(DecodeHello(http).error, HandshakeError::BadMagic);
}

// A future version may change the body layout: we must report a version
// mismatch (actionable) and not "malformed" (confusing).
TEST(Handshake, FutureVersionIsProtocolMismatchNotMalformed) {
    std::vector<uint8_t> bytes = { 'S', 'O', 'H', 'T', 0x02, 0x00, 0xFF, 0xFF, 0xFF };
    HelloDecodeResult out = DecodeHello(bytes);

    EXPECT_EQ(out.error, HandshakeError::ProtocolMismatch);
    ASSERT_TRUE(out.peerProtocol.has_value());
    EXPECT_EQ(*out.peerProtocol, 2);
}

TEST(Handshake, NameLengthAboveLimitIsRejected) {
    std::vector<uint8_t> bytes = EncodeHello(MakeHello());
    // Name length byte sits right after magic(4) + version(2) + build(8) + seed(8).
    bytes[22] = static_cast<uint8_t>(kMaxNameBytes + 1);
    EXPECT_EQ(DecodeHello(bytes).error, HandshakeError::Malformed);
}

TEST(Handshake, ControlCharactersInNameAreRejected) {
    Hello h = MakeHello();
    h.playerName = "Li\nnk";
    EXPECT_EQ(DecodeHello(EncodeHello(h)).error, HandshakeError::Malformed);
}

TEST(Handshake, LongNameIsTruncatedWithoutSplittingUtf8) {
    Hello h = MakeHello();
    // 31 ASCII bytes then "é" (2 bytes): the 32-byte cut would split "é".
    h.playerName = std::string(31, 'a') + "\xC3\xA9";
    HelloDecodeResult out = DecodeHello(EncodeHello(h));

    ASSERT_EQ(out.error, HandshakeError::None);
    EXPECT_EQ(out.hello.playerName, std::string(31, 'a'));
}

TEST(Handshake, Utf8NameWithinLimitIsKept) {
    Hello h = MakeHello();
    h.playerName = "Zelda \xC3\xA9t\xC3\xA9";
    EXPECT_EQ(DecodeHello(EncodeHello(h)).hello.playerName, h.playerName);
}

TEST(Compatibility, IdenticalHellosAreCompatible) {
    EXPECT_EQ(CheckCompatibility(MakeHello(), MakeHello()), HandshakeError::None);
}

TEST(Compatibility, PlayerNameDoesNotMatter) {
    Hello other = MakeHello();
    other.playerName = "Navi";
    EXPECT_EQ(CheckCompatibility(MakeHello(), other), HandshakeError::None);
}

TEST(Compatibility, EachMismatchIsReported) {
    Hello build = MakeHello();
    build.buildHash ^= 1;
    EXPECT_EQ(CheckCompatibility(MakeHello(), build), HandshakeError::BuildMismatch);

    Hello seed = MakeHello();
    seed.seedHash ^= 1;
    EXPECT_EQ(CheckCompatibility(MakeHello(), seed), HandshakeError::SeedMismatch);

    Hello proto = MakeHello();
    proto.protocolVersion = kProtocolVersion + 1;
    EXPECT_EQ(CheckCompatibility(MakeHello(), proto), HandshakeError::ProtocolMismatch);
}

// When several fields differ, the most fundamental one is reported:
// a seed mismatch is meaningless between two different builds.
TEST(Compatibility, PriorityIsProtocolThenBuildThenSeed) {
    Hello all = MakeHello();
    all.protocolVersion = kProtocolVersion + 1;
    all.buildHash ^= 1;
    all.seedHash ^= 1;
    EXPECT_EQ(CheckCompatibility(MakeHello(), all), HandshakeError::ProtocolMismatch);

    Hello buildAndSeed = MakeHello();
    buildAndSeed.buildHash ^= 1;
    buildAndSeed.seedHash ^= 1;
    EXPECT_EQ(CheckCompatibility(MakeHello(), buildAndSeed), HandshakeError::BuildMismatch);
}

TEST(Compatibility, EveryErrorHasADescription) {
    for (int e = 0; e <= static_cast<int>(HandshakeError::SeedMismatch); e++) {
        EXPECT_STRNE(Describe(static_cast<HandshakeError>(e)), "Unknown error");
    }
}

// Reference values from the FNV specification (http://www.isthe.com/chongo/tech/comp/fnv/).
TEST(BuildHash, MatchesFnv1aReferenceVectors) {
    EXPECT_EQ(BuildHash(""), 0xCBF29CE484222325ULL);
    EXPECT_EQ(BuildHash("a"), 0xAF63DC4C8601EC8CULL);
    EXPECT_EQ(BuildHash("foobar"), 0x85944171F73967E8ULL);
}

TEST(BuildHash, DifferentBuildsGiveDifferentHashes) {
    EXPECT_NE(BuildHash("soht:abc123/soh:f0599f9"), BuildHash("soht:abc124/soh:f0599f9"));
    EXPECT_NE(BuildHash("soht:abc123/soh:f0599f9"), BuildHash("soht:abc123-dirty/soh:f0599f9"));
}
