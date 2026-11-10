#include "soh/Network/Coop/Transport.h"

// enet.h pulls in winsock2.h: keep it out of every header, and stop
// windows.h from defining min/max macros that would break std::min.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <enet/enet.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <mutex>

#ifdef _WIN32
// Without this, Windows "closes" a UDP socket's reads with an error each time a packet we
// sent bounces (nobody listening there). Punching sends to addresses that may not answer.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

namespace Coop {

namespace {

// enet_initialize() is process-wide: reference-count it so several Transports
// (host + client in tests, or a future relay) can coexist.
// The mutex is intentionally never destroyed: a static Session owned by the
// game is destroyed at process exit, possibly after this file's statics.
std::mutex& EnetMutex() {
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}
int gEnetUsers = 0;

bool AcquireEnet() {
    std::lock_guard lock(EnetMutex());
    if (gEnetUsers == 0 && enet_initialize() != 0) {
        return false;
    }
    gEnetUsers++;
    return true;
}

void ReleaseEnet() {
    std::lock_guard lock(EnetMutex());
    if (--gEnetUsers == 0) {
        enet_deinitialize();
    }
}

enet_uint32 PacketFlags(Channel channel) {
    switch (channel) {
        case Channel::Snapshot:
            // Unreliable and sequenced. Without UNRELIABLE_FRAGMENT, ENet would
            // silently send a large snapshot as reliable.
            return ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
        case Channel::Ordered:
        case Channel::Rpc:
            return ENET_PACKET_FLAG_RELIABLE;
    }
    return ENET_PACKET_FLAG_RELIABLE;
}

// --- raw packets (room codes / same wifi), see Transport.h ---
// Same trick as EnetMutex: never destroyed, a Transport may die at process exit.
std::mutex& RawMutex() {
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}
std::shared_ptr<Transport::RawHandler> gRawHandler; // copied before calling, so it can be replaced meanwhile
std::vector<ENetHost*> gLiveHosts;                  // every Transport's socket, newest last
ENetHost* gJoinHost = nullptr;                      // OpenJoinSocket(), waiting for Connect()

UdpEndpoint ToEndpoint(const ENetAddress& address) {
    UdpEndpoint out;
    char text[64] = {};
    if (enet_address_get_host_ip(&address, text, sizeof(text)) == 0) {
        out.ip = text;
    }
    out.port = address.port;
    return out;
}

bool ToAddress(const UdpEndpoint& endpoint, ENetAddress& out) {
    out = {};
    if (endpoint.port == 0 || enet_address_set_host_ip(&out, endpoint.ip.c_str()) != 0) {
        return false;
    }
    out.port = endpoint.port;
    return true;
}

bool DeliverRaw(const ENetAddress& from, std::span<const uint8_t> data) {
    std::shared_ptr<Transport::RawHandler> handler;
    {
        std::lock_guard lock(RawMutex());
        handler = gRawHandler;
    }
    // called without the lock: the handler may well SendRaw() an answer right away
    return handler != nullptr && *handler && (*handler)(ToEndpoint(from), data);
}

int ENET_CALLBACK InterceptRaw(ENetHost* host, ENetEvent* /*event*/) {
    return DeliverRaw(host->receivedAddress, { host->receivedData, host->receivedDataLength }) ? 1 : 0;
}

bool SendOn(ENetHost* host, const UdpEndpoint& to, std::span<const uint8_t> data) {
    ENetAddress address;
    if (host == nullptr || data.empty() || !ToAddress(to, address)) {
        return false;
    }
    ENetBuffer buffer;
    buffer.data = const_cast<uint8_t*>(data.data());
    buffer.dataLength = data.size();
    return enet_socket_send(host->socket, &address, &buffer, 1) > 0;
}

uint16_t PortOf(ENetHost* host) {
    ENetAddress address{};
    if (host == nullptr || enet_socket_get_address(host->socket, &address) != 0) {
        return 0;
    }
    return address.port;
}

ENetHost* CreateHost(const ENetAddress* address, size_t maxPeers) {
    ENetHost* host = enet_host_create(address, maxPeers, kChannelCount, 0, 0);
    if (host != nullptr) {
        host->maximumPacketSize = kMaxMessageBytes;
        host->intercept = InterceptRaw;
#ifdef _WIN32
        BOOL reportBounces = FALSE;
        DWORD unused = 0;
        WSAIoctl(host->socket, SIO_UDP_CONNRESET, &reportBounces, sizeof(reportBounces), nullptr, 0, &unused, nullptr,
                 nullptr);
#endif
    }
    return host;
}

// The join socket, if one is waiting. The caller now owns it (and its AcquireEnet()).
ENetHost* TakeJoinHost() {
    std::lock_guard lock(RawMutex());
    ENetHost* host = gJoinHost;
    gJoinHost = nullptr;
    return host;
}

uint32_t RouteSource(uint32_t towards); // below, with the Nat helpers

} // namespace

bool ParseUdpEndpoint(const std::string& text, UdpEndpoint& out) {
    size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    std::string ip = text.substr(0, colon);
    std::string digits = text.substr(colon + 1);
    if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    unsigned long port = std::stoul(digits);
    // four numbers 0-255 with dots, nothing else
    int parts = 0;
    size_t at = 0;
    while (at <= ip.size()) {
        size_t dot = ip.find('.', at);
        std::string part = ip.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos ||
            std::stoul(part) > 255) {
            return false;
        }
        parts++;
        if (dot == std::string::npos) {
            break;
        }
        at = dot + 1;
    }
    if (parts != 4 || port == 0 || port > 65535) {
        return false;
    }
    out.ip = ip;
    out.port = static_cast<uint16_t>(port);
    return true;
}

