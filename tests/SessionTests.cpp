// Two Sessions talking over loopback UDP, as the game will use them.

#include "soh/Network/Coop/Session.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace Coop;
using namespace std::chrono_literals;

namespace {

Hello MakeHello(const char* name, uint64_t build = 7) {
    Hello h;
    h.buildHash = build;
    h.playerName = name;
    return h;
}

PlayerState StateAt(float x, int16_t scene = 0x55) {
    PlayerState s;
    s.inGame = true;
    s.sceneNum = scene;
    s.x = x;
    return s;
}

// Ticks both sessions (like two games running their frames) until done().
bool TickUntil(Session& a, const PlayerState& sa, Session& b, const PlayerState& sb, const std::function<bool()>& done,
               std::chrono::milliseconds deadline = 3s) {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        a.Tick(sa);
        b.Tick(sb);
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

} // namespace

TEST(Session, HostAndClientSeeEachOtherNamesAndPositions) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Zelda")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Link")));
    EXPECT_EQ(client.Role(), SessionRole::Connecting);

    ASSERT_TRUE(TickUntil(host, StateAt(100), client, StateAt(-200), [&] {
        return host.Players().size() == 1 && client.Players().size() == 1 && host.Players()[0].state &&
               client.Players()[0].state;
    }));

    EXPECT_EQ(host.Role(), SessionRole::Hosting);
    EXPECT_EQ(client.Role(), SessionRole::Connected);
    EXPECT_EQ(host.Players()[0].name, "Link");
    EXPECT_EQ(client.Players()[0].name, "Zelda");
    EXPECT_EQ(host.Players()[0].state->x, -200);
    EXPECT_EQ(client.Players()[0].state->x, 100);
}

TEST(Session, LatestStateWins) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Zelda")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Link")));
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(1),
                          [&] { return !host.Players().empty() && host.Players()[0].state; }));

    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(555), [&] { return host.Players()[0].state->x == 555; }));
}

TEST(Session, ClientStopIsSeenByHost) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Zelda")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Link")));
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] { return host.Players().size() == 1; }));

    client.Stop();
    EXPECT_EQ(client.Role(), SessionRole::Off);
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] { return host.Players().empty(); }));
    EXPECT_EQ(host.Role(), SessionRole::Hosting); // the host keeps hosting
}

TEST(Session, HostStopEndsTheClientSessionWithAReason) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Zelda")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Link")));
    ASSERT_TRUE(
        TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Connected; }));

    host.Stop();
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Off; }));
    EXPECT_FALSE(client.LastError().empty());
    EXPECT_TRUE(client.Players().empty());
}

TEST(Session, DifferentBuildIsRefusedWithAReadableReason) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Zelda", 1)));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Link", 2)));

    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Off; }));
    EXPECT_NE(client.LastError().find("same soh.exe"), std::string::npos) << client.LastError();
    EXPECT_TRUE(host.Players().empty());
}

TEST(Session, HostingOnABusyPortFailsCleanly) {
    Session first, second;
    ASSERT_TRUE(first.Host(0, MakeHello("Zelda")));
    EXPECT_FALSE(second.Host(first.Port(), MakeHello("Ganon")));
    EXPECT_EQ(second.Role(), SessionRole::Off);
    EXPECT_NE(second.LastError().find("port"), std::string::npos);
}

TEST(Session, LoggerReceivesLifecycleLines) {
    std::vector<std::string> lines;
    {
        Session host([&](const std::string& l) { lines.push_back(l); });
        ASSERT_TRUE(host.Host(0, MakeHello("Zelda")));
    }
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines[0].find("Hosting"), std::string::npos);
}

TEST(ParseAddress, HostAloneUsesTheDefaultPort) {
    std::string host;
    uint16_t port = 0;
    ASSERT_TRUE(ParseAddress("192.168.1.20", host, port));
    EXPECT_EQ(host, "192.168.1.20");
    EXPECT_EQ(port, kDefaultPort);
}

TEST(ParseAddress, ExplicitPortAndSpacesFromCopyPaste) {
    std::string host;
    uint16_t port = 0;
    ASSERT_TRUE(ParseAddress("  100.64.0.7:30000 \t", host, port));
    EXPECT_EQ(host, "100.64.0.7");
    EXPECT_EQ(port, 30000);
}

