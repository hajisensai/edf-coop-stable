#include "congestion.h"

#include <algorithm>

namespace dn {

uint32_t RateController::baseRttMs() const {
    uint32_t best = 0;
    for (uint32_t v : base_)
        if (v && (!best || v < best)) best = v;
    return best;
}

bool RateController::appLimited(uint64_t nowMs) const {
    const uint64_t second = nowMs / 1000;
    const uint64_t last = second == sentSecond_ ? sentLast_ : second == sentSecond_ + 1 ? sentNow_ : 0;
    return static_cast<double>(last) < rate_ / 2;
}

void RateController::onSent(uint32_t bytes, uint64_t nowMs) {
    const uint64_t second = nowMs / 1000;
    if (second != sentSecond_) {
        sentLast_ = second == sentSecond_ + 1 ? sentNow_ : 0;
        sentNow_ = 0;
        sentSecond_ = second;
    }
    sentNow_ += bytes;
}

void RateController::onRtt(uint32_t rttMs, uint64_t nowMs) {
    if (rttMs == 0) rttMs = 1;
    const uint64_t second = nowMs / 1000;
    if (samples_ == 0 || second != baseSecond_) {
        // Seconds without a sample leave their slot empty: an old minimum must not outlive the window.
        const uint64_t steps = samples_ == 0 ? kBaseSlots : std::min<uint64_t>(second - baseSecond_, kBaseSlots);
        for (uint64_t i = 0; i < steps; ++i) {
            baseIndex_ = (baseIndex_ + 1) % kBaseSlots;
            base_[baseIndex_] = 0;
        }
        baseSecond_ = second;
    }
    uint32_t& slot = base_[baseIndex_];
    slot = slot ? std::min(slot, rttMs) : rttMs;
    srtt_ = samples_ == 0 ? rttMs : 0.875 * srtt_ + 0.125 * rttMs;
    ++samples_;
    const uint32_t base = baseRttMs();
    queueMs_ = rttMs > base ? rttMs - base : 0;
    // One adjustment per round trip (at least every 50 ms): the delay needs that long to show what the last
    // change did.
    const uint64_t round = std::max<uint64_t>(50, static_cast<uint64_t>(srtt_));
    if (lastAdjustMs_ && nowMs - lastAdjustMs_ < round) return;
    lastAdjustMs_ = nowMs;
    const double off = std::clamp((static_cast<double>(kTargetQueueMs) - queueMs_) / kTargetQueueMs, -1.0, 1.0);
    if (off > 0 && appLimited(nowMs)) return;
    rate_ = std::clamp(rate_ * (1.0 + kGainPerRound * off), static_cast<double>(kMinRate), static_cast<double>(kMaxRate));
}

void RateController::onLoss(uint64_t nowMs) {
    const uint64_t round = std::max<uint64_t>(50, static_cast<uint64_t>(srtt_));
    if (lastLossMs_ && nowMs - lastLossMs_ < round) return;
    lastLossMs_ = nowMs;
    rate_ = std::max(rate_ * kLossCut, static_cast<double>(kMinRate));
}

}  // namespace dn