namespace Nat {

namespace {

const uint32_t kPunchMagic = 0x31485043u; // "CPH1" in memory, same as Crests of Courage
const uint32_t kLanMagic = 0x4C485453u;   // "STHL" in memory: SOH-Together LAN
const uint8_t kLanProbe = 1;
const uint8_t kLanAnswer = 2;
const size_t kLanNameMax = 32;

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::string Dotted(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xFF) + "." + std::to_string((ip >> 8) & 0xFF) +
           "." + std::to_string(ip & 0xFF);
}

} // namespace

std::vector<uint8_t> StunRequest(const StunTxn& txn) {
    // binding request, no attributes, magic cookie, then the transaction id
    std::vector<uint8_t> packet = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
    packet.insert(packet.end(), txn.begin(), txn.end());
    return packet;
}

bool LooksLikeStun(std::span<const uint8_t> data) {
    // same test as online_on_datagram
    return data.size() >= 20 && data[4] == 0x21 && data[5] == 0x12 && data[6] == 0xA4 && data[7] == 0x42 &&
           (data[0] & 0xC0) == 0;
}

StunTxn StunTxnOf(std::span<const uint8_t> data) {
    StunTxn txn{};
    if (data.size() >= 20) {
        std::memcpy(txn.data(), data.data() + 8, txn.size());
    }
    return txn;
}

bool ParseStunReply(std::span<const uint8_t> data, UdpEndpoint& mapped) {
    // copied from Crests of Courage online.cpp parse_stun
    if (data.size() < 20 || data[0] != 0x01 || data[1] != 0x01) {
        return false;
    }
    const size_t length = (static_cast<size_t>(data[2]) << 8) | data[3];
    if (20 + length > data.size()) {
        return false;
    }
    UdpEndpoint plain;
    for (size_t at = 20; at + 4 <= 20 + length;) {
        const uint16_t type = static_cast<uint16_t>((data[at] << 8) | data[at + 1]);
        const size_t len = (static_cast<size_t>(data[at + 2]) << 8) | data[at + 3];
        const uint8_t* v = data.data() + at + 4;
        if (at + 4 + len > 20 + length) {
            break;
        }
        // 0x0020 XOR-MAPPED-ADDRESS, 0x0001 MAPPED-ADDRESS; family 1 = IPv4
        if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01) {
            uint16_t port = static_cast<uint16_t>((v[2] << 8) | v[3]);
            uint8_t ip[4] = { v[4], v[5], v[6], v[7] };
            if (type == 0x0020) {
                port ^= static_cast<uint16_t>(kStunCookie >> 16);
                ip[0] ^= 0x21;
                ip[1] ^= 0x12;
                ip[2] ^= 0xA4;
                ip[3] ^= 0x42;
            }
            UdpEndpoint found;
            found.ip = std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." +
                       std::to_string(ip[3]);
            found.port = port;
            if (type == 0x0020) {
                mapped = found;
                return true;
            }
            plain = found;
        }
        at += 4 + ((len + 3) & ~static_cast<size_t>(3));
    }
    if (plain.ip.empty()) {
        return false;
    }
    mapped = plain;
    return true;
}