TEST(ParseAddress, InvalidInputsAreRejected) {
    std::string host;
    uint16_t port = 0;
    for (const char* bad : { "", "   ", ":27720", "1.2.3.4:", "1.2.3.4:0", "1.2.3.4:65536", "1.2.3.4:99999999",
                             "1.2.3.4:12a", "my host:1234", "::1" }) {
        EXPECT_FALSE(ParseAddress(bad, host, port)) << "'" << bad << "'";
    }
}

namespace {
PlayerState Somewhere(int32_t entrance = 0x0EE, int16_t room = 2) {
    PlayerState s;
    s.inGame = true;
    s.entranceIndex = entrance;
    s.roomNum = room;
    return s;
}
constexpr int32_t kEntrances = 0x614; // ENTR_MAX in SoH 9.3.1
} // namespace

TEST(Teleport, AllowedWhenBothInGameAndTheirLocationIsValid) {
    EXPECT_EQ(TeleportBlocker(Somewhere(), Somewhere(), kEntrances), nullptr);
    EXPECT_EQ(TeleportBlocker(Somewhere(), Somewhere(kEntrances - 1, 0), kEntrances), nullptr);
}

TEST(Teleport, BlockedWithAReasonOtherwise) {
    PlayerState notInGame;
    EXPECT_NE(TeleportBlocker(notInGame, Somewhere(), kEntrances), nullptr);
    EXPECT_NE(TeleportBlocker(Somewhere(), notInGame, kEntrances), nullptr);
    // values coming from the network that would make the game load garbage
    EXPECT_NE(TeleportBlocker(Somewhere(), Somewhere(-1), kEntrances), nullptr);
    EXPECT_NE(TeleportBlocker(Somewhere(), Somewhere(kEntrances), kEntrances), nullptr);
    EXPECT_NE(TeleportBlocker(Somewhere(), Somewhere(0x0EE, -1), kEntrances), nullptr);
}

TEST(Session, PoseIsReceivedAndDroppedWhenTheyLeaveTheGame) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Zelda")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Link")));

    PlayerPose pose;
    pose.modelGroup = 9;
    PlayerState inGame = StateAt(10);
    PlayerState onTitle;
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, inGame, [&] {
        client.Tick(inGame, &pose);
        return !host.Players().empty() && host.Players()[0].pose && host.Players()[0].pose->modelGroup == 9;
    }));

    ASSERT_TRUE(TickUntil(host, StateAt(0), client, onTitle, [&] { return !host.Players()[0].pose.has_value(); }));
}

namespace {
// Ticks three sessions until done().
bool TickAll(Session& a, Session& b, Session& c, const std::function<bool()>& done) {
    auto end = std::chrono::steady_clock::now() + 3s;
    PlayerState s = StateAt(0);
    while (std::chrono::steady_clock::now() < end) {
        a.Tick(s);
        b.Tick(s);
        c.Tick(s);
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
} // namespace

TEST(SessionFlags, ClientChangeReachesHostAndIsRelayedToOtherClients) {
    Session host, alice, bob;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(alice.Join("127.0.0.1", host.Port(), MakeHello("Alice")));
    ASSERT_TRUE(bob.Join("127.0.0.1", host.Port(), MakeHello("Bob")));
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        return host.Players().size() == 2 && alice.Role() == SessionRole::Connected &&
               bob.Role() == SessionRole::Connected;
    }));

    FlagChange opened{ 0x00, 2, 0x05, true }; // a chest in the Deku Tree
    alice.SendFlagChange(opened);

    std::vector<FlagChange> atHost, atBob, atAlice;
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        for (auto& c : host.TakeFlagChanges())
            atHost.push_back(c);
        for (auto& c : bob.TakeFlagChanges())
            atBob.push_back(c);
        for (auto& c : alice.TakeFlagChanges())
            atAlice.push_back(c);
        return !atHost.empty() && !atBob.empty();
    }));
    EXPECT_TRUE(atHost[0] == opened);
    EXPECT_TRUE(atBob[0] == opened);
    // a few more frames: the sender must not get its own change back
    TickAll(host, alice, bob, [&] {
        for (auto& c : alice.TakeFlagChanges())
            atAlice.push_back(c);
        return false;
    });
    EXPECT_TRUE(atAlice.empty());
}

