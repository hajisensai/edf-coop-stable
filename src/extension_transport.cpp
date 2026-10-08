#include "direct_net.h"

#include <algorithm>
#include <cstring>
#include <chrono>

namespace dn {
namespace {
std::atomic<uint64_t> nextGeneration{1}; // unique across retired/replaced DirectNet instances
constexpr uint64_t kRoomLeaseMs = 2000;
constexpr uint64_t kLinkFreshMs = 3000;
constexpr size_t kInboxLimit = 256;
constexpr size_t kPendingLimit = 256 * 1024;
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

void DirectNet::setExtensionRoom(std::string room, std::string host, std::vector<std::string> members) {
    std::lock_guard<std::mutex> lock(mu_);
    std::sort(members.begin(), members.end());
    if (room != extensionRoom_ || host != extensionHost_ || members != extensionMembers_) {
        resetExtensionLocked();
        extensionFault_ = false;
        extensionRoom_ = std::move(room);
        extensionHost_ = std::move(host);
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
    extensionMembers_.clear();
    extensionPeers_.clear();
    extensionFault_ = false;
    extensionUpdatedMs_ = 0;
}

// Called with mu_ held, including in send/poll and authenticated receive. No stale
// snapshot can authorize a send after a link disappeared/reconnected or its key changed.
bool DirectNet::refreshExtensionLocked() {
    const uint64_t now = extensionNowMs();
    bool ready = running_ && active_ && !extensionFault_ && !extensionRoom_.empty() &&
        validId(localPuid_) && validId(extensionHost_) && extensionUpdatedMs_ &&
        now - extensionUpdatedMs_ <= kRoomLeaseMs &&
        extensionPeers_.size() <= EDF6COOP_EXTENSION_MAX_PEERS &&
        std::binary_search(extensionMembers_.begin(), extensionMembers_.end(), localPuid_) &&
        std::binary_search(extensionMembers_.begin(), extensionMembers_.end(), extensionHost_) &&
        std::adjacent_find(extensionMembers_.begin(), extensionMembers_.end()) == extensionMembers_.end();
    for (const auto& id : extensionMembers_) ready = ready && validId(id);
    std::vector<uint64_t> links;
    auto live = [&](const Link& link) {
        return usable(link) && now - link.lastRecvMs <= kLinkFreshMs &&
            link.tx.pendingBytes() < kPendingLimit && !congested(link);
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
            ready = roster == extensionMembers_;
            links = {hostLink_->id, rosterPages_.applied};
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

bool DirectNet::extensionSend(uint64_t generation, const std::string& peer, const uint8_t* data, uint32_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!data || !bytes || bytes > EDF6COOP_EXTENSION_MAX_PAYLOAD || !refreshExtensionLocked() ||
        generation != extensionGeneration_ || !std::binary_search(extensionPeers_.begin(), extensionPeers_.end(), peer))
        return false;
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
    sendData(*link, std::move(msg), extensionNowMs());
    return true;
}

bool DirectNet::extensionPoll(uint64_t generation, uint32_t capacity, Delivered& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!refreshExtensionLocked() || generation != extensionGeneration_ || extensionInbox_.empty() ||
        extensionInbox_.front().data.size() > capacity) return false;
    out = std::move(extensionInbox_.front());
    extensionInbox_.pop_front();
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
        if (it != clients_.end() && usable(it->second)) sendData(it->second, msg, extensionNowMs());
        return true;
    }
    if (extensionInbox_.size() >= kInboxLimit) {
        extensionFault_ = true; // no silent truncation of a reliable transaction
        resetExtensionLocked();
        return true;
    }
    extensionInbox_.push_back({msg.src, msg.socketName, msg.channel, msg.payload, msg.cls});
    return true;
}
} // namespace dn