std::vector<uint8_t> PunchPacket(uint64_t token, PunchKind kind) {
    std::vector<uint8_t> packet;
    packet.reserve(kPunchSize);
    PutU32(packet, kPunchMagic);
    PutU32(packet, static_cast<uint32_t>(token));
    PutU32(packet, static_cast<uint32_t>(token >> 32));
    packet.push_back(static_cast<uint8_t>(kind));
    return packet;
}

bool ParsePunch(std::span<const uint8_t> data, uint64_t& token, PunchKind& kind) {
    if (data.size() != kPunchSize || GetU32(data.data()) != kPunchMagic) {
        return false;
    }
    token = static_cast<uint64_t>(GetU32(data.data() + 4)) | (static_cast<uint64_t>(GetU32(data.data() + 8)) << 32);
    kind = static_cast<PunchKind>(data[12]);
    return true;
}

bool ParseHexToken(const std::string& text, uint64_t& out) {
    if (text.size() != 16) {
        return false;
    }
    out = 0;
    for (const char c : text) {
        uint64_t v;
        if (c >= '0' && c <= '9') {
            v = static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v = static_cast<uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v = static_cast<uint64_t>(c - 'A' + 10);
        } else {
            return false;
        }
        out = (out << 4) | v;
    }
    return true;
}

std::string NormalizeRoomCode(const std::string& typed) {
    std::string code;
    for (const char c : typed) {
        if (c >= 'a' && c <= 'z') {
            code += static_cast<char>(c - 'a' + 'A');
        } else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            code += c;
        }
    }
    return code;
}

bool IsPrivateIPv4(const std::string& ip) {
    UdpEndpoint e;
    if (!ParseUdpEndpoint(ip + ":1", e)) {
        return false;
    }
    unsigned a = 0, b = 0;
    size_t dot = ip.find('.');
    a = static_cast<unsigned>(std::stoul(ip.substr(0, dot)));
    b = static_cast<unsigned>(std::stoul(ip.substr(dot + 1, ip.find('.', dot + 1) - dot - 1)));
    return a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31);
}

std::string LanAddress() {
    const uint32_t ip = RouteSource(0x08080808u);
    const uint32_t a = ip >> 24;
    const uint32_t b = (ip >> 16) & 0xFF;
    if (a == 10 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31)) {
        return Dotted(ip);
    }
    return "";
}

std::string TailscaleAddress() {
    const uint32_t ip = RouteSource(0x64646464u);
    if ((ip >> 24) == 100 && ((ip >> 16) & 0xC0) == 64) {
        return Dotted(ip);
    }
    return "";
}

std::vector<uint8_t> LanProbe() {
    std::vector<uint8_t> packet;
    PutU32(packet, kLanMagic);
    packet.push_back(kLanProbe);
    return packet;
}

bool IsLanProbe(std::span<const uint8_t> data) {
    return data.size() == 5 && GetU32(data.data()) == kLanMagic && data[4] == kLanProbe;
}

std::vector<uint8_t> LanAnswer(uint16_t protocol, uint16_t port, const std::string& name) {
    std::vector<uint8_t> packet;
    PutU32(packet, kLanMagic);
    packet.push_back(kLanAnswer);
    packet.push_back(static_cast<uint8_t>(protocol));
    packet.push_back(static_cast<uint8_t>(protocol >> 8));
    packet.push_back(static_cast<uint8_t>(port));
    packet.push_back(static_cast<uint8_t>(port >> 8));
    std::string shortName = name.substr(0, kLanNameMax);
    packet.push_back(static_cast<uint8_t>(shortName.size()));
    packet.insert(packet.end(), shortName.begin(), shortName.end());
    return packet;
}

bool ParseLanAnswer(std::span<const uint8_t> data, uint16_t& protocol, uint16_t& port, std::string& name) {
    if (data.size() < 10 || GetU32(data.data()) != kLanMagic || data[4] != kLanAnswer) {
        return false;
    }
    size_t nameLength = data[9];
    if (nameLength > kLanNameMax || data.size() != 10 + nameLength) {
        return false;
    }
    protocol = static_cast<uint16_t>(data[5] | (data[6] << 8));
    port = static_cast<uint16_t>(data[7] | (data[8] << 8));
    if (port == 0) {
        return false;
    }
    name.assign(reinterpret_cast<const char*>(data.data()) + 10, nameLength);
    return true;
}

} // namespace Nat

