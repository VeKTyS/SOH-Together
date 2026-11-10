// Room codes / same wifi helpers (Transport.h, namespace Coop::Nat) and the raw packets on
// the game socket that CoopLobby.cpp uses for STUN, hole punching and "find games on my wifi".

#include "soh/Network/Coop/Transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <thread>

using namespace Coop;
using namespace std::chrono_literals;

namespace {

std::vector<uint8_t> Bytes(std::initializer_list<int> values) {
    std::vector<uint8_t> out;
    for (int v : values) {
        out.push_back(static_cast<uint8_t>(v));
    }
    return out;
}

// A STUN binding success reply holding one address attribute
std::vector<uint8_t> StunReply(uint16_t type, const uint8_t ip[4], uint16_t port, bool xorIt) {
    std::vector<uint8_t> out = Bytes({ 0x01, 0x01, 0x00, 12, 0x21, 0x12, 0xA4, 0x42 });
    for (int i = 0; i < 12; i++) {
        out.push_back(static_cast<uint8_t>(i));
    }
    uint16_t p = xorIt ? static_cast<uint16_t>(port ^ 0x2112) : port;
    const uint8_t cookie[4] = { 0x21, 0x12, 0xA4, 0x42 };
    std::vector<uint8_t> attr = { static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(type), 0, 8, 0, 1,
                                  static_cast<uint8_t>(p >> 8),    static_cast<uint8_t>(p) };
    for (int i = 0; i < 4; i++) {
        attr.push_back(static_cast<uint8_t>(xorIt ? ip[i] ^ cookie[i] : ip[i]));
    }
    out.insert(out.end(), attr.begin(), attr.end());
    return out;
}

} // namespace

// ---- pure helpers ----

TEST(Nat, ParsesEndpoints) {
    UdpEndpoint e;
    ASSERT_TRUE(ParseUdpEndpoint("203.0.113.5:27720", e));
    EXPECT_EQ(e.ip, "203.0.113.5");
    EXPECT_EQ(e.port, 27720);
    EXPECT_EQ(e.Text(), "203.0.113.5:27720");
    EXPECT_FALSE(ParseUdpEndpoint("203.0.113.5", e));
    EXPECT_FALSE(ParseUdpEndpoint("203.0.113.5:0", e));
    EXPECT_FALSE(ParseUdpEndpoint("203.0.113.5:70000", e));
    EXPECT_FALSE(ParseUdpEndpoint("256.0.0.1:80", e));
    EXPECT_FALSE(ParseUdpEndpoint("1.2.3:80", e));
    EXPECT_FALSE(ParseUdpEndpoint("1.2.3.4.5:80", e));
    EXPECT_FALSE(ParseUdpEndpoint("example.com:80", e)); // names would block the game on a lookup
    EXPECT_FALSE(ParseUdpEndpoint(":80", e));
}

TEST(Nat, StunRequestIsABindingRequest) {
    Nat::StunTxn txn{};
    for (size_t i = 0; i < txn.size(); i++) {
        txn[i] = static_cast<uint8_t>(0xA0 + i);
    }
    std::vector<uint8_t> packet = Nat::StunRequest(txn);
    ASSERT_EQ(packet.size(), 20u);
    EXPECT_EQ(packet[0], 0x00);
    EXPECT_EQ(packet[1], 0x01);
    EXPECT_EQ(packet[2], 0x00);
    EXPECT_EQ(packet[3], 0x00);
    EXPECT_EQ(Nat::StunTxnOf(packet), txn);
}

TEST(Nat, ReadsXorMappedAddress) {
    const uint8_t ip[4] = { 198, 51, 100, 7 };
    std::vector<uint8_t> reply = StunReply(0x0020, ip, 54321, true);
    ASSERT_TRUE(Nat::LooksLikeStun(reply));
    UdpEndpoint mapped;
    ASSERT_TRUE(Nat::ParseStunReply(reply, mapped));
    EXPECT_EQ(mapped.Text(), "198.51.100.7:54321");
}

TEST(Nat, FallsBackToPlainMappedAddress) {
    const uint8_t ip[4] = { 192, 0, 2, 1 };
    UdpEndpoint mapped;
    ASSERT_TRUE(Nat::ParseStunReply(StunReply(0x0001, ip, 4000, false), mapped));
    EXPECT_EQ(mapped.Text(), "192.0.2.1:4000");
}

TEST(Nat, RejectsBrokenStun) {
    const uint8_t ip[4] = { 192, 0, 2, 1 };
    std::vector<uint8_t> reply = StunReply(0x0020, ip, 4000, true);
    UdpEndpoint mapped;

    std::vector<uint8_t> cut(reply.begin(), reply.end() - 3); // says 12 bytes of attributes, has 9
    EXPECT_FALSE(Nat::ParseStunReply(cut, mapped));

    std::vector<uint8_t> request = reply;
    request[1] = 0x01;
    request[0] = 0x00; // a request, not a success reply
    EXPECT_FALSE(Nat::ParseStunReply(request, mapped));

    std::vector<uint8_t> ipv6 = reply;
    ipv6[25] = 0x02; // family 2 = IPv6, we only do IPv4
    EXPECT_FALSE(Nat::ParseStunReply(ipv6, mapped));

    std::vector<uint8_t> tooLong = reply;
    tooLong[23] = 200; // attribute longer than the message
    EXPECT_FALSE(Nat::ParseStunReply(tooLong, mapped));

    EXPECT_FALSE(Nat::LooksLikeStun(Bytes({ 1, 2, 3 })));
}

