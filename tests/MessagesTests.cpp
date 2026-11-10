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

TEST(Messages, EntranceIndexRoundTripIncludingUnknown) {
    for (int32_t entrance : { -1, 0, 0x0EE, 0x613 }) {
        PlayerState s = MakeState();
        s.entranceIndex = entrance;
        std::optional<PlayerState> out = DecodePlayerState(EncodePlayerState(s));
        ASSERT_TRUE(out.has_value());
        EXPECT_EQ(out->entranceIndex, entrance);
    }
}

namespace {
PlayerPose MakePose() {
    PlayerPose p;
    for (size_t i = 0; i < kPoseJointCount; i++) {
        // negative values on purpose: they must survive the trip
        p.joints[i] = { static_cast<int16_t>(i * 100 - 1200), static_cast<int16_t>(-static_cast<int>(i)),
                        static_cast<int16_t>(0x7FFF - i) };
    }
    p.upperLimbRot = { -1, 2, -3 };
    p.prevTransl = { 400, -500, 600 };
    p.movementFlags = 0x05;
    p.currentBoots = 1;
    p.currentShield = 3;
    p.currentTunic = 2;
    p.stateFlags1 = 0x80000001;
    p.stateFlags2 = 0x00400000;
    p.buttonItem0 = 0x3D;
    p.itemAction = -1;
    p.heldItemAction = 5;
    p.modelGroup = 7;
    p.invincibilityTimer = -20;
    p.unk_862 = -300;
    p.unk_85C = 0.75f;
    p.actionVar1 = -2;
    return p;
}
} // namespace

TEST(Messages, PlayerPoseRoundTripKeepsEveryField) {
    PlayerPose in = MakePose();
    std::optional<PlayerPose> out = DecodePlayerPose(EncodePlayerPose(in));
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(*out == in);
    EXPECT_EQ(PeekType(EncodePlayerPose(in)), MessageType::PlayerPose);
}

TEST(Messages, PlayerPoseEveryTruncationIsRejected) {
    std::vector<uint8_t> full = EncodePlayerPose(MakePose());
    for (size_t len = 0; len < full.size(); len++) {
        EXPECT_FALSE(DecodePlayerPose(std::span<const uint8_t>(full.data(), len)).has_value()) << "cut at " << len;
    }
    full.push_back(0);
    EXPECT_FALSE(DecodePlayerPose(full).has_value());
}

TEST(Messages, PlayerPoseWithBrokenFloatIsRejected) {
    PlayerPose p = MakePose();
    p.unk_85C = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(DecodePlayerPose(EncodePlayerPose(p)).has_value());
}

TEST(Messages, PoseAndStateAreNotMixedUp) {
    EXPECT_FALSE(DecodePlayerPose(EncodePlayerState(MakeState())).has_value());
    EXPECT_FALSE(DecodePlayerState(EncodePlayerPose(MakePose())).has_value());
}

// Keeps an eye on bandwidth: it's sent 20 times a second to every player.
TEST(Messages, PlayerPoseStaysSmall) {
    EXPECT_LE(EncodePlayerPose(MakePose()).size(), 200u);
}