namespace {

// Which of our addresses the OS would use to reach `towards`. A UDP "connect" only picks
// the route, nothing is sent. Copied from Crests of Courage lan.cpp route_source.
uint32_t RouteSource(uint32_t towards) {
    if (!AcquireEnet()) { // Winsock on Windows
        return 0;
    }
    uint32_t out = 0;
    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s != ENET_SOCKET_NULL) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(53);
        to.sin_addr.s_addr = htonl(towards);
        if (connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0) {
            sockaddr_in me{};
#ifdef _WIN32
            int len = sizeof(me);
#else
            socklen_t len = sizeof(me);
#endif
            if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
                out = ntohl(me.sin_addr.s_addr);
            }
        }
        enet_socket_destroy(s);
    }
    ReleaseEnet();
    return out;
}

} // namespace

std::vector<std::string> LocalIPv4Addresses() {
    std::vector<std::string> result;
    if (!AcquireEnet()) { // also initializes Winsock on Windows
        return result;
    }
    char hostName[256] = {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* list = nullptr;
    if (gethostname(hostName, sizeof(hostName) - 1) == 0 && getaddrinfo(hostName, nullptr, &hints, &list) == 0) {
        for (addrinfo* it = list; it != nullptr; it = it->ai_next) {
            char text[INET_ADDRSTRLEN] = {};
            auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr &&
                std::string_view(text).rfind("127.", 0) != 0 &&
                std::find(result.begin(), result.end(), text) == result.end()) {
                result.emplace_back(text);
            }
        }
        freeaddrinfo(list);
    }
    ReleaseEnet();
    return result;
}

uint32_t Transport::EncodeDisconnectData(DisconnectReason reason, HandshakeError error) {
    return static_cast<uint32_t>(reason) | (static_cast<uint32_t>(error) << 8);
}

void Transport::DecodeDisconnectData(uint32_t data, DisconnectReason& reason, HandshakeError& error) {
    uint8_t r = data & 0xFF;
    uint8_t e = (data >> 8) & 0xFF;
    reason = r <= static_cast<uint8_t>(DisconnectReason::HandshakeTimeout) ? static_cast<DisconnectReason>(r)
                                                                           : DisconnectReason::ConnectionLost;
    error =
        e <= static_cast<uint8_t>(HandshakeError::SeedMismatch) ? static_cast<HandshakeError>(e) : HandshakeError::None;
}

std::unique_ptr<Transport> Transport::Listen(uint16_t port, const TransportConfig& config) {
    if (!AcquireEnet()) {
        return nullptr;
    }
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    ENetHost* host = CreateHost(&address, config.maxPeers);
    if (host == nullptr) {
        ReleaseEnet();
        return nullptr;
    }
    return std::unique_ptr<Transport>(new Transport(host, config));
}

std::unique_ptr<Transport> Transport::Connect(const std::string& hostName, uint16_t port,
                                              const TransportConfig& config) {
    if (!AcquireEnet()) {
        return nullptr;
    }
    ENetAddress address{};
    if (enet_address_set_host(&address, hostName.c_str()) != 0) {
        ReleaseEnet();
        return nullptr;
    }
    address.port = port;
    // Room codes: the joiner already punched the NAT from this socket, so play on it.
    ENetHost* host = TakeJoinHost();
    if (host != nullptr) {
        ReleaseEnet(); // the join socket already holds one
    } else {
        host = CreateHost(nullptr, 1);
    }
    if (host == nullptr) {
        ReleaseEnet();
        return nullptr;
    }
    std::unique_ptr<Transport> transport(new Transport(host, config));
    ENetPeer* peer = enet_host_connect(host, &address, kChannelCount, 0);
    if (peer == nullptr) {
        return nullptr;
    }
    // Tracked now so that a failed connection is still reported (Disconnected).
    transport->Track(peer);
    return transport;
}

Transport::Transport(ENetHost* host, const TransportConfig& config) : mHost(host), mConfig(config) {
    std::lock_guard lock(RawMutex());
    gLiveHosts.push_back(mHost);
}

Transport::~Transport() {
    for (auto& [id, state] : mPeers) {
        if (!state.closing) {
            enet_peer_disconnect_now(state.enet, EncodeDisconnectData(DisconnectReason::Normal, HandshakeError::None));
        }
    }
    enet_host_flush(mHost);
    {
        std::lock_guard lock(RawMutex());
        std::erase(gLiveHosts, mHost);
    }
    enet_host_destroy(mHost);
    ReleaseEnet();
}

