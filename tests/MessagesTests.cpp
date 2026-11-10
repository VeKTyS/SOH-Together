#include "soh/Network/Coop/Messages.h"

#include <gtest/gtest.h>

#include <cmath>
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

TEST(Messages, HealthRoundTrip) {
    PlayerState s = MakeState();
    s.health = 0x30;
    s.healthCapacity = 0x140;
    std::optional<PlayerState> out = DecodePlayerState(EncodePlayerState(s));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->health, 0x30);
    EXPECT_EQ(out->healthCapacity, 0x140);
}

TEST(Messages, TimeOfDayRoundTrip) {
    PlayerState s = MakeState();
    s.dayTime = 0xC123;
    s.night = true;
    std::optional<PlayerState> out = DecodePlayerState(EncodePlayerState(s));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->dayTime, 0xC123);
    EXPECT_TRUE(out->night);
    std::vector<uint8_t> bad = EncodePlayerState(s);
    bad[bad.size() - 9] = 2; // night is 0 or 1 (followed by color r,g,b, team and 2 health u16)
    EXPECT_FALSE(DecodePlayerState(bad).has_value());
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
    EXPECT_LE(EncodePlayerPose(MakePose()).size(), 220u);
}

TEST(Messages, FlagChangeRoundTrip) {
    for (FlagChange c : { FlagChange{ -1, 5, 0x1A7, true }, FlagChange{ 0x55, 1, 0x3F, false } }) {
        std::optional<FlagChange> out = DecodeFlagChange(EncodeFlagChange(c));
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(*out == c);
    }
    std::vector<uint8_t> bad = EncodeFlagChange({});
    bad.back() = 2; // "set" must be 0 or 1
    EXPECT_FALSE(DecodeFlagChange(bad).has_value());
}

namespace {
FlagSnapshot MakeSnapshot() {
    FlagSnapshot s;
    s.scenes.resize(124);
    for (size_t i = 0; i < s.scenes.size(); i++) {
        s.scenes[i] = { static_cast<uint32_t>(i), 0xFFFFFFFF, 0, 0x80000000, 7, 8, static_cast<uint32_t>(i * 3) };
    }
    s.eventChkInf = std::vector<uint16_t>(14, 0xABCD);
    s.itemGetInf = { 1, 2, 3, 4 };
    s.infTable = std::vector<uint16_t>(30, 0x0F0F);
    s.randomizerInf = std::vector<uint16_t>(150, 0x1234);
    return s;
}
} // namespace

TEST(Messages, FlagSnapshotRoundTripAndSize) {
    FlagSnapshot in = MakeSnapshot();
    std::vector<uint8_t> bytes = EncodeFlagSnapshot(in);
    std::optional<FlagSnapshot> out = DecodeFlagSnapshot(bytes);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(*out == in);
    EXPECT_LT(bytes.size(), 8000u); // a few KB, once per save load: fine
}

TEST(Messages, FlagSnapshotEveryTruncationIsRejected) {
    std::vector<uint8_t> full = EncodeFlagSnapshot(MakeSnapshot());
    for (size_t len = 0; len < full.size(); len += 7) { // every 7th cut, it's a big message
        EXPECT_FALSE(DecodeFlagSnapshot(std::span<const uint8_t>(full.data(), len)).has_value()) << len;
    }
}

// A 5-byte packet claiming 65535 scenes must not make us allocate megabytes.
TEST(Messages, FlagSnapshotWithLyingCountsIsRejected) {
    std::vector<uint8_t> liar = { static_cast<uint8_t>(MessageType::FlagSnapshot), 0xFF, 0xFF, 0, 0 };
    EXPECT_FALSE(DecodeFlagSnapshot(liar).has_value());

    FlagSnapshot s;
    std::vector<uint8_t> bytes = EncodeFlagSnapshot(s); // 0 scenes, then 4 empty arrays
    bytes[3] = 0xFF;                                    // eventChkInf count = 255, no data behind
    EXPECT_FALSE(DecodeFlagSnapshot(bytes).has_value());
}

