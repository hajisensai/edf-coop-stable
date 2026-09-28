#include "reliable.h"

#include <algorithm>
#include <cmath>

namespace dn {
namespace {

constexpr uint32_t kMinRtoMs = 60;
constexpr uint32_t kMaxRtoMs = 1000;
constexpr size_t kMaxReorderBuffer = 4096;
// Retransmission rate limit (token bucket). After a long stall thousands of packets are pending and
// all their timers expire together; the budget caps the resend burst (~2.4 MB/s at full packets)
// while still letting every lost packet be retried, so recovery is not serialised behind the oldest.
constexpr double kRetransmitPerSecond = 2000.0;
constexpr double kRetransmitBurst = 64.0;

}  // namespace

void ReliableSender::track(uint32_t seq, std::vector<uint8_t> datagram, uint64_t nowMs) {
    Pending& p = pending_[seq];
    p.datagram = std::move(datagram);
    p.firstSentMs = nowMs;
    p.lastSentMs = nowMs;
    p.sends = 1;
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
        return pending_.erase(it);
    };
    for (auto it = pending_.begin(); it != pending_.end() && it->first <= ack.cumulative;) it = take(it);
    for (uint32_t bit = 0; bit < kAckBits; ++bit) {
        if (!ack.has(bit)) continue;
        auto it = pending_.find(ack.cumulative + 2 + bit);
        if (it != pending_.end()) take(it);
    }
    return acked;
}

uint32_t ReliableSender::rtoMs() const {
    double rto = srtt_ + 4 * rttvar_;
    return std::clamp(static_cast<uint32_t>(rto), kMinRtoMs, kMaxRtoMs);
}

void ReliableSender::poll(uint64_t nowMs, const std::function<void(const std::vector<uint8_t>&)>& resend) {
    if (lastBudgetMs_ == 0) lastBudgetMs_ = nowMs;
    budget_ = std::min(kRetransmitBurst, budget_ + (nowMs - lastBudgetMs_) * kRetransmitPerSecond / 1000.0);
    lastBudgetMs_ = nowMs;
    for (auto& [seq, p] : pending_) {
        if (budget_ < 1.0) break;
        // Exponential backoff per packet, capped at the maximum RTO.
        uint64_t backoff = std::min<uint64_t>(static_cast<uint64_t>(rtoMs()) << std::min<uint32_t>(p.sends - 1, 4),
                                              kMaxRtoMs);
        if (nowMs - p.lastSentMs < backoff) continue;
        resend(p.datagram);
        p.lastSentMs = nowMs;
        ++p.sends;
        ++retransmits_;
        budget_ -= 1.0;
    }
}

uint64_t ReliableSender::oldestPendingAgeMs(uint64_t nowMs) const {
    if (pending_.empty()) return 0;
    return nowMs - pending_.begin()->second.firstSentMs;
}

AckMsg ReliableReceiver::currentAck() const {
    AckMsg ack;
    ack.cumulative = expected_ - 1;
    for (const auto& [seq, msg] : buffer_) {
        ack.set(seq - expected_ - 1);  // seq == cumulative + 2 + bit
    }
    return ack;
}

AckMsg ReliableReceiver::onData(DataMsg msg, std::vector<DataMsg>& deliver) {
    uint32_t seq = msg.seq;
    // Already delivered (below the cumulative point) or already held: a retransmitted duplicate.
    if (seq < expected_ || buffer_.count(seq)) return currentAck();
    if (seq == expected_) {
        deliver.push_back(std::move(msg));
        ++expected_;
        // Release everything now contiguous: ordered packets that were waiting, and skip markers of
        // unordered packets that were handed out on arrival.
        for (auto it = buffer_.find(expected_); it != buffer_.end(); it = buffer_.find(expected_)) {
            if (!it->second.delivered) deliver.push_back(std::move(it->second.msg));
            buffer_.erase(it);
            ++expected_;
        }
    } else if (buffer_.size() < kMaxReorderBuffer) {
        Slot slot;
        if (msg.reliability == 2) {
            slot.msg = std::move(msg);  // ReliableOrdered: wait for every earlier packet
        } else {
            deliver.push_back(std::move(msg));  // ReliableUnordered: deliver now, remember it for dedup
            slot.delivered = true;
        }
        buffer_.emplace(seq, std::move(slot));
    }
    // Buffer full: drop without acknowledging; the sender retransmits once the gap closes.
    return currentAck();
}

}  // namespace dn