TEST(SessionFlags, SnapshotRequestAndAnswer) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Guest")));
    ASSERT_TRUE(
        TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Connected; }));

    client.RequestFlagSnapshot();
    std::vector<PeerId> requests;
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] {
        requests = host.TakeSnapshotRequests();
        return !requests.empty();
    }));

    FlagSnapshot snap;
    snap.scenes.resize(2);
    snap.scenes[1][1] = 0x10; // one switch flag in scene 1
    snap.eventChkInf = { 0xBEEF };
    host.SendFlagSnapshot(requests[0], snap);

    std::optional<FlagSnapshot> got;
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] {
        got = client.TakeFlagSnapshot();
        return got.has_value();
    }));
    EXPECT_TRUE(*got == snap);
}

TEST(SessionFlags, OnlyClientsAskAndOnlyTheHostAnswers) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Guest")));
    ASSERT_TRUE(
        TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Connected; }));

    host.RequestFlagSnapshot();                 // host asking: ignored
    client.SendFlagSnapshot(1, FlagSnapshot{}); // client answering: ignored
    TickUntil(
        host, StateAt(0), client, StateAt(0), [] { return false; }, 300ms);
    EXPECT_TRUE(client.TakeSnapshotRequests().empty());
    EXPECT_FALSE(host.TakeFlagSnapshot().has_value());
}

TEST(SessionEnemies, KillReachesHostAndIsRelayedButNotEchoed) {
    Session host, alice, bob;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(alice.Join("127.0.0.1", host.Port(), MakeHello("Alice")));
    ASSERT_TRUE(bob.Join("127.0.0.1", host.Port(), MakeHello("Bob")));
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        return host.Players().size() == 2 && alice.Role() == SessionRole::Connected &&
               bob.Role() == SessionRole::Connected;
    }));

    EnemyKill keese{ 0x00, 0, 1, 4, 0x0013, 0x0002 };
    bob.SendEnemyKill(keese);

    std::vector<EnemyKill> atHost, atAlice, atBob;
    auto collect = [&] {
        for (auto& k : host.TakeEnemyKills())
            atHost.push_back(k);
        for (auto& k : alice.TakeEnemyKills())
            atAlice.push_back(k);
        for (auto& k : bob.TakeEnemyKills())
            atBob.push_back(k);
    };
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        collect();
        return !atHost.empty() && !atAlice.empty();
    }));
    EXPECT_TRUE(atHost[0] == keese);
    EXPECT_TRUE(atAlice[0] == keese);
    TickAll(host, alice, bob, [&] {
        collect();
        return false;
    });
    EXPECT_TRUE(atBob.empty());
    EXPECT_EQ(atHost.size(), 1u);
    EXPECT_EQ(atAlice.size(), 1u);
}

TEST(SessionEnemies, HostKillReachesEveryClient) {
    Session host, alice, bob;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(alice.Join("127.0.0.1", host.Port(), MakeHello("Alice")));
    ASSERT_TRUE(bob.Join("127.0.0.1", host.Port(), MakeHello("Bob")));
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        return host.Players().size() == 2 && alice.Role() == SessionRole::Connected &&
               bob.Role() == SessionRole::Connected;
    }));
    host.SendEnemyKill({ 0x01, 0, 0, 9, 0x0025, 0 });
    size_t gotAlice = 0, gotBob = 0;
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        gotAlice += alice.TakeEnemyKills().size();
        gotBob += bob.TakeEnemyKills().size();
        return gotAlice == 1 && gotBob == 1;
    }));
}

TEST(SessionEnemies, HostStatesReachClientAndClientHitsReachHost) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Guest")));
    ASSERT_TRUE(
        TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Connected; }));

    EnemyStates states;
    states.sceneNum = 5;
    states.enemies.resize(2);
    states.enemies[1].listIndex = 4;
    states.enemies[1].health = 3;
    std::optional<EnemyStates> got;
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] {
        host.SendEnemyStates(states); // sent every frame like in the game
        if (auto s = client.TakeEnemyStates()) {
            got = s;
        }
        return got.has_value();
    }));
    EXPECT_TRUE(*got == states);

    EnemyHit hit{ 5, 0, 4, 0, 2, 1, 0x1, { 1, 2, 3 } };
    client.SendEnemyHit(hit);
    std::vector<EnemyHit> hits;
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] {
        for (auto& h : host.TakeEnemyHits())
            hits.push_back(h);
        return !hits.empty();
    }));
    EXPECT_TRUE(hits[0] == hit);
}

TEST(SessionEnemies, OnlyTheHostSendsStates) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Guest")));
    ASSERT_TRUE(
        TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Connected; }));
    EnemyStates states;
    states.enemies.resize(1);
    client.SendEnemyStates(states); // a client doesn't run enemies: ignored
    bool anything = false;
    TickUntil(
        host, StateAt(0), client, StateAt(0),
        [&] {
            anything = anything || host.TakeEnemyStates().has_value();
            return false;
        },
        300ms);
    EXPECT_FALSE(anything);
}