TEST(Messages, EnemyKillRoundTrip) {
    // negative values on purpose: room -1, params with the top bit set
    for (EnemyKill k : { EnemyKill{ 0x00, 0, 2, 7, 0x0013, 0x0001 }, EnemyKill{ 0x51, 3, -1, 300, 0x0197, -1 } }) {
        std::optional<EnemyKill> out = DecodeEnemyKill(EncodeEnemyKill(k));
        ASSERT_TRUE(out.has_value());
        EXPECT_TRUE(*out == k);
    }
}

TEST(Messages, EnemyKillTruncatedOrPaddedIsRejected) {
    std::vector<uint8_t> full = EncodeEnemyKill({ 1, 0, 2, 3, 4, 5 });
    for (size_t len = 0; len < full.size(); len++) {
        EXPECT_FALSE(DecodeEnemyKill(std::span<const uint8_t>(full.data(), len)).has_value()) << len;
    }
    full.push_back(0);
    EXPECT_FALSE(DecodeEnemyKill(full).has_value());
    EXPECT_FALSE(DecodeEnemyKill(EncodeFlagChange({})).has_value()); // wrong type
}

namespace {
EnemyStates MakeEnemyStates() {
    EnemyStates s;
    s.sceneNum = 0x0D;
    s.layer = 1;
    EnemyState a;
    a.room = 3;
    a.listIndex = 12;
    a.actorId = 0x0002; // Stalfos
    a.pos = { 100.5f, -20.0f, 3000.25f };
    a.focus = { 100.5f, 30.0f, 3000.25f };
    a.scale = { 0.01f, 0.01f, 0.01f };
    a.shapeRot = { 0, -16384, 0 };
    a.worldRot = { 0, -16000, 0 };
    a.health = 8;
    a.submitted = 1 | 2 | 4;
    for (int i = 0; i < 8; i++) {
        a.quadCorners.push_back({ float(i), float(i * 2), float(-i) });
    }
    for (int i = 0; i < 21; i++) {
        a.joints.push_back({ int16_t(i), int16_t(-i * 100), int16_t(i * 300) });
    }
    for (int i = 0; i < 20; i++) {
        a.body.push_back(static_cast<uint8_t>(i * 13));
    }
    a.slotKinds = { 0, 1, 2 }; // 20 bytes = 3 slots
    EnemyState b;              // no skeleton, no quads
    b.room = -1;
    b.listIndex = 0;
    b.actorId = 0x0013;
    s.enemies = { a, b };
    return s;
}
} // namespace

TEST(Messages, EnemyStatesRoundTrip) {
    EnemyStates in = MakeEnemyStates();
    std::optional<EnemyStates> out = DecodeEnemyStates(EncodeEnemyStates(in));
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(*out == in);
}

TEST(Messages, EnemyStatesEveryTruncationIsRejected) {
    std::vector<uint8_t> full = EncodeEnemyStates(MakeEnemyStates());
    for (size_t len = 0; len < full.size(); len++) {
        EXPECT_FALSE(DecodeEnemyStates(std::span<const uint8_t>(full.data(), len)).has_value()) << len;
    }
}

TEST(Messages, EnemyStatesWithBadNumbersIsRejected) {
    EnemyStates s = MakeEnemyStates();
    s.enemies[0].pos.y = std::nanf("");
    EXPECT_FALSE(DecodeEnemyStates(EncodeEnemyStates(s)).has_value());
    s = MakeEnemyStates();
    s.enemies[0].quadCorners[5].x = 1e9f;
    EXPECT_FALSE(DecodeEnemyStates(EncodeEnemyStates(s)).has_value());
}