TEST(Nat, PunchPacketsRoundTrip) {
    // same bytes as Crests of Courage: "CPH1", token little endian, kind
    std::vector<uint8_t> packet = Nat::PunchPacket(0x0123456789ABCDEFull, Nat::PunchKind::Probe);
    ASSERT_EQ(packet.size(), Nat::kPunchSize);
    EXPECT_EQ(std::string(packet.begin(), packet.begin() + 4), "CPH1");
    EXPECT_EQ(packet[4], 0xEF);
    EXPECT_EQ(packet[11], 0x01);
    uint64_t token = 0;
    Nat::PunchKind kind;
    ASSERT_TRUE(Nat::ParsePunch(packet, token, kind));
    EXPECT_EQ(token, 0x0123456789ABCDEFull);
    EXPECT_EQ(kind, Nat::PunchKind::Probe);

    packet.push_back(0);
    EXPECT_FALSE(Nat::ParsePunch(packet, token, kind));
    std::vector<uint8_t> wrongMagic = Nat::PunchPacket(1, Nat::PunchKind::Answer);
    wrongMagic[0] = 'X';
    EXPECT_FALSE(Nat::ParsePunch(wrongMagic, token, kind));
}

TEST(Nat, HexTokensMatchTheServer) {
    // the server sends 8 random bytes as 16 hex digits
    uint64_t token = 0;
    ASSERT_TRUE(Nat::ParseHexToken("0123456789abcDEF", token));
    EXPECT_EQ(token, 0x0123456789ABCDEFull);
    EXPECT_FALSE(Nat::ParseHexToken("0123", token));
    EXPECT_FALSE(Nat::ParseHexToken("0123456789abcdeg", token));
}

TEST(Nat, NormalizesRoomCodes) {
    EXPECT_EQ(Nat::NormalizeRoomCode(" k7w-q2p "), "K7WQ2P");
    EXPECT_EQ(Nat::NormalizeRoomCode("My Room!"), "MYROOM");
    EXPECT_EQ(Nat::NormalizeRoomCode("été"), "T"); // not ASCII = dropped, like the server
}

TEST(Nat, KnowsPrivateAddresses) {
    EXPECT_TRUE(Nat::IsPrivateIPv4("10.0.0.2"));
    EXPECT_TRUE(Nat::IsPrivateIPv4("192.168.1.20"));
    EXPECT_TRUE(Nat::IsPrivateIPv4("172.16.0.1"));
    EXPECT_TRUE(Nat::IsPrivateIPv4("172.31.255.1"));
    EXPECT_FALSE(Nat::IsPrivateIPv4("172.32.0.1"));
    EXPECT_FALSE(Nat::IsPrivateIPv4("100.64.0.1")); // Tailscale / carrier NAT, not home wifi
    EXPECT_FALSE(Nat::IsPrivateIPv4("8.8.8.8"));
    EXPECT_FALSE(Nat::IsPrivateIPv4("not an ip"));
}

TEST(Nat, LanAddressIsPrivateOrEmpty) {
    // depends on the machine: only check the shape
    std::string lan = Nat::LanAddress();
    EXPECT_TRUE(lan.empty() || Nat::IsPrivateIPv4(lan)) << lan;
    std::string ts = Nat::TailscaleAddress();
    EXPECT_TRUE(ts.empty() || ts.rfind("100.", 0) == 0) << ts;
}

TEST(Nat, LanPacketsRoundTrip) {
    EXPECT_TRUE(Nat::IsLanProbe(Nat::LanProbe()));
    std::vector<uint8_t> answer = Nat::LanAnswer(10, 27720, "Malon");
    EXPECT_FALSE(Nat::IsLanProbe(answer));
    uint16_t protocol = 0, port = 0;
    std::string name;
    ASSERT_TRUE(Nat::ParseLanAnswer(answer, protocol, port, name));
    EXPECT_EQ(protocol, 10);
    EXPECT_EQ(port, 27720);
    EXPECT_EQ(name, "Malon");

    // long names are cut, garbage is refused
    ASSERT_TRUE(Nat::ParseLanAnswer(Nat::LanAnswer(10, 1, std::string(100, 'a')), protocol, port, name));
    EXPECT_EQ(name.size(), 32u);
    answer.pop_back();
    EXPECT_FALSE(Nat::ParseLanAnswer(answer, protocol, port, name));
    EXPECT_FALSE(Nat::ParseLanAnswer(Nat::LanAnswer(10, 0, "x"), protocol, port, name));
    EXPECT_FALSE(Nat::ParseLanAnswer(Nat::LanProbe(), protocol, port, name));
}

