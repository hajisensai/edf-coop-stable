#include "direct_net.h"
#include "extension_limits.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <chrono>

namespace dn {
namespace {
std::atomic<uint64_t> nextGeneration{1}; // unique across retired/replaced DirectNet instances
constexpr uint64_t kRoomLeaseMs = 2000;
constexpr uint64_t kLinkFreshMs = 3000;
// After an overflow, readiness returns once every participant link's real backlog (game traffic
// included) is back under half of the extension ceiling: room to work again, not an instant refault.
constexpr size_t kResumeBytes = extension::kPendingBytes / 2;
constexpr size_t kResumePackets = ReliableSender::kMaxPendingPackets / 2;

uint64_t extensionNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool validId(const std::string& id) { return !id.empty() && id.size() < sizeof(EDF6CoopPeer::id); }
void copyPeer(EDF6CoopPeer& out, const std::string& id) {
    out = {};
    if (validId(id)) memcpy(out.id, id.data(), id.size());
}
}

void DirectNet::resetExtensionLocked() {
    extensionInbox_.clear();
    extensionReady_ = false;
    extensionLinks_.clear();
    extensionGeneration_ = nextGeneration.fetch_add(1);
}

// An overflow (legacy send/relay backlog, or the inbox) cancels this generation: queued messages are
// dropped and every consumer sees a new generation, so no half of an acknowledged transaction is
// delivered silently. It is not a room boundary: once drained, the same room is ready again with a
// fresh generation (consumers renegotiate their own epochs, as after any other readiness blip).
void DirectNet::faultExtensionLocked(const char* why) {
    extensionFault_ = true;
    ++extensionFaults_;
    resetExtensionLocked();
    // A consumer that stops polling refills the inbox right after each recovery: one line per 10 s, with
    // the count of the faults it stands for, instead of one per cycle.
    const uint64_t now = extensionNowMs();
    if (extensionFaultLogMs_ && now - extensionFaultLogMs_ < 10000) { ++extensionFaultsUnlogged_; return; }
    logf("DIRECT extension %s: generation %llu renewed (fault %llu, %llu more since the last line); ready again "
        "once the backlog drains", why, static_cast<unsigned long long>(extensionGeneration_),
        static_cast<unsigned long long>(extensionFaults_), static_cast<unsigned long long>(extensionFaultsUnlogged_));
    extensionFaultLogMs_ = now ? now : 1;
    extensionFaultsUnlogged_ = 0;
}

bool DirectNet::extensionDrainedLocked() const {
    auto drained = [](const Link& link) {
        return link.tx.pendingBytes() <= kResumeBytes && link.tx.pendingCount() <= kResumePackets;
    };
    if (opt_.mode == Mode::Host) {
        for (const auto& id : extensionPeers_) {
            const auto link = clients_.find(id);
            if (link != clients_.end() && !drained(link->second)) return false;
        }
        return true;
    }
    return !hostLink_ || drained(*hostLink_);
}

void DirectNet::setExtensionRoom(std::string room, std::string host, std::vector<std::string> members, uint64_t worldEpoch) {
    std::lock_guard<std::mutex> lock(mu_);
    std::sort(members.begin(), members.end());
    if (room != extensionRoom_ || host != extensionHost_ || members != extensionMembers_ || worldEpoch != extensionWorldEpoch_) {
        resetExtensionLocked();
        extensionFault_ = false;
        extensionRoom_ = std::move(room);
        extensionHost_ = std::move(host);
        extensionWorldEpoch_ = worldEpoch;
        extensionMembers_ = std::move(members);
        extensionPeers_ = extensionMembers_;
        std::erase(extensionPeers_, localPuid_);
    }
    extensionUpdatedMs_ = extensionNowMs();
    refreshExtensionLocked();
}

void DirectNet::clearExtensionRoom() {
    std::lock_guard<std::mutex> lock(mu_);
    resetExtensionLocked();
    extensionRoom_.clear();
    extensionHost_.clear();
    extensionWorldEpoch_ = 0;
    extensionMembers_.clear();
    extensionPeers_.clear();
    extensionFault_ = false;
    extensionUpdatedMs_ = 0;
}

bool DirectNet::extensionParticipant(const std::string& peer) {
    std::lock_guard<std::mutex> lock(mu_);
    return !extensionRoom_.empty() && std::binary_search(extensionMembers_.begin(), extensionMembers_.end(), peer);
}

// Called with mu_ held, including in send/poll and authenticated receive. No stale
// snapshot can authorize a send after a link disappeared/reconnected or its key changed.
bool DirectNet::refreshExtensionLocked() {
    const uint64_t now = extensionNowMs();
    if (extensionFault_ && extensionDrainedLocked()) {
        extensionFault_ = false;
        if (!extensionFaultsUnlogged_)
            logf("DIRECT extension backlog drained after fault %llu: the room may become ready again",
                static_cast<unsigned long long>(extensionFaults_));
    }
    bool ready = running_ && active_ && !extensionFault_ && !extensionRoom_.empty() && extensionWorldEpoch_ &&
        validId(localPuid_) && validId(extensionHost_) && extensionUpdatedMs_ &&
        now - extensionUpdatedMs_ <= kRoomLeaseMs &&
        extensionPeers_.size() <= EDF6COOP_EXTENSION_MAX_PEERS &&
        std::binary_search(extensionMembers_.begin(), extensionMembers_.end(), localPuid_) &&
        std::binary_search(extensionMembers_.begin(), extensionMembers_.end(), extensionHost_) &&
        std::adjacent_find(extensionMembers_.begin(), extensionMembers_.end()) == extensionMembers_.end();
    for (const auto& id : extensionMembers_) ready = ready && validId(id);
    std::vector<uint64_t> links;
    auto live = [&](const Link& link) {
        // Backlog is flow control, not an identity/session boundary. Admission
        // measures the actual queue atomically; congestion must not reset leases.
        return usable(link) && now - link.lastRecvMs <= kLinkFreshMs;
    };
    if (ready && opt_.mode == Mode::Host && extensionHost_ == localPuid_) {
        for (const auto& id : extensionPeers_) {
            const auto link = clients_.find(id);
            const auto identity = memberIds_.find(id);
            if (link == clients_.end() || !live(link->second) || identity == memberIds_.end() ||
                identity->second != link->second.identityCommitment) { ready = false; break; }
            links.push_back(link->second.id);
        }
    } else if (ready && opt_.mode == Mode::Join && extensionHost_ != localPuid_) {
        ready = hostLink_ && live(*hostLink_) && hostLink_->puid == extensionHost_ &&
            roomOwner_ == extensionHost_ && roomOwnerId_ == hostLink_->identityCommitment;
        if (ready) {
            auto roster = roster_;
            std::sort(roster.begin(), roster.end());
            // A lobby-only member may join/leave without changing this mission.
            // Every actual participant still needs a currently authenticated route.
            ready = std::includes(roster.begin(), roster.end(), extensionMembers_.begin(), extensionMembers_.end());
            links = {hostLink_->id};
        }
    } else ready = false;
    if (ready != extensionReady_ || (ready && links != extensionLinks_)) {
        resetExtensionLocked();
        extensionReady_ = ready;
        extensionLinks_ = std::move(links);
    }
    if (!extensionGeneration_) extensionGeneration_ = nextGeneration.fetch_add(1);
    return ready;
}

void DirectNet::extensionSnapshot(EDF6CoopSnapshot& out) {
    std::lock_guard<std::mutex> lock(mu_);
    out = {};
    out.size = sizeof(out);
    out.ready = refreshExtensionLocked() ? 1u : 0u;
    out.generation = extensionGeneration_;
    out.isHost = !extensionHost_.empty() && localPuid_ == extensionHost_ ? 1u : 0u;
    out.peerCount = out.ready ? static_cast<uint32_t>(extensionPeers_.size()) : 0;
    copyPeer(out.local, localPuid_);
    copyPeer(out.host, extensionHost_);
}

bool DirectNet::extensionPeer(uint64_t generation, uint32_t index, EDF6CoopPeer& out) {
    std::lock_guard<std::mutex> lock(mu_);
    out = {};
    if (!refreshExtensionLocked() || generation != extensionGeneration_ || index >= extensionPeers_.size()) return false;
    copyPeer(out, extensionPeers_[index]);
    return true;
}

bool DirectNet::sendExtensionDataLocked(Link& link, DataMsg msg, uint64_t now) {
    return admitExtensionDataLocked(link, std::move(msg), now, extension::kPendingBytes,
        static_cast<uint32_t>(ReliableSender::kMaxPendingPackets), true) == EDF6COOP_EXTENSION_SEND_ACCEPTED;
}

uint32_t DirectNet::admitExtensionDataLocked(Link& link, DataMsg msg, uint64_t now,
                                          uint32_t bytesLimit, uint32_t packetsLimit, bool failClosed) {
    // Measure the same encoding sendData tracks, before it allocates a sequence.
    // Refusal must not leave a gap in the shared reliable ordered sequence space.
    Message wire;
    wire.type = MsgType::Data;
    wire.epoch = link.epoch;
    wire.data = msg;
    const size_t bytes = encode(wire, opt_.key).size();
    if (bytes > bytesLimit || link.tx.pendingBytes() > bytesLimit - bytes ||
        link.tx.pendingCount() >= packetsLimit) {
        if (!failClosed) return EDF6COOP_EXTENSION_SEND_WOULD_BLOCK;
        faultExtensionLocked(msg.src == localPuid_ ? "send backlog over the ceiling" : "relay backlog over the ceiling");
        return EDF6COOP_EXTENSION_SEND_UNAVAILABLE;
    }
    sendData(link, std::move(msg), now);
    return EDF6COOP_EXTENSION_SEND_ACCEPTED;
}

bool DirectNet::extensionSend(uint64_t generation, const std::string& peer, const uint8_t* data, uint32_t bytes) {
    return extensionSendWithLimits(generation, peer, data, bytes, extension::kPendingBytes,
        static_cast<uint32_t>(ReliableSender::kMaxPendingPackets), true) == EDF6COOP_EXTENSION_SEND_ACCEPTED;
}

uint32_t DirectNet::extensionTrySend(uint64_t generation, const std::string& peer, const uint8_t* data, uint32_t bytes,
                                   uint32_t bytesLimit, uint32_t packetsLimit) {
    return extensionSendWithLimits(generation, peer, data, bytes, bytesLimit, packetsLimit, false);
}

uint32_t DirectNet::extensionSendWithLimits(uint64_t generation, const std::string& peer, const uint8_t* data,
                                         uint32_t bytes, uint32_t bytesLimit, uint32_t packetsLimit, bool failClosed) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!data || !bytes || bytes > EDF6COOP_EXTENSION_MAX_PAYLOAD || !bytesLimit ||
        bytesLimit > extension::kPendingBytes || !packetsLimit || packetsLimit > ReliableSender::kMaxPendingPackets ||
        !refreshExtensionLocked() || generation != extensionGeneration_ ||
        !std::binary_search(extensionPeers_.begin(), extensionPeers_.end(), peer))
        return EDF6COOP_EXTENSION_SEND_UNAVAILABLE;
    // Always one stable ordered path. Joiner-to-joiner packets go through the host;
    // switching between mesh and relay would make two independent ordering domains.
    Link* link = opt_.mode == Mode::Host ? &clients_.at(peer) : &*hostLink_;
    DataMsg msg;
    msg.src = localPuid_;
    msg.dst = peer;
    msg.socketName = kExtensionSocket;
    msg.channel = kExtensionChannel;
    msg.reliability = 2; // EOS_PR_ReliableOrdered
    msg.payload.assign(data, data + bytes);
    return admitExtensionDataLocked(*link, std::move(msg), extensionNowMs(), bytesLimit, packetsLimit, failClosed);
}