// counts that lie about what follows must not make us allocate anything big
TEST(Messages, EnemyStatesWithLyingCountsIsRejected) {
    std::vector<uint8_t> tooMany = { static_cast<uint8_t>(MessageType::EnemyStates), 0, 0, 0, 200 };
    EXPECT_FALSE(DecodeEnemyStates(tooMany).has_value());

    EnemyStates one;
    one.enemies.resize(1);
    std::vector<uint8_t> bytes = EncodeEnemyStates(one);
    // the entry ends with: quad count (1 byte), joint count (1), body size (2), all 0 here
    size_t n = bytes.size();
    bytes[n - 3] = 120; // joint count says 120, nothing behind it
    EXPECT_FALSE(DecodeEnemyStates(bytes).has_value());
    bytes[n - 3] = 0;
    bytes[n - 4] = 9; // 9 quads, over the limit
    EXPECT_FALSE(DecodeEnemyStates(bytes).has_value());
    bytes[n - 4] = 0;
    bytes[n - 1] = 0x30; // body of 12 KB, over the limit
    EXPECT_FALSE(DecodeEnemyStates(bytes).has_value());
    bytes[n - 1] = 0;
    bytes[n - 2] = 40; // 40 body bytes announced, none there
    EXPECT_FALSE(DecodeEnemyStates(bytes).has_value());
}

TEST(Messages, EnemyStatesStaysUnderOnePacketForARoom) {
    // a busy room: 10 enemies with a 21-limb skeleton
    EnemyStates s = MakeEnemyStates();
    s.enemies.resize(1);
    s.enemies[0].quadCorners.clear();
    s.enemies[0].body.clear(); // the copied memory is extra, this checks the rest
    s.enemies[0].slotKinds.clear();
    s.enemies.resize(10, s.enemies[0]);
    EXPECT_LT(EncodeEnemyStates(s).size(), 2000u);
}

TEST(Messages, EnemyHitRoundTrip) {
    EnemyHit in{ 0x00, 2, 7, 0x0013, 4, 3, 0x00000200, { 10, -20, 30 } };
    std::optional<EnemyHit> out = DecodeEnemyHit(EncodeEnemyHit(in));
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(*out == in);
    std::vector<uint8_t> bytes = EncodeEnemyHit(in);
    bytes.pop_back();
    EXPECT_FALSE(DecodeEnemyHit(bytes).has_value());
}

TEST(Messages, ItemsRoundTrip) {
    ItemGiven item{ 1, -5 };
    std::optional<ItemGiven> outItem = DecodeItemGiven(EncodeItemGiven(item));
    ASSERT_TRUE(outItem.has_value());
    EXPECT_TRUE(*outItem == item);

    DungeonItems d{ 3, 0x07, -1 };
    std::optional<DungeonItems> outD = DecodeDungeonItems(EncodeDungeonItems(d));
    ASSERT_TRUE(outD.has_value());
    EXPECT_TRUE(*outD == d);

    std::vector<uint8_t> bad = EncodeItemGiven(item);
    bad.push_back(0);
    EXPECT_FALSE(DecodeItemGiven(bad).has_value());
    EXPECT_FALSE(DecodeDungeonItems(EncodeItemGiven(item)).has_value());
}

TEST(Messages, FlagSnapshotCarriesTheSaveBlob) {
    FlagSnapshot in = MakeSnapshot();
    for (int i = 0; i < 120; i++) {
        in.saveBlob.push_back(static_cast<uint8_t>(i * 7));
    }
    std::optional<FlagSnapshot> out = DecodeFlagSnapshot(EncodeFlagSnapshot(in));
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(*out == in);

    std::vector<uint8_t> bytes = EncodeFlagSnapshot(in);
    bytes.pop_back(); // blob shorter than its size says
    EXPECT_FALSE(DecodeFlagSnapshot(bytes).has_value());
}

TEST(Messages, ThingStatesRoundTripAndLimits) {
    ThingStates in;
    in.owner = 0xDEADBEEF;
    in.sceneNum = 0x55;
    in.layer = 2;
    ThingState pot;
    pot.localId = 3;
    pot.room = 1;
    pot.listIndex = 14;
    pot.actorId = 0x111;
    pot.pos = { 10.5f, 40.0f, -200.0f };
    pot.rot = { 0, 1234, 0 };
    ThingState bomb; // only exists for its owner
    bomb.localId = 4;
    bomb.listIndex = -1;
    bomb.actorId = 0x10;
    bomb.body = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    bomb.slotKinds = { 0, 0 };
    in.things = { pot, bomb };
    std::optional<ThingStates> out = DecodeThingStates(EncodeThingStates(in));
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(*out == in);

    std::vector<uint8_t> full = EncodeThingStates(in);
    for (size_t len = 0; len < full.size(); len++) {
        EXPECT_FALSE(DecodeThingStates(std::span<const uint8_t>(full.data(), len)).has_value()) << len;
    }
    std::vector<uint8_t> tooMany = { static_cast<uint8_t>(MessageType::ThingStates), 0, 0, 0, 0, 0, 0, 0, 99 };
    EXPECT_FALSE(DecodeThingStates(tooMany).has_value());
}