// ---- raw packets on real sockets (loopback, like TransportTests) ----

namespace {

TransportConfig MakeConfig(const char* name) {
    TransportConfig c;
    c.localHello.buildHash = 1;
    c.localHello.playerName = name;
    return c;
}

// Clears the process-wide handler and join socket even if a test fails halfway.
struct RawGuard {
    ~RawGuard() {
        Transport::SetRawHandler(nullptr);
        Transport::CloseJoinSocket();
    }
};

} // namespace

TEST(RawUdp, JoinSocketIsTheOneConnectUses) {
    RawGuard guard;
    // What the host hears, raw: who sent it and from which port
    std::vector<UdpEndpoint> heard;
    Transport::SetRawHandler([&](const UdpEndpoint& from, std::span<const uint8_t> data) {
        uint64_t token;
        Nat::PunchKind kind;
        if (!Nat::ParsePunch(data, token, kind)) {
            return false; // not ours: ENet's
        }
        heard.push_back(from);
        return true;
    });

    auto host = Transport::Listen(0, MakeConfig("host"));
    ASSERT_NE(host, nullptr);
    const UdpEndpoint hostAt{ "127.0.0.1", host->LocalPort() };

    const uint16_t joinPort = Transport::OpenJoinSocket();
    ASSERT_NE(joinPort, 0);
    EXPECT_TRUE(Transport::JoinSocketOpen());
    EXPECT_EQ(Transport::OpenJoinSocket(), joinPort); // twice = the same one

    // the joiner punches from the join socket; the host's ENet socket hands it to the handler
    ASSERT_TRUE(Transport::SendRaw(hostAt, Nat::PunchPacket(42, Nat::PunchKind::Probe)));
    std::vector<TransportEvent> hostEvents;
    for (int i = 0; i < 200 && heard.empty(); i++) {
        host->Poll(hostEvents);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(heard.size(), 1u);
    EXPECT_EQ(heard[0].port, joinPort);
    EXPECT_TRUE(hostEvents.empty()); // ENet never saw the punch

    // the real join reuses that socket: the host sees the same port again
    auto client = Transport::Connect("127.0.0.1", hostAt.port, MakeConfig("client"));
    ASSERT_NE(client, nullptr);
    EXPECT_FALSE(Transport::JoinSocketOpen());
    EXPECT_EQ(client->LocalPort(), joinPort);

    std::vector<TransportEvent> clientEvents;
    bool connected = false;
    for (int i = 0; i < 600 && !connected; i++) {
        host->Poll(hostEvents);
        client->Poll(clientEvents);
        for (const TransportEvent& ev : clientEvents) {
            connected = connected || ev.type == TransportEvent::Type::Connected;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(connected);

    // punches that arrive after the join still never reach ENet
    ASSERT_TRUE(host->SendRawFrom({ "127.0.0.1", joinPort }, Nat::PunchPacket(42, Nat::PunchKind::Answer)));
    heard.clear();
    for (int i = 0; i < 200 && heard.empty(); i++) {
        client->Poll(clientEvents);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(heard.size(), 1u);
    EXPECT_EQ(heard[0].port, hostAt.port);
}

TEST(RawUdp, JoinSocketReadsGoToTheHandler) {
    RawGuard guard;
    std::vector<std::vector<uint8_t>> got;
    Transport::SetRawHandler([&](const UdpEndpoint&, std::span<const uint8_t> data) {
        got.emplace_back(data.begin(), data.end());
        return true;
    });
    const uint16_t joinPort = Transport::OpenJoinSocket();
    ASSERT_NE(joinPort, 0);

    auto host = Transport::Listen(0, MakeConfig("host"));
    ASSERT_NE(host, nullptr);
    // a "same wifi" answer from the host's socket to the join socket
    std::vector<uint8_t> answer = Nat::LanAnswer(kProtocolVersion, host->LocalPort(), "Saria");
    ASSERT_TRUE(host->SendRawFrom({ "127.0.0.1", joinPort }, answer));
    for (int i = 0; i < 200 && got.empty(); i++) {
        Transport::PollJoinSocket();
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], answer);
}

TEST(RawUdp, NoHandlerLeavesEverythingToEnet) {
    RawGuard guard;
    Transport::SetRawHandler(nullptr);
    // the normal handshake still works with the intercept installed and nobody listening
    auto host = Transport::Listen(0, MakeConfig("host"));
    ASSERT_NE(host, nullptr);
    auto client = Transport::Connect("127.0.0.1", host->LocalPort(), MakeConfig("client"));
    ASSERT_NE(client, nullptr);
    std::vector<TransportEvent> hostEvents, clientEvents;
    bool connected = false;
    for (int i = 0; i < 600 && !connected; i++) {
        host->Poll(hostEvents);
        client->Poll(clientEvents);
        for (const TransportEvent& ev : hostEvents) {
            connected = connected || ev.type == TransportEvent::Type::Connected;
        }
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(connected);
    EXPECT_FALSE(Transport::SendRaw({ "127.0.0.1", 0 }, Nat::LanProbe())); // port 0: refused
}