PeerId Transport::Track(ENetPeer* enet) {
    PeerId id = mNextId++;
    enet->data = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
    PeerState& state = mPeers[id];
    state.enet = enet;
    state.connectedAt = std::chrono::steady_clock::now();
    return id;
}

PeerId Transport::IdOf(ENetPeer* enet) const {
    return static_cast<PeerId>(reinterpret_cast<uintptr_t>(enet->data));
}

void Transport::SendHello(ENetPeer* enet) {
    std::vector<uint8_t> hello = EncodeHello(mConfig.localHello);
    ENetPacket* packet = enet_packet_create(hello.data(), hello.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(enet, static_cast<enet_uint8>(Channel::Ordered), packet) != 0) {
        enet_packet_destroy(packet);
    }
}

void Transport::CloseLocally(PeerId id, DisconnectReason reason, HandshakeError error,
                             std::vector<TransportEvent>& out) {
    PeerState& state = mPeers.at(id);
    if (state.closing) {
        return;
    }
    state.closing = true;
    // "later" = after our queued messages (including our Hello), so the other
    // side can also work out why the session failed.
    enet_peer_disconnect_later(state.enet, EncodeDisconnectData(reason, error));

    TransportEvent ev;
    ev.type = TransportEvent::Type::Disconnected;
    ev.peer = id;
    ev.reason = reason;
    ev.handshakeError = error;
    out.push_back(std::move(ev));
}

void Transport::OnReceive(PeerId id, uint8_t channel, std::span<const uint8_t> data, std::vector<TransportEvent>& out) {
    PeerState& state = mPeers.at(id);
    if (state.closing) {
        return;
    }

    if (!state.ready) {
        // The only acceptable first message is a Hello on the ordered channel.
        if (channel != static_cast<uint8_t>(Channel::Ordered)) {
            CloseLocally(id, DisconnectReason::ProtocolViolation, HandshakeError::None, out);
            return;
        }
        HelloDecodeResult decoded = DecodeHello(data);
        HandshakeError error = decoded.error;
        if (error == HandshakeError::None) {
            error = CheckCompatibility(mConfig.localHello, decoded.hello);
        }
        if (error != HandshakeError::None) {
            CloseLocally(id, DisconnectReason::Refused, error, out);
            return;
        }
        state.ready = true;
        TransportEvent ev;
        ev.type = TransportEvent::Type::Connected;
        ev.peer = id;
        ev.remoteHello = std::move(decoded.hello);
        out.push_back(std::move(ev));
        return;
    }

    if (channel >= kChannelCount) {
        CloseLocally(id, DisconnectReason::ProtocolViolation, HandshakeError::None, out);
        return;
    }
    TransportEvent ev;
    ev.type = TransportEvent::Type::Message;
    ev.peer = id;
    ev.channel = static_cast<Channel>(channel);
    ev.payload.assign(data.begin(), data.end());
    out.push_back(std::move(ev));
}

