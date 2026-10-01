#include "reliable.h"

#include <algorithm>
#include <cmath>

namespace dn {
namespace {

// Real games showed a 60 ms floor retransmitting nearly every packet on 45-65 ms links (RTO about the RTT):
// the ACK only has to come back a little late. Game data was sent unreliably by the game itself, so a lost
// packet arriving some tens of ms later costs little, while a spurious resend doubles the traffic.
constexpr uint32_t kMinRtoMs = 150;
constexpr uint32_t kMaxRtoMs = 1000;
// RFC 6298: RTO = SRTT + max(G, 4 * RTTVAR), G the clock granularity (nowMs() is in milliseconds; a few ms
// of scheduling jitter as well).
constexpr double kClockGranularityMs = 5.0;
constexpr size_t kMaxReorderBuffer = 4096;
// Retransmissions follow what the link delivers (packet conservation, as TCP's ACK clock): every packet the
// peer acknowledges earns this many resends. Above 1 so a link recovering from an outage doubles its resend
// rate every round trip instead of draining a backlog at the probe rate; still bounded by delivery, so a
// link the network drops most packets of resends little. A real game in 2026-10 resent 2000 packets/s per
// link (the old fixed budget) into an uplink already losing nearly all of them, until EOS itself starved.
constexpr double kRetransmitsPerAck = 2.0;
// Credit a link earns without any acknowledgement: probes, so a link whose acknowledgements all got lost
// (an outage) still resends its oldest packets and finds out when it is back.
constexpr double kProbeRetransmitsPerSecond = 10.0;
constexpr double kRetransmitBurst = 64.0;
// The shared cap of all links of a process: 256 KB/s, 2 Mbit/s. EDF6 sends well under 1 Mbit/s per player
// in all, so this repairs heavy loss on every link at once while a host's resends can never be what fills
// its uplink (the 2026-10 game above reached 12-19 Mbit/s).
constexpr double kRetransmitBytesPerSecond = 256.0 * 1024;
constexpr double kRetransmitBytesBurst = 64.0 * 1024;
// Unacknowledged data one link may hold. EDF6 sends well under 1 Mbit/s per player, so a link
// stalled for the whole link timeout (60 s) stays far below this; only a peer that receives but
// never acknowledges (or bounces data back to itself through the host) gets here.
constexpr size_t kMaxPendingPackets = 8192;
constexpr size_t kMaxPendingBytes = 8u << 20;

}  // namespace

bool RetransmitBudget::take(size_t bytes, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    if (lastMs_ == 0 || nowMs < lastMs_) lastMs_ = nowMs;  // instances' clocks may differ by a few ms
    tokens_ = std::min(kRetransmitBytesBurst, tokens_ + (nowMs - lastMs_) * kRetransmitBytesPerSecond / 1000.0);
    lastMs_ = nowMs;
    if (tokens_ < static_cast<double>(bytes)) {
        ++refusals_;
        return false;
    }
    tokens_ -= static_cast<double>(bytes);
    return true;
}

uint64_t RetransmitBudget::refusals() const {
    std::lock_guard<std::mutex> lock(mu_);
    return refusals_;
}

std::shared_ptr<RetransmitBudget> processRetransmitBudget() {
    static std::shared_ptr<RetransmitBudget> budget = std::make_shared<RetransmitBudget>();
    return budget;
}

void ReliableSender::track(uint32_t seq, std::vector<uint8_t> datagram, uint64_t nowMs, bool expires) {
    Pending& p = pending_[seq];
    pendingBytes_ += datagram.size() - p.datagram.size();
    p.datagram = std::move(datagram);
    p.firstSentMs = nowMs;
    p.lastSentMs = nowMs;
    p.sends = 1;
    p.expiresMs = expires ? nowMs + kExpireMs : 0;
}

void ReliableSender::sampleRtt(uint64_t rttMs) {
    double r = static_cast<double>(rttMs);
    if (!haveRtt_) {
        srtt_ = r;
        rttvar_ = r / 2;
        haveRtt_ = true;
        return;
    }
    rttvar_ = 0.75 * rttvar_ + 0.25 * std::fabs(srtt_ - r);
    srtt_ = 0.875 * srtt_ + 0.125 * r;
}

size_t ReliableSender::onAck(const AckMsg& ack, uint64_t nowMs) {
    size_t acked = 0;
    auto take = [&](std::map<uint32_t, Pending>::iterator it) {
        // Karn's rule: only packets sent once give an unambiguous RTT sample.
        if (it->second.sends == 1) sampleRtt(nowMs - it->second.firstSentMs);
        ++acked;
        pendingBytes_ -= it->second.datagram.size();
        return pending_.erase(it);
    };
    peerCumulative_ = std::max(peerCumulative_, ack.cumulative);
    for (auto it = pending_.begin(); it != pending_.end() && it->first <= ack.cumulative;) it = take(it);
    for (const AckRange& r : ack.ranges) {
        uint64_t end = uint64_t{r.first} + r.count;
        for (auto it = pending_.lower_bound(r.first); it != pending_.end() && it->first < end;) it = take(it);
    }
    credit_ = std::min(kRetransmitBurst, credit_ + kRetransmitsPerAck * static_cast<double>(acked));
    return acked;
}

uint32_t ReliableSender::rtoMs() const {
    double rto = srtt_ + std::max(kClockGranularityMs, 4 * rttvar_);
    return std::clamp(static_cast<uint32_t>(rto), kMinRtoMs, kMaxRtoMs);
}

void ReliableSender::poll(uint64_t nowMs, const std::function<bool(std::vector<uint8_t>&)>& resend,
                          RetransmitBudget* shared) {
    if (lastPollMs_ == 0) lastPollMs_ = nowMs;
    uint64_t elapsed = nowMs - lastPollMs_;
    lastPollMs_ = nowMs;
    credit_ = std::min(kRetransmitBurst, credit_ + elapsed * kProbeRetransmitsPerSecond / 1000.0);
    bool held = false;
    for (auto it = pending_.begin(); it != pending_.end();) {
        Pending& p = it->second;
        if (p.expiresMs != 0 && nowMs >= p.expiresMs) {
            // An unreliable game packet past its deadline: given up, the peer learns it from a Forward.
            pendingBytes_ -= p.datagram.size();
            it = pending_.erase(it);
            ++abandoned_;
            forwardNow_ = true;
            continue;
        }
        // Exponential backoff per packet, capped at the maximum RTO.
        uint64_t backoff = std::min<uint64_t>(static_cast<uint64_t>(rtoMs()) << std::min<uint32_t>(p.sends - 1, 4),
                                              kMaxRtoMs);
        if (nowMs - p.lastSentMs < backoff) {
            ++it;
            continue;
        }
        // Oldest first: they are the ones the peer's delivery (and our stall timeout) waits for.
        if (credit_ < 1.0 || (shared && !shared->take(p.datagram.size(), nowMs))) {
            held = true;
            ++it;
            continue;
        }
        if (!resend(p.datagram)) {
            sendRefused();
            held = true;
            ++it;
            continue;
        }
        p.lastSentMs = nowMs;
        ++p.sends;
        ++retransmits_;
        credit_ -= 1.0;
        ++it;
    }
    if (held) limitedMs_ += elapsed;
}

std::optional<uint32_t> ReliableSender::forwardDue(uint64_t nowMs) {
    uint32_t f = floor();
    // Below the floor everything is acknowledged or given up. The peer's cumulative point stops short of it
    // only at a packet we gave up: it would wait for that packet forever.
    if (f <= peerCumulative_ + 1) return std::nullopt;
    if (!forwardNow_ && nowMs - lastForwardMs_ < rtoMs()) return std::nullopt;
    forwardNow_ = false;
    lastForwardMs_ = nowMs;
    return f;
}

bool ReliableSender::overloaded() const {
    return pending_.size() > kMaxPendingPackets || pendingBytes_ > kMaxPendingBytes;
}

uint64_t ReliableSender::oldestReliableAgeMs(uint64_t nowMs) const {
    for (const auto& [seq, p] : pending_)
        if (p.expiresMs == 0) return nowMs - p.firstSentMs;
    return 0;
}

AckMsg ReliableReceiver::currentAck(uint32_t arrived) const {
    AckMsg ack;
    ack.cumulative = expected_ - 1;
    std::vector<AckRange> held;
    for (const auto& [seq, slot] : buffer_) {
        if (!held.empty() && uint64_t{held.back().first} + held.back().count == seq && held.back().count < UINT16_MAX)
            ++held.back().count;
        else
            held.push_back({seq, 1});
    }
    if (held.size() <= kMaxAckRanges) {
        ack.ranges = std::move(held);
        return ack;
    }
    // More gaps than one ACK names: the lowest ones, which hold the sender's oldest packets, and the range
    // of the packet that just came in. Every packet held here is resent until acknowledged, so each one is
    // named at the latest by the ACK its resend brings.
    ack.ranges.assign(held.begin(), held.begin() + (kMaxAckRanges - 1));
    auto own = std::find_if(held.begin() + (kMaxAckRanges - 1), held.end(), [&](const AckRange& r) {
        return arrived >= r.first && uint64_t{arrived} < uint64_t{r.first} + r.count;
    });
    ack.ranges.push_back(own != held.end() ? *own : held[kMaxAckRanges - 1]);
    return ack;
}

void ReliableReceiver::releaseContiguous(std::vector<DataMsg>& deliver) {
    // Ordered packets that were waiting, and skip markers of unordered packets handed out on arrival.
    for (auto it = buffer_.find(expected_); it != buffer_.end(); it = buffer_.find(expected_)) {
        if (!it->second.delivered) deliver.push_back(std::move(it->second.msg));
        buffer_.erase(it);
        ++expected_;
    }
}

AckMsg ReliableReceiver::onData(DataMsg msg, std::vector<DataMsg>& deliver) {
    uint32_t seq = msg.seq;
    // Already delivered (below the cumulative point) or already held: a retransmitted duplicate.
    if (seq < expected_ || buffer_.count(seq)) {
        ++duplicates_;  // the sender resent it: our ACK was late or lost (or the resend was spurious)
        return currentAck(seq);
    }
    if (seq == expected_) {
        deliver.push_back(std::move(msg));
        ++expected_;
        releaseContiguous(deliver);
    } else if (buffer_.size() < kMaxReorderBuffer) {
        Slot slot;
        if (msg.reliability == 2) {
            slot.msg = std::move(msg);  // ReliableOrdered: wait for every earlier packet
        } else {
            deliver.push_back(std::move(msg));  // unordered: deliver now, remember it for dedup
            slot.delivered = true;
        }
        buffer_.emplace(seq, std::move(slot));
    }
    // Buffer full: drop without acknowledging; the sender retransmits once the gap closes.
    return currentAck(seq);
}

AckMsg ReliableReceiver::onForward(uint32_t floor, std::vector<DataMsg>& deliver) {
    if (floor > expected_) {
        uint64_t held = 0;
        for (auto it = buffer_.begin(); it != buffer_.end() && it->first < floor; it = buffer_.erase(it)) {
            if (!it->second.delivered) deliver.push_back(std::move(it->second.msg));  // in sequence order
            ++held;
        }
        skipped_ += floor - expected_ - held;
        expected_ = floor;
        releaseContiguous(deliver);
    }
    return currentAck(0);
}

}  // namespace dn