bool DirectNet::extensionPoll(uint64_t generation, uint32_t capacity, Delivered& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!refreshExtensionLocked() || generation != extensionGeneration_ || extensionInbox_.empty() ||
        extensionInbox_.front().data.size() > capacity) return false;
    out = std::move(extensionInbox_.front());
    extensionInbox_.pop_front();
    return true;
}

bool DirectNet::extensionPollBatch(uint64_t generation, EDF6CoopExtensionMessage* messages, uint32_t maxMessages,
                                   uint8_t* buffer, uint32_t bufferBytes, uint32_t& count) {
    count = 0;
    std::lock_guard<std::mutex> lock(mu_);
    if (!messages || !buffer || !refreshExtensionLocked() || generation != extensionGeneration_) return false;
    uint32_t used = 0;
    while (count < maxMessages && !extensionInbox_.empty()) {
        const auto& next = extensionInbox_.front();
        const auto size = static_cast<uint32_t>(next.data.size());
        if (size > bufferBytes - used) break; // stays queued for the next batch
        // Queued senders are always current peers: any peer change resets the inbox with the generation.
        const auto peer = std::lower_bound(extensionPeers_.begin(), extensionPeers_.end(), next.src);
        if (peer == extensionPeers_.end() || *peer != next.src) { extensionInbox_.pop_front(); continue; }
        memcpy(buffer + used, next.data.data(), size);
        messages[count] = {static_cast<uint32_t>(peer - extensionPeers_.begin()), used, size, 0};
        used += size;
        ++count;
        extensionInbox_.pop_front();
    }
    return true;
}