void Transport::Poll(std::vector<TransportEvent>& out) {
    for (TransportEvent& ev : mPending) {
        out.push_back(std::move(ev));
    }
    mPending.clear();

    ENetEvent event;
    while (enet_host_service(mHost, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                // Outgoing connections were tracked in Connect(); incoming ones are new.
                PeerId id = event.peer->data != nullptr ? IdOf(event.peer) : Track(event.peer);
                mPeers.at(id).connectedAt = std::chrono::steady_clock::now();
                SendHello(event.peer);
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                OnReceive(IdOf(event.peer), event.channelID, { event.packet->data, event.packet->dataLength }, out);
                enet_packet_destroy(event.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                PeerId id = IdOf(event.peer);
                auto it = mPeers.find(id);
                if (it == mPeers.end()) {
                    break;
                }
                if (!it->second.closing) {
                    TransportEvent ev;
                    ev.type = TransportEvent::Type::Disconnected;
                    ev.peer = id;
                    DecodeDisconnectData(event.data, ev.reason, ev.handshakeError);
                    out.push_back(std::move(ev));
                }
                event.peer->data = nullptr;
                mPeers.erase(it);
                break;
            }
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    // Connected at the ENet level but silent: drop it.
    auto now = std::chrono::steady_clock::now();
    for (auto& [id, state] : mPeers) {
        if (!state.ready && !state.closing && state.enet->state == ENET_PEER_STATE_CONNECTED &&
            now - state.connectedAt > mConfig.handshakeTimeout) {
            CloseLocally(id, DisconnectReason::HandshakeTimeout, HandshakeError::None, out);
        }
    }

    enet_host_flush(mHost);
}

bool Transport::Send(PeerId peer, Channel channel, std::span<const uint8_t> payload) {
    auto it = mPeers.find(peer);
    if (it == mPeers.end() || !it->second.ready || it->second.closing || payload.size() > kMaxMessageBytes) {
        return false;
    }
    ENetPacket* packet = enet_packet_create(payload.data(), payload.size(), PacketFlags(channel));
    if (packet == nullptr) {
        return false;
    }
    if (enet_peer_send(it->second.enet, static_cast<enet_uint8>(channel), packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

void Transport::Disconnect(PeerId peer) {
    if (mPeers.contains(peer)) {
        CloseLocally(peer, DisconnectReason::Normal, HandshakeError::None, mPending);
    }
}

uint16_t Transport::LocalPort() const {
    return PortOf(mHost);
}

size_t Transport::ReadyPeerCount() const {
    size_t n = 0;
    for (const auto& [id, state] : mPeers) {
        n += state.ready && !state.closing ? 1 : 0;
    }
    return n;
}

void Transport::Flush() {
    enet_host_flush(mHost);
}

uint32_t Transport::RoundTripMs(PeerId peer) const {
    auto it = mPeers.find(peer);
    return it == mPeers.end() ? 0 : it->second.enet->lastRoundTripTime;
}

uint64_t Transport::BytesSent() const {
    return mHost->totalSentData;
}

uint64_t Transport::BytesReceived() const {
    return mHost->totalReceivedData;
}

void Transport::SetRawHandler(RawHandler handler) {
    auto shared = std::make_shared<RawHandler>(std::move(handler));
    std::lock_guard lock(RawMutex());
    gRawHandler = std::move(shared);
}

bool Transport::SendRaw(const UdpEndpoint& to, std::span<const uint8_t> data) {
    std::lock_guard lock(RawMutex());
    ENetHost* host = gJoinHost != nullptr ? gJoinHost : (gLiveHosts.empty() ? nullptr : gLiveHosts.back());
    return SendOn(host, to, data);
}

bool Transport::SendRawFrom(const UdpEndpoint& to, std::span<const uint8_t> data) {
    return SendOn(mHost, to, data);
}

uint16_t Transport::OpenJoinSocket() {
    std::lock_guard lock(RawMutex());
    if (gJoinHost == nullptr) {
        if (!AcquireEnet()) {
            return 0;
        }
        // bound right away (port 0 = any) so we know our port before sending anything
        ENetAddress any{};
        any.host = ENET_HOST_ANY;
        any.port = 0;
        gJoinHost = CreateHost(&any, 1);
        if (gJoinHost == nullptr) {
            ReleaseEnet();
            return 0;
        }
        enet_socket_set_option(gJoinHost->socket, ENET_SOCKOPT_BROADCAST, 1);
    }
    return PortOf(gJoinHost);
}

void Transport::CloseJoinSocket() {
    ENetHost* host = TakeJoinHost();
    if (host != nullptr) {
        enet_host_destroy(host);
        ReleaseEnet();
    }
}

bool Transport::JoinSocketOpen() {
    std::lock_guard lock(RawMutex());
    return gJoinHost != nullptr;
}

void Transport::PollJoinSocket() {
    // Read under the lock, hand out without it (the handler may answer with SendRaw()).
    struct Datagram {
        ENetAddress from;
        std::vector<uint8_t> data;
    };
    std::vector<Datagram> got;
    {
        std::lock_guard lock(RawMutex());
        if (gJoinHost == nullptr) {
            return;
        }
        uint8_t buffer[2048];
        // a cap so a flood can't freeze the frame
        for (int i = 0; i < 64; i++) {
            ENetBuffer view;
            view.data = buffer;
            view.dataLength = sizeof(buffer);
            ENetAddress from{};
            int length = enet_socket_receive(gJoinHost->socket, &from, &view, 1);
            if (length <= 0) {
                break; // 0 = nothing more, -1 = error (the next frame tries again)
            }
            got.push_back({ from, std::vector<uint8_t>(buffer, buffer + length) });
        }
    }
    for (const Datagram& d : got) {
        DeliverRaw(d.from, d.data);
    }
}

} // namespace Coop
