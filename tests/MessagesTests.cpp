#include "soh/Network/Coop/Messages.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>

using namespace Coop;

namespace {

PlayerState MakeState() {
    PlayerState s;
    s.frame = 123456;
    s.inGame = true;
    s.sceneNum = 0x55; // Kokiri Forest
    s.roomNum = -1;
    s.linkAge = 1;
    s.x = -123.5f;
    s.y = 42.25f;
    s.z = 9876.0f;
    s.yaw = -0x4000;
    return s;
}

// Overwrites the float at the given byte offset in an encoded message.
void PatchFloat(std::vector<uint8_t>& bytes, size_t offset, float v) {
    std::memcpy(bytes.data() + offset, &v, sizeof(v));
}

// type(1) frame(4) inGame(1) scene(2) room(2) age(1) -> x at 11, y at 15, z at 19
constexpr size_t kOffsetX = 11;
constexpr size_t kOffsetZ = 19;

} // namespace

TEST(Messages, PlayerStateRoundTripKeepsSignsAndFractions) {
    PlayerState in = MakeState();
    std::optional<PlayerState> out = DecodePlayerState(EncodePlayerState(in));

    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->frame, in.frame);
    EXPECT_TRUE(out->inGame);
    EXPECT_EQ(out->sceneNum, in.sceneNum);
    EXPECT_EQ(out->roomNum, -1);
    EXPECT_EQ(out->linkAge, 1);
    EXPECT_EQ(out->x, in.x);
    EXPECT_EQ(out->y, in.y);
    EXPECT_EQ(out->z, in.z);
    EXPECT_EQ(out->yaw, -0x4000);
}

TEST(Messages, PeekTypeReadsTheFirstByte) {
    EXPECT_EQ(PeekType(EncodePlayerState(MakeState())), MessageType::PlayerState);
    EXPECT_FALSE(PeekType({}).has_value());
}

TEST(Messages, EveryTruncationIsRejected) {
    std::vector<uint8_t> full = EncodePlayerState(MakeState());
    for (size_t len = 0; len < full.size(); len++) {
        EXPECT_FALSE(DecodePlayerState(std::span<const uint8_t>(full.data(), len)).has_value()) << "cut at " << len;
    }
}

TEST(Messages, TrailingBytesAndWrongTypeAreRejected) {
    std::vector<uint8_t> bytes = EncodePlayerState(MakeState());
    bytes.push_back(0);
    EXPECT_FALSE(DecodePlayerState(bytes).has_value());

    std::vector<uint8_t> wrongType = EncodePlayerState(MakeState());
    wrongType[0] = 0xEE;
    EXPECT_FALSE(DecodePlayerState(wrongType).has_value());
}

TEST(Messages, InGameFlagMustBeZeroOrOne) {
    std::vector<uint8_t> bytes = EncodePlayerState(MakeState());
    bytes[5] = 2;
    EXPECT_FALSE(DecodePlayerState(bytes).has_value());
}

// A NaN position would propagate into the game's math and drawing code.
TEST(Messages, NonFiniteOrAbsurdPositionsAreRejected) {
    for (float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(), kMaxCoordinate * 2 }) {
        std::vector<uint8_t> bytes = EncodePlayerState(MakeState());
        PatchFloat(bytes, kOffsetX, bad);
        EXPECT_FALSE(DecodePlayerState(bytes).has_value()) << bad;

        bytes = EncodePlayerState(MakeState());
        PatchFloat(bytes, kOffsetZ, bad);
        EXPECT_FALSE(DecodePlayerState(bytes).has_value()) << bad;
    }
}

TEST(Messages, CoordinateLimitIsInclusive) {
    PlayerState s = MakeState();
    s.x = kMaxCoordinate;
    s.z = -kMaxCoordinate;
    EXPECT_TRUE(DecodePlayerState(EncodePlayerState(s)).has_value());
}
