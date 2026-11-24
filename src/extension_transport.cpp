#include "direct_net.h"
#include "extension_limits.h"
#include "log.h"
#include "netclass.h"

#include <algorithm>
#include <cstring>
#include <chrono>

namespace dn {
namespace {
std::atomic<uint64_t> nextGeneration{1}; // unique across retired/replaced DirectNet instances
constexpr uint64_t kRoomLeaseMs = 2000;
constexpr uint64_t kLinkFreshMs = 3000;
// After an overflow, readiness returns once every participant link's real extension backlog (its own ordered
// domain, Link::xtx: game traffic and state messages are not in it) is back under half of the extension
// ceiling: room to work again, not an instant refault.
constexpr size_t kResumeBytes = extension::kPendingBytes / 2;
constexpr size_t kResumePackets = ReliableSender::kMaxPendingPackets / 2;
constexpr uint8_t kStateClass = static_cast<uint8_t>(TrafficClass::State);

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
    extensionReliableQueued_ = 0;
    extensionStateQueued_.clear();
    // A new generation takes any state sequence again (a sender's sequence follows its clock: extensionSendState).
    extensionStateSeen_.clear();
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
        return link.xtx.pendingBytes() <= kResumeBytes && link.xtx.pendingCount() <= kResumePackets;
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
    // Refusal must not leave a gap in the link's extension ordered sequence space (Link::xtx).
    Message wire;
    wire.type = MsgType::Data;
    wire.epoch = link.epoch;
    wire.data = msg;
    const size_t bytes = encode(wire, opt_.key).size();
    if (bytes > bytesLimit || link.xtx.pendingBytes() > bytesLimit - bytes ||
        link.xtx.pendingCount() >= packetsLimit) {
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
    // On each link the extension has an ordered domain of its own (Link::xtx): game packets never hold it back.
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

// State goes over the same stable path as reliable messages, unnumbered (reliability 0, class State): it is
// never tracked, retransmitted or upgraded, and is not in any backlog. The payload carries the key and a
// sequence ahead of the data. The sequence follows the sender's steady clock in microseconds (or one above
// the last, if the clock has not moved), so a sender whose process restarted still counts above everything
// it sent before and the receiver never holds it back.
bool DirectNet::extensionSendState(uint64_t generation, const std::string& peer, uint32_t key, const uint8_t* data,
                                   uint32_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!data || !bytes || bytes > EDF6COOP_EXTENSION_MAX_PAYLOAD || !refreshExtensionLocked() ||
        generation != extensionGeneration_ || !std::binary_search(extensionPeers_.begin(), extensionPeers_.end(), peer))
        return false;
    Link& link = opt_.mode == Mode::Host ? clients_.at(peer) : *hostLink_;
    const auto clock = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    extensionStateSeq_ = std::max(extensionStateSeq_ + 1, clock);
    DataMsg msg;
    msg.src = localPuid_;
    msg.dst = peer;
    msg.socketName = kExtensionSocket;
    msg.channel = kExtensionChannel;
    msg.reliability = 0;
    msg.cls = kStateClass;
    msg.payload.resize(kExtensionStateHeader + bytes);
    memcpy(msg.payload.data(), &key, 4);
    memcpy(msg.payload.data() + 4, &extensionStateSeq_, 8);
    memcpy(msg.payload.data() + kExtensionStateHeader, data, bytes);
    const uint64_t now = extensionNowMs();
    // Over the link's state budget it is dropped like a lost datagram: the next one replaces it.
    if (opt_.shedState && !takeStateBudget(link, msg.payload.size(), now)) {
        ++stateShed_;
        return true;
    }
    sendData(link, std::move(msg), now);
    return true;
}

void DirectNet::popExtensionFrontLocked() {
    const Delivered& front = extensionInbox_.front();
    if (front.state) extensionStateQueued_.erase({front.src, front.key});
    else --extensionReliableQueued_;
    extensionInbox_.pop_front();
}

bool DirectNet::extensionPoll(uint64_t generation, uint32_t capacity, Delivered& out, uint32_t* peerIndex) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!refreshExtensionLocked() || generation != extensionGeneration_ || extensionInbox_.empty() ||
        extensionInbox_.front().data.size() > capacity) return false;
    // Queued senders are always current peers: any peer change resets the inbox with the generation.
    const auto peer = std::lower_bound(extensionPeers_.begin(), extensionPeers_.end(), extensionInbox_.front().src);
    if (peerIndex) *peerIndex = static_cast<uint32_t>(peer - extensionPeers_.begin());
    out = extensionInbox_.front();
    popExtensionFrontLocked();
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
        if (peer == extensionPeers_.end() || *peer != next.src) { popExtensionFrontLocked(); continue; }
        memcpy(buffer + used, next.data.data(), size);
        messages[count] = {static_cast<uint32_t>(peer - extensionPeers_.begin()), used, size,
                           next.state ? EDF6COOP_EXTENSION_MESSAGE_STATE : 0u, next.state ? next.key : 0u};
        used += size;
        ++count;
        popExtensionFrontLocked();
    }
    return true;
}

