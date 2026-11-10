// Integration tests: a real host and a real client exchange UDP packets over
// the loopback interface (127.0.0.1) inside this process.

#include "soh/Network/Coop/Transport.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <enet/enet.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <thread>

using namespace Coop;
using namespace std::chrono_literals;

namespace {

using Events = std::vector<TransportEvent>;
using Type = TransportEvent::Type;

TransportConfig MakeConfig(const char* name, uint64_t build = 1, uint64_t seed = 0) {
    TransportConfig c;
    c.localHello.buildHash = build;
    c.localHello.seedHash = seed;
    c.localHello.playerName = name;
    return c;
}

// Polls both sides until the condition holds. Loopback is fast: the deadline
// is generous only to absorb a loaded machine, it is not a pacing mechanism.
bool PumpUntil(Transport* a, Events& ea, Transport* b, Events& eb, const std::function<bool()>& done,
               std::chrono::milliseconds deadline = 3s) {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        if (a != nullptr) {
            a->Poll(ea);
        }
        if (b != nullptr) {
            b->Poll(eb);
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

const TransportEvent* Find(const Events& events, Type type) {
    auto it = std::find_if(events.begin(), events.end(), [&](const TransportEvent& e) { return e.type == type; });
    return it == events.end() ? nullptr : &*it;
}

size_t Count(const Events& events, Type type) {
    return std::count_if(events.begin(), events.end(), [&](const TransportEvent& e) { return e.type == type; });
}

struct Session {
    std::unique_ptr<Transport> host;
    std::unique_ptr<Transport> client;
    Events hostEvents;
    Events clientEvents;

    Session(TransportConfig hostConfig, TransportConfig clientConfig) {
        host = Transport::Listen(0, hostConfig);
        if (host) {
            client = Transport::Connect("127.0.0.1", host->LocalPort(), clientConfig);
        }
    }

    bool Pump(const std::function<bool()>& done, std::chrono::milliseconds deadline = 3s) {
        return PumpUntil(host.get(), hostEvents, client.get(), clientEvents, done, deadline);
    }

    bool BothConnected() {
        return Pump([&] { return Find(hostEvents, Type::Connected) && Find(clientEvents, Type::Connected); });
    }
};

// Bare ENet client that does not speak our protocol.
struct ForeignClient {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;

    explicit ForeignClient(uint16_t port) {
        enet_initialize();
        host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        ENetAddress address{};
        enet_address_set_host(&address, "127.0.0.1");
        address.port = port;
        peer = enet_host_connect(host, &address, kChannelCount, 0);
    }
    ~ForeignClient() {
        enet_host_destroy(host);
        enet_deinitialize();
    }

    // Returns the ENet event type seen (NONE if nothing), stores disconnect data.
    ENetEventType Service(uint32_t* disconnectData = nullptr) {
        ENetEvent event;
        if (enet_host_service(host, &event, 0) <= 0) {
            return ENET_EVENT_TYPE_NONE;
        }
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
        if (event.type == ENET_EVENT_TYPE_DISCONNECT && disconnectData != nullptr) {
            *disconnectData = event.data;
        }
        return event.type;
    }

    void Send(uint8_t channel, std::vector<uint8_t> bytes) {
        enet_peer_send(peer, channel, enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE));
        enet_host_flush(host);
    }
};

} // namespace

TEST(Transport, CompatiblePeersConnectAndSeeEachOther) {
    Session s(MakeConfig("Zelda"), MakeConfig("Link"));
    ASSERT_TRUE(s.host && s.client);
    ASSERT_TRUE(s.BothConnected());

    EXPECT_EQ(Find(s.hostEvents, Type::Connected)->remoteHello.playerName, "Link");
    EXPECT_EQ(Find(s.clientEvents, Type::Connected)->remoteHello.playerName, "Zelda");
    EXPECT_EQ(s.host->ReadyPeerCount(), 1u);
    EXPECT_EQ(s.client->ReadyPeerCount(), 1u);
}

TEST(Transport, OrderedChannelKeepsOrder) {
    Session s(MakeConfig("Zelda"), MakeConfig("Link"));
    ASSERT_TRUE(s.BothConnected());
    PeerId toHost = Find(s.clientEvents, Type::Connected)->peer;

    for (uint8_t i = 0; i < 100; i++) {
        ASSERT_TRUE(s.client->Send(toHost, Channel::Ordered, std::vector<uint8_t>{ i }));
    }
    ASSERT_TRUE(s.Pump([&] { return Count(s.hostEvents, Type::Message) == 100; }));

    uint8_t expected = 0;
    for (const TransportEvent& e : s.hostEvents) {
        if (e.type == Type::Message) {
            EXPECT_EQ(e.channel, Channel::Ordered);
            ASSERT_EQ(e.payload.size(), 1u);
            EXPECT_EQ(e.payload[0], expected++);
        }
    }
}

TEST(Transport, RpcChannelDeliversWithItsChannelTag) {
    Session s(MakeConfig("Zelda"), MakeConfig("Link"));
    ASSERT_TRUE(s.BothConnected());
    PeerId toClient = Find(s.hostEvents, Type::Connected)->peer;

    std::vector<uint8_t> payload = { 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_TRUE(s.host->Send(toClient, Channel::Rpc, payload));
    ASSERT_TRUE(s.Pump([&] { return Find(s.clientEvents, Type::Message) != nullptr; }));

    const TransportEvent* msg = Find(s.clientEvents, Type::Message);
    EXPECT_EQ(msg->channel, Channel::Rpc);
    EXPECT_EQ(msg->payload, payload);
}

TEST(Transport, LargeReliableMessageIsReassembled) {
    Session s(MakeConfig("Zelda"), MakeConfig("Link"));
    ASSERT_TRUE(s.BothConnected());
    PeerId toClient = Find(s.hostEvents, Type::Connected)->peer;

    // About the size of a save file: far above one UDP packet (~1400 bytes).
    std::vector<uint8_t> save(80000);
    for (size_t i = 0; i < save.size(); i++) {
        save[i] = static_cast<uint8_t>(i * 31);
    }
    ASSERT_TRUE(s.host->Send(toClient, Channel::Ordered, save));
    ASSERT_TRUE(s.Pump([&] { return Find(s.clientEvents, Type::Message) != nullptr; }));
    EXPECT_EQ(Find(s.clientEvents, Type::Message)->payload, save);
}

TEST(Transport, BuildMismatchIsRefusedOnBothSidesWithTheReason) {
    Session s(MakeConfig("Zelda", 1), MakeConfig("Link", 2));
    ASSERT_TRUE(
        s.Pump([&] { return Find(s.hostEvents, Type::Disconnected) && Find(s.clientEvents, Type::Disconnected); }));

    for (const Events* events : { &s.hostEvents, &s.clientEvents }) {
        EXPECT_EQ(Find(*events, Type::Connected), nullptr);
        const TransportEvent* d = Find(*events, Type::Disconnected);
        EXPECT_EQ(d->reason, DisconnectReason::Refused);
        EXPECT_EQ(d->handshakeError, HandshakeError::BuildMismatch);
    }
}

TEST(Transport, SeedMismatchIsRefused) {
    Session s(MakeConfig("Zelda", 1, 111), MakeConfig("Link", 1, 222));
    ASSERT_TRUE(s.Pump([&] { return Find(s.clientEvents, Type::Disconnected) != nullptr; }));
    EXPECT_EQ(Find(s.clientEvents, Type::Disconnected)->handshakeError, HandshakeError::SeedMismatch);
}

TEST(Transport, RefusedPeerIsReportedOnlyOnce) {
    Session s(MakeConfig("Zelda", 1), MakeConfig("Link", 2));
    ASSERT_TRUE(s.Pump([&] { return s.host->ReadyPeerCount() == 0 && Find(s.hostEvents, Type::Disconnected); }));
    // Keep pumping: ENet's own disconnect notification must not produce a second event.
    s.Pump([] { return false; }, 300ms);
    EXPECT_EQ(Count(s.hostEvents, Type::Disconnected), 1u);
    EXPECT_EQ(Count(s.clientEvents, Type::Disconnected), 1u);
}

TEST(Transport, SendIsRefusedBeforeHandshakeAndToUnknownPeers) {
    Session s(MakeConfig("Zelda"), MakeConfig("Link"));
    std::vector<uint8_t> payload = { 1 };
    EXPECT_FALSE(s.client->Send(1, Channel::Ordered, payload)); // tracked but not ready yet
    EXPECT_FALSE(s.client->Send(999, Channel::Ordered, payload));
}

TEST(Transport, GracefulDisconnectIsSeenByBothSides) {
    Session s(MakeConfig("Zelda"), MakeConfig("Link"));
    ASSERT_TRUE(s.BothConnected());
    PeerId toHost = Find(s.clientEvents, Type::Connected)->peer;

    s.client->Disconnect(toHost);
    ASSERT_TRUE(
        s.Pump([&] { return Find(s.hostEvents, Type::Disconnected) && Find(s.clientEvents, Type::Disconnected); }));
    EXPECT_EQ(Find(s.hostEvents, Type::Disconnected)->reason, DisconnectReason::Normal);
    EXPECT_EQ(Find(s.clientEvents, Type::Disconnected)->reason, DisconnectReason::Normal);
}

TEST(Transport, ForeignProtocolIsRefusedAsBadMagic) {
    auto host = Transport::Listen(0, MakeConfig("Zelda"));
    ASSERT_TRUE(host);
    ForeignClient foreign(host->LocalPort());
    Events events;
    bool sent = false;
    uint32_t data = 0;
    bool foreignDisconnected = false;

    ASSERT_TRUE(PumpUntil(host.get(), events, nullptr, events, [&] {
        ENetEventType t = foreign.Service(&data);
        if (t == ENET_EVENT_TYPE_CONNECT && !sent) {
            foreign.Send(0, { 'G', 'E', 'T', ' ', '/', ' ' });
            sent = true;
        }
        foreignDisconnected |= t == ENET_EVENT_TYPE_DISCONNECT;
        return foreignDisconnected && Find(events, Type::Disconnected) != nullptr;
    }));

    const TransportEvent* d = Find(events, Type::Disconnected);
    EXPECT_EQ(d->reason, DisconnectReason::Refused);
    EXPECT_EQ(d->handshakeError, HandshakeError::BadMagic);
    // The foreign side also receives the reason in ENet's disconnect payload.
    DisconnectReason reason;
    HandshakeError error;
    Transport::DecodeDisconnectData(data, reason, error);
    EXPECT_EQ(reason, DisconnectReason::Refused);
    EXPECT_EQ(error, HandshakeError::BadMagic);
}

TEST(Transport, MessageBeforeHelloOnAnotherChannelIsAViolation) {
    auto host = Transport::Listen(0, MakeConfig("Zelda"));
    ASSERT_TRUE(host);
    ForeignClient foreign(host->LocalPort());
    Events events;
    bool sent = false;

    ASSERT_TRUE(PumpUntil(host.get(), events, nullptr, events, [&] {
        if (foreign.Service() == ENET_EVENT_TYPE_CONNECT && !sent) {
            foreign.Send(static_cast<uint8_t>(Channel::Snapshot), { 1, 2, 3 });
            sent = true;
        }
        return Find(events, Type::Disconnected) != nullptr;
    }));
    EXPECT_EQ(Find(events, Type::Disconnected)->reason, DisconnectReason::ProtocolViolation);
}

TEST(Transport, SilentPeerIsDroppedAfterHandshakeTimeout) {
    TransportConfig config = MakeConfig("Zelda");
    config.handshakeTimeout = 200ms;
    auto host = Transport::Listen(0, config);
    ASSERT_TRUE(host);
    ForeignClient foreign(host->LocalPort());
    Events events;

    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(PumpUntil(host.get(), events, nullptr, events, [&] {
        foreign.Service();
        return Find(events, Type::Disconnected) != nullptr;
    }));
    EXPECT_EQ(Find(events, Type::Disconnected)->reason, DisconnectReason::HandshakeTimeout);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 200ms);
}

TEST(Transport, DisconnectDataRoundTripAndGarbageIsSafe) {
    DisconnectReason reason;
    HandshakeError error;
    Transport::DecodeDisconnectData(
        Transport::EncodeDisconnectData(DisconnectReason::Refused, HandshakeError::SeedMismatch), reason, error);
    EXPECT_EQ(reason, DisconnectReason::Refused);
    EXPECT_EQ(error, HandshakeError::SeedMismatch);

    // A peer timing out gives 0; a buggy peer could send anything.
    Transport::DecodeDisconnectData(0, reason, error);
    EXPECT_EQ(reason, DisconnectReason::ConnectionLost);
    Transport::DecodeDisconnectData(0xFFFFFFFF, reason, error);
    EXPECT_EQ(reason, DisconnectReason::ConnectionLost);
    EXPECT_EQ(error, HandshakeError::None);
}

TEST(Transport, LocalAddressesAreDottedQuadsWithoutLoopback) {
    for (const std::string& ip : LocalIPv4Addresses()) {
        EXPECT_EQ(std::count(ip.begin(), ip.end(), '.'), 3) << ip;
        EXPECT_NE(ip.rfind("127.", 0), 0u) << ip;
    }
}

TEST(Transport, RoundTripIsZeroForUnknownPeerAndMeasuredForReadyPeer) {
    Session s(MakeConfig("Zelda"), MakeConfig("Link"));
    ASSERT_TRUE(s.BothConnected());
    EXPECT_EQ(s.host->RoundTripMs(999), 0u);
    // ENet starts at 500 ms until its first ping is acknowledged (every 500 ms).
    PeerId toClient = Find(s.hostEvents, Type::Connected)->peer;
    ASSERT_TRUE(s.Pump([&] { return s.host->RoundTripMs(toClient) < 100; }, 3s));
}