TEST(Messages, WelcomeForwardedPeerLeftEventRoundTrip) {
    Welcome w{ 7 };
    EXPECT_TRUE(DecodeWelcome(EncodeWelcome(w)) == w);
    EXPECT_FALSE(DecodeWelcome(EncodeWelcome(Welcome{ 0 })).has_value()); // 0 = no id

    Forwarded f;
    f.origin = 3;
    f.name = "Malon";
    f.inner = EncodePlayerState(PlayerState{});
    std::optional<Forwarded> fo = DecodeForwarded(EncodeForwarded(f));
    ASSERT_TRUE(fo.has_value());
    EXPECT_TRUE(*fo == f);

    PeerLeft p{ 4 };
    EXPECT_TRUE(DecodePeerLeft(EncodePeerLeft(p)) == p);

    Event e;
    e.origin = 2;
    e.kind = EventKind::OcarinaNote;
    e.data = { 1, 2, 3 };
    std::vector<uint8_t> bytes = EncodeEvent(e);
    EXPECT_TRUE(DecodeEvent(bytes) == e);
    for (size_t len = 0; len < bytes.size(); len++) {
        EXPECT_FALSE(DecodeEvent(std::span<const uint8_t>(bytes.data(), len)).has_value()) << len;
    }
    bytes.push_back(0); // trailing byte
    EXPECT_FALSE(DecodeEvent(bytes).has_value());
}

TEST(Messages, PlayerStateCarriesColorAndTeam) {
    PlayerState s;
    s.colorR = 1;
    s.colorG = 2;
    s.colorB = 3;
    s.team = 4;
    std::optional<PlayerState> out = DecodePlayerState(EncodePlayerState(s));
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->colorR, 1);
    EXPECT_EQ(out->colorB, 3);
    EXPECT_EQ(out->team, 4);
}

TEST(Messages, ObjectStatesRoundTripAndLimits) {
    ObjectStates in;
    in.sceneNum = 0x52;
    in.layer = 1;
    ObjectState platform; // moving: position only this frame
    platform.room = 3;
    platform.listIndex = 7;
    platform.actorId = 0x1A3;
    platform.pos = { -120.0f, 300.5f, 42.0f };
    platform.shapeRot = { 0, -16384, 5 };
    platform.worldRot = { 1, 2, 3 };
    platform.scale = { 0.1f, 0.1f, 0.1f };
    ObjectState block = platform; // with its memory this time
    block.listIndex = 8;
    block.body = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
    block.slotKinds = { 0, 1 };
    in.objects = { platform, block };
    std::vector<uint8_t> full = EncodeObjectStates(in);
    std::optional<ObjectStates> out = DecodeObjectStates(full);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(*out == in);

    for (size_t len = 0; len < full.size(); len++) {
        EXPECT_FALSE(DecodeObjectStates(std::span<const uint8_t>(full.data(), len)).has_value()) << len;
    }
    std::vector<uint8_t> trailing = full;
    trailing.push_back(0);
    EXPECT_FALSE(DecodeObjectStates(trailing).has_value());
    std::vector<uint8_t> tooMany = { static_cast<uint8_t>(MessageType::ObjectStates), 0, 0, 0, 200 };
    EXPECT_FALSE(DecodeObjectStates(tooMany).has_value());

    ObjectStates insane = in; // a broken scale must not reach the game
    insane.objects[0].scale.y = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(DecodeObjectStates(EncodeObjectStates(insane)).has_value());
}