// True means reserved for extensions, including malformed packets. Nothing in
// this namespace may reach the game, even when an extension is absent/not ready.
bool DirectNet::extensionPacket(const DataMsg& msg) {
    if (msg.socketName != kExtensionSocket && msg.channel != kExtensionChannel) return false;
    // Reliable ordered (numbered in the link's extension domain), or state (unnumbered, class State, key and
    // sequence ahead of the data). Anything else in the namespace is swallowed.
    const bool ordered = msg.reliability == 2 && msg.seq && !msg.cls && !msg.payload.empty() &&
                         msg.payload.size() <= EDF6COOP_EXTENSION_MAX_PAYLOAD;
    const bool state = msg.reliability == 0 && !msg.seq && msg.cls == kStateClass &&
                       msg.payload.size() > kExtensionStateHeader &&
                       msg.payload.size() <= kExtensionStateHeader + EDF6COOP_EXTENSION_MAX_PAYLOAD;
    if (msg.socketName != kExtensionSocket || msg.channel != kExtensionChannel || (!ordered && !state) ||
        !refreshExtensionLocked() ||
        !std::binary_search(extensionPeers_.begin(), extensionPeers_.end(), msg.src) ||
        !std::binary_search(extensionMembers_.begin(), extensionMembers_.end(), msg.dst)) return true;
    if (msg.dst != localPuid_) {
        if (opt_.mode != Mode::Host || msg.src == msg.dst) return true;
        auto it = clients_.find(msg.dst);
        if (it == clients_.end() || !usable(it->second)) return true;
        const uint64_t now = extensionNowMs();
        if (ordered) {
            sendExtensionDataLocked(it->second, msg, now);
        } else if (takeRelayBudget(msg.payload.size(), now)) {
            // State is relayed as it came, unnumbered, within the host's relay budget (the next one replaces it).
            relayed_ += msg.payload.size();
            sendData(it->second, msg, now);
        }
        return true;
    }
    if (ordered) {
        if (extensionReliableQueued_ >= extension::kInboxMessages) {
            faultExtensionLocked("inbox full"); // no silent truncation of a reliable transaction
            return true;
        }
        extensionInbox_.push_back({msg.src, msg.socketName, msg.channel, msg.payload, msg.cls});
        ++extensionReliableQueued_;
        return true;
    }
    uint32_t key = 0;
    uint64_t seq = 0;
    memcpy(&key, msg.payload.data(), 4);
    memcpy(&seq, msg.payload.data() + 4, 8);
    const auto id = std::make_pair(msg.src, key);
    if (const auto seen = extensionStateSeen_.find(id); seen != extensionStateSeen_.end()) {
        if (seq <= seen->second) return true; // older than (or a copy of) one already taken
        seen->second = seq;
    } else if (extensionStateSeen_.size() >= EDF6COOP_EXTENSION_MAX_STATE_KEYS) {
        logRateLimited("ext-state-keys", 10000, "DIRECT extension state %s key %u dropped: %u (sender, key) pairs "
                       "tracked already", shortId(msg.src).c_str(), key, EDF6COOP_EXTENSION_MAX_STATE_KEYS);
        return true;
    } else {
        extensionStateSeen_.emplace(id, seq);
    }
    std::vector<uint8_t> data(msg.payload.begin() + kExtensionStateHeader, msg.payload.end());
    if (const auto queued = extensionStateQueued_.find(id); queued != extensionStateQueued_.end()) {
        queued->second->data = std::move(data); // the unread one is replaced in place: only the newest is kept
        return true;
    }
    Delivered d;
    d.src = msg.src;
    d.socketName = msg.socketName;
    d.channel = msg.channel;
    d.data = std::move(data);
    d.cls = msg.cls;
    d.state = true;
    d.key = key;
    extensionInbox_.push_back(std::move(d));
    extensionStateQueued_.emplace(id, &extensionInbox_.back());
    return true;
}
} // namespace dn
