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
#include <unistd.h>
#endif

#include <algorithm>
#include <mutex>

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

ENetHost* CreateHost(const ENetAddress* address, size_t maxPeers) {
    ENetHost* host = enet_host_create(address, maxPeers, kChannelCount, 0, 0);
    if (host != nullptr) {
        host->maximumPacketSize = kMaxMessageBytes;
    }
    return host;
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
    ENetHost* host = CreateHost(nullptr, 1);
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
}

Transport::~Transport() {
    for (auto& [id, state] : mPeers) {
        if (!state.closing) {
            enet_peer_disconnect_now(state.enet, EncodeDisconnectData(DisconnectReason::Normal, HandshakeError::None));
        }
    }
    enet_host_flush(mHost);
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
    ENetAddress address{};
    if (enet_socket_get_address(mHost->socket, &address) != 0) {
        return 0;
    }
    return address.port;
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

} // namespace Coop