TEST(SessionEnemies, HostHitsReachClientsForSharedHealth) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Guest")));
    ASSERT_TRUE(
        TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Connected; }));
    host.SendEnemyHit({ 0x11, 0, 3, 0x28, 2, 0, 0, {} }); // a boss losing 2 health on the host
    std::vector<EnemyHit> got;
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0), [&] {
        for (auto& h : client.TakeEnemyHits())
            got.push_back(h);
        return !got.empty();
    }));
    EXPECT_EQ(got[0].damage, 2);
}

TEST(SessionItems, ItemsAndDungeonKeysAreRelayedToEveryoneElse) {
    Session host, alice, bob;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(alice.Join("127.0.0.1", host.Port(), MakeHello("Alice")));
    ASSERT_TRUE(bob.Join("127.0.0.1", host.Port(), MakeHello("Bob")));
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        return host.Players().size() == 2 && alice.Role() == SessionRole::Connected &&
               bob.Role() == SessionRole::Connected;
    }));
    alice.SendItemGiven({ 0, 0x04 }); // fairy slingshot
    alice.SendDungeonItems({ 0, 0x02, 1 });
    size_t hostItems = 0, bobItems = 0, aliceItems = 0, bobKeys = 0;
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        hostItems += host.TakeItemsGiven().size();
        bobItems += bob.TakeItemsGiven().size();
        aliceItems += alice.TakeItemsGiven().size();
        bobKeys += bob.TakeDungeonItems().size();
        return hostItems == 1 && bobItems == 1 && bobKeys == 1;
    }));
    EXPECT_EQ(aliceItems, 0u); // not echoed back
}

// a real room: a dozen enemies with skeletons is several KB, more than one UDP
// packet. It has to arrive anyway (ENet splits it).
TEST(SessionEnemies, BigEnemyPacketArrives) {
    Session host, client;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(client.Join("127.0.0.1", host.Port(), MakeHello("Guest")));
    ASSERT_TRUE(
        TickUntil(host, StateAt(0), client, StateAt(0), [&] { return client.Role() == SessionRole::Connected; }));
    EnemyStates states;
    states.enemies.resize(20);
    for (auto& e : states.enemies) {
        e.joints.resize(40);
        e.quadCorners.resize(8);
    }
    std::optional<EnemyStates> got;
    ASSERT_TRUE(TickUntil(host, StateAt(0), client, StateAt(0),
                          [&] {
                              host.SendEnemyStates(states);
                              if (auto s = client.TakeEnemyStates()) {
                                  got = s;
                              }
                              return got.has_value();
                          }))
        << "size " << EncodeEnemyStates(states).size();
    EXPECT_EQ(got->enemies.size(), 20u);
}

TEST(SessionThings, ClientThingsReachTheOtherClientLatestOnly) {
    Session host, alice, bob;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(alice.Join("127.0.0.1", host.Port(), MakeHello("Alice")));
    ASSERT_TRUE(bob.Join("127.0.0.1", host.Port(), MakeHello("Bob")));
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        return host.Players().size() == 2 && alice.Role() == SessionRole::Connected &&
               bob.Role() == SessionRole::Connected;
    }));
    ThingStates mine;
    mine.owner = 42;
    mine.things.resize(1);
    std::vector<ThingStates> atBob, atHost;
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        alice.SendThingStates(mine); // every frame, like the game
        for (auto& t : bob.TakeThingStates())
            atBob.push_back(t);
        for (auto& t : host.TakeThingStates())
            atHost.push_back(t);
        return !atBob.empty() && !atHost.empty();
    }));
    EXPECT_EQ(atBob.back().owner, 42u);
    EXPECT_EQ(atHost.back().things.size(), 1u);
}