// True means reserved for extensions, including malformed packets. Nothing in
// this namespace may reach the game, even when an extension is absent/not ready.
bool DirectNet::extensionPacket(const DataMsg& msg) {
    if (msg.socketName != kExtensionSocket && msg.channel != kExtensionChannel) return false;
    if (msg.socketName != kExtensionSocket || msg.channel != kExtensionChannel || msg.reliability != 2 ||
        !msg.seq || msg.cls || msg.payload.empty() || msg.payload.size() > EDF6COOP_EXTENSION_MAX_PAYLOAD ||
        !refreshExtensionLocked() ||
        !std::binary_search(extensionPeers_.begin(), extensionPeers_.end(), msg.src) ||
        !std::binary_search(extensionMembers_.begin(), extensionMembers_.end(), msg.dst)) return true;
    if (msg.dst != localPuid_) {
        if (opt_.mode != Mode::Host || msg.src == msg.dst) return true;
        auto it = clients_.find(msg.dst);
        if (it != clients_.end() && usable(it->second)) sendExtensionDataLocked(it->second, msg, extensionNowMs());
        return true;
    }
    if (extensionInbox_.size() >= extension::kInboxMessages) {
        faultExtensionLocked("inbox full"); // no silent truncation of a reliable transaction
        return true;
    }
    extensionInbox_.push_back({msg.src, msg.socketName, msg.channel, msg.payload, msg.cls});
    return true;
}
} // namespace dn
