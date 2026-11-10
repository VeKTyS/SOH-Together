#include "soh/Network/Coop/Session.h"

#include <algorithm>
#include <utility>

namespace Coop {

std::string DescribeDisconnect(DisconnectReason reason, HandshakeError error) {
    switch (reason) {
        case DisconnectReason::Normal:
            return "Disconnected";
        case DisconnectReason::Refused:
            return std::string("Refused: ") + Describe(error);
        case DisconnectReason::ProtocolViolation:
            return "Refused: the other side is not a SOH-Together game";
        case DisconnectReason::HandshakeTimeout:
            return "The other side did not answer the handshake";
        case DisconnectReason::ConnectionLost:
            return "Connection lost (host unreachable, firewall or timeout)";
    }
    return "Disconnected";
}

const char* TeleportBlocker(const PlayerState& me, const PlayerState& them, int32_t entranceCount) {
    if (!me.inGame) {
        return "Load your save first";
    }
    if (!them.inGame) {
        return "They're not in game";
    }
    if (them.entranceIndex < 0 || them.entranceIndex >= entranceCount || them.roomNum < 0) {
        return "Not sure where they are yet, try again in a sec";
    }
    return nullptr;
}

bool ParseAddress(const std::string& text, std::string& host, uint16_t& port) {
    size_t begin = text.find_first_not_of(" \t");
    size_t end = text.find_last_not_of(" \t");
    if (begin == std::string::npos) {
        return false;
    }
    std::string trimmed = text.substr(begin, end - begin + 1);

    size_t colon = trimmed.rfind(':');
    host = trimmed.substr(0, colon);
    port = kDefaultPort;
    if (colon != std::string::npos) {
        std::string digits = trimmed.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        unsigned long value = std::stoul(digits);
        if (value == 0 || value > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(value);
    }
    return !host.empty() && host.find_first_of(" \t:") == std::string::npos;
}

Session::Session(Logger logger) : mLogger(std::move(logger)) {
}

Session::~Session() {
    // At process exit the logging backend may already be destroyed.
    mLogger = nullptr;
    Stop();
}

void Session::Log(const std::string& line) const {
    if (mLogger) {
        mLogger(line);
    }
}

bool Session::Host(uint16_t port, const Hello& local) {
    Stop();
    TransportConfig config;
    config.localHello = local;
    config.maxPeers = kMaxPlayers - 1; // 2 to 16 players like Crests of Courage
    mTransport = Transport::Listen(port, config);
    if (!mTransport) {
        mLastError = "Cannot open UDP port " + std::to_string(port) + " (already in use?)";
        Log(mLastError);
        return false;
    }
    mRole = SessionRole::Hosting;
    mSelfId = kHostPlayerId;
    mLastError.clear();
    Log("Hosting on UDP port " + std::to_string(mTransport->LocalPort()));
    return true;
}

bool Session::Join(const std::string& host, uint16_t port, const Hello& local) {
    Stop();
    TransportConfig config;
    config.localHello = local;
    mTransport = Transport::Connect(host, port, config);
    if (!mTransport) {
        mLastError = "Invalid address: " + host;
        Log(mLastError);
        return false;
    }
    mRole = SessionRole::Connecting;
    mLastError.clear();
    Log("Connecting to " + host + ":" + std::to_string(port));
    return true;
}

void Session::Stop() {
    if (mTransport) {
        Log("Session stopped");
    }
    mTransport.reset(); // the destructor notifies every peer
    mPlayers.clear();
    mRole = SessionRole::Off;
    mFlagChangesIn.clear();
    mFlagSnapshotIn.reset();
    mSnapshotRequestsIn.clear();
    mEnemyKillsIn.clear();
    mEnemyStatesIn.reset();
    mEnemyHitsIn.clear();
    mItemsIn.clear();
    mDungeonItemsIn.clear();
    mThingsIn.clear();
    mEventsIn.clear();
    mObjectStatesIn.reset();
    mSelfId = 0;
    mNextId = kHostPlayerId + 1;
}

uint16_t Session::Port() const {
    return mRole == SessionRole::Hosting && mTransport ? mTransport->LocalPort() : 0;
}

uint32_t Session::RoundTripMs(const RemotePlayer& player) const {
    return mTransport && !player.relayed ? mTransport->RoundTripMs(player.peer) : 0;
}

uint64_t Session::BytesSent() const {
    return mTransport ? mTransport->BytesSent() : 0;
}

uint64_t Session::BytesReceived() const {
    return mTransport ? mTransport->BytesReceived() : 0;
}

RemotePlayer* Session::Find(PeerId peer) {
    auto it = std::find_if(mPlayers.begin(), mPlayers.end(), [&](const RemotePlayer& p) { return p.peer == peer; });
    return it == mPlayers.end() ? nullptr : &*it;
}

const RemotePlayer* Session::FindById(uint32_t id) const {
    auto it = std::find_if(mPlayers.begin(), mPlayers.end(), [&](const RemotePlayer& p) { return p.id == id; });
    return it == mPlayers.end() ? nullptr : &*it;
}

RemotePlayer& Session::RelayedPlayer(uint32_t id, const std::string& name) {
    for (RemotePlayer& p : mPlayers) {
        if (p.relayed && p.id == id) {
            p.name = name;
            return p;
        }
    }
    RemotePlayer p;
    p.peer = kRelayedPeerBase + id;
    p.id = id;
    p.relayed = true;
    p.name = name;
    Log("Player joined: " + name);
    mPlayers.push_back(std::move(p));
    return mPlayers.back();
}

void Session::HandleEvent(TransportEvent& ev) {
    switch (ev.type) {
        case TransportEvent::Type::Connected: {
            RemotePlayer player;
            player.peer = ev.peer;
            player.name = ev.remoteHello.playerName;
            if (mRole == SessionRole::Hosting) {
                player.id = mNextId++;
                SendTo(ev.peer, Channel::Ordered, EncodeWelcome(Welcome{ player.id }));
            } else {
                player.id = kHostPlayerId; // a client's only peer is the host
            }
            // host first in the list, relayed players after (RequestFlagSnapshot relies on it)
            mPlayers.insert(std::find_if(mPlayers.begin(), mPlayers.end(), [](const RemotePlayer& p) { return p.relayed; }),
                            std::move(player));
            if (mRole == SessionRole::Connecting) {
                mRole = SessionRole::Connected;
            }
            Log("Player joined: " + ev.remoteHello.playerName);
            break;
        }
        case TransportEvent::Type::Disconnected: {
            RemotePlayer* player = Find(ev.peer);
            std::string who = player != nullptr ? player->name : "peer";
            std::string why = DescribeDisconnect(ev.reason, ev.handshakeError);
            Log(who + " left: " + why);
            if (mRole == SessionRole::Hosting && player != nullptr) {
                std::vector<uint8_t> left = EncodePeerLeft(PeerLeft{ player->id });
                RelayToOthers(ev.peer, left);
            }
            std::erase_if(mPlayers, [&](const RemotePlayer& p) { return p.peer == ev.peer; });
            // A client only has one peer, the host: losing it ends the session.
            if (mRole == SessionRole::Connecting || mRole == SessionRole::Connected) {
                mLastError = why;
                mTransport.reset();
                mRole = SessionRole::Off;
            }
            break;
        }
        case TransportEvent::Type::Message: {
            if (RemotePlayer* player = Find(ev.peer)) {
                mInChannel = ev.channel;
                HandleMessage(*player, ev.payload);
            }
            break;
        }
    }
}

void Session::HandleMessage(RemotePlayer& from, std::span<const uint8_t> payload) {
    switch (PeekType(payload).value_or(static_cast<MessageType>(0))) {
        case MessageType::PlayerState:
            if (std::optional<PlayerState> state = DecodePlayerState(payload)) {
                from.state = *state;
                from.lastStateAt = std::chrono::steady_clock::now();
                if (!state->inGame) {
                    from.pose.reset(); // back on the title screen: nothing to draw
                }
                Forward(from, payload);
            }
            break;
        case MessageType::PlayerPose:
            if (std::optional<PlayerPose> pose = DecodePlayerPose(payload)) {
                from.pose = *pose;
                Forward(from, payload);
            }
            break;
        case MessageType::Welcome:
            if (mRole == SessionRole::Connected && !from.relayed) {
                if (std::optional<Welcome> w = DecodeWelcome(payload)) {
                    mSelfId = w->playerId;
                }
            }
            break;
        case MessageType::Forwarded:
            // host -> us: another client's state / pose
            if (mRole == SessionRole::Connected && !from.relayed) {
                if (std::optional<Forwarded> f = DecodeForwarded(payload)) {
                    MessageType inner = PeekType(f->inner).value_or(static_cast<MessageType>(0));
                    if (f->origin != mSelfId && f->origin != kHostPlayerId &&
                        (inner == MessageType::PlayerState || inner == MessageType::PlayerPose)) {
                        HandleMessage(RelayedPlayer(f->origin, f->name), f->inner);
                    }
                }
            }
            break;
        case MessageType::PeerLeft:
            if (mRole == SessionRole::Connected && !from.relayed) {
                if (std::optional<PeerLeft> left = DecodePeerLeft(payload)) {
                    for (const RemotePlayer& p : mPlayers) {
                        if (p.relayed && p.id == left->playerId) {
                            Log(p.name + " left");
                        }
                    }
                    std::erase_if(mPlayers,
                                  [&](const RemotePlayer& p) { return p.relayed && p.id == left->playerId; });
                }
            }
            break;
        case MessageType::Event:
            if (std::optional<Event> e = DecodeEvent(payload)) {
                if (mRole == SessionRole::Hosting) {
                    e->origin = from.id; // the host knows who sent it, don't trust the packet
                    RelayToOthers(from.peer, EncodeEvent(*e), mInChannel);
                }
                mEventsIn.push_back(std::move(*e));
            }
            break;
        case MessageType::FlagChange:
            if (std::optional<FlagChange> change = DecodeFlagChange(payload)) {
                mFlagChangesIn.push_back(*change);
                RelayToOthers(from.peer, payload);
            }
            break;
        case MessageType::EnemyKilled:
            if (std::optional<EnemyKill> kill = DecodeEnemyKill(payload)) {
                mEnemyKillsIn.push_back(*kill);
                RelayToOthers(from.peer, payload);
            }
            break;
        case MessageType::EnemyStates:
            // only the host runs the enemies
            if (mRole == SessionRole::Connected) {
                if (std::optional<EnemyStates> states = DecodeEnemyStates(payload)) {
                    mEnemyStatesIn = std::move(*states);
                }
            }
            break;
        case MessageType::ItemGiven:
            if (std::optional<ItemGiven> item = DecodeItemGiven(payload)) {
                mItemsIn.push_back(*item);
                RelayToOthers(from.peer, payload);
            }
            break;
        case MessageType::DungeonItems:
            if (std::optional<DungeonItems> d = DecodeDungeonItems(payload)) {
                mDungeonItemsIn.push_back(*d);
                RelayToOthers(from.peer, payload);
            }
            break;
        case MessageType::ThingStates:
            if (std::optional<ThingStates> things = DecodeThingStates(payload)) {
                auto same = [&](const ThingStates& t) { return t.owner == things->owner; };
                std::erase_if(mThingsIn, same);
                mThingsIn.push_back(std::move(*things));
                RelayToOthers(from.peer, payload, Channel::Snapshot);
            }
            break;
        case MessageType::ObjectStates:
            // only the host runs the platforms (same as enemies)
            if (mRole == SessionRole::Connected && !from.relayed) {
                if (std::optional<ObjectStates> objects = DecodeObjectStates(payload)) {
                    mObjectStatesIn = std::move(*objects);
                }
            }
            break;
        case MessageType::EnemyHit:
            if (std::optional<EnemyHit> hit = DecodeEnemyHit(payload)) {
                mEnemyHitsIn.push_back(*hit);
                RelayToOthers(from.peer, payload);
            }
            break;
        case MessageType::FlagSnapshotRequest:
            if (mRole == SessionRole::Hosting) {
                mSnapshotRequestsIn.push_back(from.peer);
            }
            break;
        case MessageType::FlagSnapshot:
            // only the host's flags count
            if (mRole == SessionRole::Connected) {
                if (std::optional<FlagSnapshot> snapshot = DecodeFlagSnapshot(payload)) {
                    // the snapshot already contains what arrived before it: don't apply those twice
                    mFlagChangesIn.clear();
                    mItemsIn.clear();
                    mDungeonItemsIn.clear();
                    mEventsIn.clear();
                    mFlagSnapshotIn = std::move(*snapshot);
                }
            }
            break;
        default:
            break; // unknown: ignore (shouldn't happen within one protocol version)
    }
}

void Session::Forward(const RemotePlayer& from, std::span<const uint8_t> payload) {
    if (mRole != SessionRole::Hosting || from.relayed) {
        return;
    }
    Forwarded f;
    f.origin = from.id;
    f.name = from.name;
    f.inner.assign(payload.begin(), payload.end());
    RelayToOthers(from.peer, EncodeForwarded(f), Channel::Snapshot);
}

void Session::SendEvent(EventKind kind, std::vector<uint8_t> data, Channel channel) {
    if (data.size() > kMaxEventData || mSelfId == 0) {
        return;
    }
    Event e;
    e.origin = mSelfId;
    e.kind = kind;
    e.data = std::move(data);
    SendToAll(channel, EncodeEvent(e));
}

std::vector<Event> Session::TakeEvents() {
    return std::exchange(mEventsIn, {});
}

void Session::SendTo(PeerId peer, Channel channel, std::span<const uint8_t> bytes) {
    if (peer >= kRelayedPeerBase) {
        return; // a relayed player has no connection of its own: the host passes things on
    }
    if (mTransport) {
        mTransport->Send(peer, channel, bytes);
    }
}

void Session::RelayToOthers(PeerId from, std::span<const uint8_t> payload, Channel channel) {
    if (mRole != SessionRole::Hosting) {
        return;
    }
    for (const RemotePlayer& other : mPlayers) {
        if (other.peer != from && !other.relayed) {
            SendTo(other.peer, channel, payload);
        }
    }
}

void Session::SendToAll(Channel channel, std::span<const uint8_t> bytes) {
    for (const RemotePlayer& p : mPlayers) {
        SendTo(p.peer, channel, bytes);
    }
}

void Session::SendFlagChange(const FlagChange& change) {
    SendToAll(Channel::Ordered, EncodeFlagChange(change));
}

void Session::SendEnemyKill(const EnemyKill& kill) {
    SendToAll(Channel::Ordered, EncodeEnemyKill(kill));
}

std::vector<EnemyKill> Session::TakeEnemyKills() {
    return std::exchange(mEnemyKillsIn, {});
}

void Session::SendEnemyStates(const EnemyStates& states) {
    if (mRole == SessionRole::Hosting) {
        SendToAll(Channel::Snapshot, EncodeEnemyStates(states));
    }
}

std::optional<EnemyStates> Session::TakeEnemyStates() {
    return std::exchange(mEnemyStatesIn, std::nullopt);
}

void Session::SendEnemyHit(const EnemyHit& hit) {
    SendToAll(Channel::Ordered, EncodeEnemyHit(hit));
}

std::vector<EnemyHit> Session::TakeEnemyHits() {
    return std::exchange(mEnemyHitsIn, {});
}

void Session::SendItemGiven(const ItemGiven& item) {
    SendToAll(Channel::Ordered, EncodeItemGiven(item));
}

std::vector<ItemGiven> Session::TakeItemsGiven() {
    return std::exchange(mItemsIn, {});
}

void Session::SendDungeonItems(const DungeonItems& items) {
    SendToAll(Channel::Ordered, EncodeDungeonItems(items));
}

std::vector<DungeonItems> Session::TakeDungeonItems() {
    return std::exchange(mDungeonItemsIn, {});
}

void Session::SendThingStates(const ThingStates& states) {
    SendToAll(Channel::Snapshot, EncodeThingStates(states));
}

std::vector<ThingStates> Session::TakeThingStates() {
    return std::exchange(mThingsIn, {});
}

void Session::RequestFlagSnapshot() {
    if (mRole == SessionRole::Connected && !mPlayers.empty()) {
        SendTo(mPlayers[0].peer, Channel::Ordered, EncodeFlagSnapshotRequest());
    }
}

void Session::SendFlagSnapshot(PeerId peer, const FlagSnapshot& snapshot) {
    if (mRole == SessionRole::Hosting) {
        SendTo(peer, Channel::Ordered, EncodeFlagSnapshot(snapshot));
    }
}

std::vector<FlagChange> Session::TakeFlagChanges() {
    return std::exchange(mFlagChangesIn, {});
}

std::optional<FlagSnapshot> Session::TakeFlagSnapshot() {
    return std::exchange(mFlagSnapshotIn, std::nullopt);
}

std::vector<PeerId> Session::TakeSnapshotRequests() {
    return std::exchange(mSnapshotRequestsIn, {});
}

void Session::Tick(const PlayerState& local, const PlayerPose* pose) {
    if (!mTransport) {
        return;
    }
    mEvents.clear();
    mTransport->Poll(mEvents);
    for (TransportEvent& ev : mEvents) {
        HandleEvent(ev);
        if (!mTransport) {
            return; // the session ended while handling this event
        }
    }

    std::vector<uint8_t> bytes = EncodePlayerState(local);
    std::vector<uint8_t> poseBytes;
    if (pose != nullptr) {
        poseBytes = EncodePlayerPose(*pose);
    }
    for (RemotePlayer& player : mPlayers) {
        if (player.relayed) {
            continue;
        }
        mTransport->Send(player.peer, Channel::Snapshot, bytes);
        if (!poseBytes.empty()) {
            mTransport->Send(player.peer, Channel::Snapshot, poseBytes);
        }
        player.pingMs = mTransport->RoundTripMs(player.peer);
    }
    // Without this, this frame's state would wait for next frame's Poll(): +50 ms.
    mTransport->Flush();
}

void Session::SendObjectStates(const ObjectStates& states) {
    if (mRole == SessionRole::Hosting) {
        SendToAll(Channel::Snapshot, EncodeObjectStates(states));
    }
}

std::optional<ObjectStates> Session::TakeObjectStates() {
    return std::exchange(mObjectStatesIn, std::nullopt);
}

} // namespace Coop