TEST(SessionRelay, ClientsGetIdsAndSeeEachOther) {
    Session host, alice, bob;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(alice.Join("127.0.0.1", host.Port(), MakeHello("Alice")));
    ASSERT_TRUE(bob.Join("127.0.0.1", host.Port(), MakeHello("Bob")));
    // alice sees host + bob, bob sees host + alice, all with a state
    auto seesAll = [](const Session& s) {
        return s.Players().size() == 2 && s.Players()[0].state && s.Players()[1].state;
    };
    ASSERT_TRUE(TickAll(host, alice, bob, [&] { return seesAll(alice) && seesAll(bob); }));
    EXPECT_EQ(host.SelfId(), kHostPlayerId);
    EXPECT_NE(alice.SelfId(), 0u);
    EXPECT_NE(alice.SelfId(), bob.SelfId());
    // the host stays first (snapshot requests go to Players()[0])
    EXPECT_EQ(alice.Players()[0].id, kHostPlayerId);
    EXPECT_FALSE(alice.Players()[0].relayed);
    EXPECT_TRUE(alice.Players()[1].relayed);
    EXPECT_EQ(alice.Players()[1].name, "Bob");
    EXPECT_EQ(alice.Players()[1].id, bob.SelfId());

    // bob leaves: alice forgets him
    bob.Stop();
    ASSERT_TRUE(TickAll(host, alice, bob, [&] { return alice.Players().size() == 1; }));
    EXPECT_EQ(alice.Players()[0].id, kHostPlayerId);
}

TEST(SessionRelay, EventsReachEveryoneWithTheRealSender) {
    Session host, alice, bob;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(alice.Join("127.0.0.1", host.Port(), MakeHello("Alice")));
    ASSERT_TRUE(bob.Join("127.0.0.1", host.Port(), MakeHello("Bob")));
    ASSERT_TRUE(TickAll(host, alice, bob, [&] { return alice.SelfId() != 0 && bob.SelfId() != 0; }));

    alice.SendEvent(EventKind::PlayerSfx, { 0x12, 0x34 });
    std::vector<Event> atHost, atBob, atAlice;
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        for (auto& e : host.TakeEvents())
            atHost.push_back(e);
        for (auto& e : bob.TakeEvents())
            atBob.push_back(e);
        for (auto& e : alice.TakeEvents())
            atAlice.push_back(e);
        return !atHost.empty() && !atBob.empty();
    }));
    EXPECT_EQ(atHost[0].origin, alice.SelfId());
    EXPECT_EQ(atBob[0].origin, alice.SelfId());
    EXPECT_EQ(atBob[0].kind, EventKind::PlayerSfx);
    EXPECT_EQ(atBob[0].data, (std::vector<uint8_t>{ 0x12, 0x34 }));
    EXPECT_TRUE(atAlice.empty()); // not echoed back

    host.SendEvent(EventKind::RoomState, { 1 });
    std::vector<Event> fromHost;
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        for (auto& e : alice.TakeEvents())
            fromHost.push_back(e);
        return !fromHost.empty();
    }));
    EXPECT_EQ(fromHost[0].origin, kHostPlayerId);
}

// voice goes on the lane that can drop packets, the host must relay it too
TEST(SessionRelay, FastLaneEventsAreRelayedToo) {
    Session host, alice, bob;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    ASSERT_TRUE(alice.Join("127.0.0.1", host.Port(), MakeHello("Alice")));
    ASSERT_TRUE(bob.Join("127.0.0.1", host.Port(), MakeHello("Bob")));
    ASSERT_TRUE(TickAll(host, alice, bob, [&] { return alice.SelfId() != 0 && bob.SelfId() != 0; }));

    std::vector<Event> atBob;
    ASSERT_TRUE(TickAll(host, alice, bob, [&] {
        alice.SendEvent(EventKind::Voice, { 7, 7, 7 }, Channel::Snapshot); // can be lost, so keep sending
        for (auto& e : bob.TakeEvents())
            atBob.push_back(e);
        return !atBob.empty();
    }));
    EXPECT_EQ(atBob[0].kind, EventKind::Voice);
    EXPECT_EQ(atBob[0].origin, alice.SelfId());
}

// Crests of Courage takes 2 to 16 players, we used to stop at 5
TEST(SessionRelay, SixteenPlayersFit) {
    Session host;
    ASSERT_TRUE(host.Host(0, MakeHello("Host")));
    std::vector<std::unique_ptr<Session>> clients;
    for (int i = 0; i < 15; i++) {
        clients.push_back(std::make_unique<Session>());
        std::string name = "P" + std::to_string(i);
        ASSERT_TRUE(clients.back()->Join("127.0.0.1", host.Port(), MakeHello(name.c_str())));
    }
    auto end = std::chrono::steady_clock::now() + 5s;
    PlayerState s = StateAt(0);
    while (std::chrono::steady_clock::now() < end && host.Players().size() < 15) {
        host.Tick(s);
        for (auto& c : clients)
            c->Tick(s);
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(host.Players().size(), 15u);
}
