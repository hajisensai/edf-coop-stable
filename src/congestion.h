// How many bytes per second one path carries now: a delay-based rate controller (LEDBAT's idea, RFC 6817, in a
// rate form), so the budget follows the path instead of a fixed cap.
//
// A path's round trip without queueing is the lowest one seen in the last kBaseWindowMs; anything above it is time
// spent in a queue somewhere on the way. While that queueing delay stays under kTargetQueueMs the rate grows (by
// up to kGainPerRound of itself per round trip, in proportion to how far below the target it is), above it the rate
// shrinks the same way, and a lost probe or packet cuts it by kLossCut (once per round trip). A path the sender does
// not fill (it sent less than half its rate in the last second) does not grow: an idle path has proven nothing.
//
// EDF6 keeps its own routine state sync under about 320 kbps (40 KB/s) and all traffic under 1 Mbps (the
// developer's description, traffic.h); kStartRate starts a path at 1 Mbps, and kMinRate never goes below the
// game's own budget, so a path the controller distrusts still carries what the game sent before this existed.
#pragma once
#include <array>
#include <cstdint>

namespace dn {

class RateController {
public:
    static constexpr uint32_t kMinRate = 40 * 1024;
    static constexpr uint32_t kMaxRate = 8u << 20;
    static constexpr uint32_t kStartRate = 128 * 1024;
    static constexpr uint32_t kTargetQueueMs = 50;
    static constexpr uint64_t kBaseWindowMs = 10000;
    static constexpr double kGainPerRound = 0.25;
    static constexpr double kLossCut = 0.7;

    // A round trip measured on the path (a pong, an acknowledgement).
    void onRtt(uint32_t rttMs, uint64_t nowMs);
    // A probe or packet the path lost.
    void onLoss(uint64_t nowMs);
    // Bytes sent over the path (it is being used: the rate may grow).
    void onSent(uint32_t bytes, uint64_t nowMs);
    uint32_t rate() const { return static_cast<uint32_t>(rate_); }
    uint32_t baseRttMs() const;
    uint32_t queueMs() const { return queueMs_; }
    uint32_t srttMs() const { return static_cast<uint32_t>(srtt_); }
    bool measured() const { return samples_ > 0; }

private:
    double rate_ = kStartRate;
    double srtt_ = 0;
    uint32_t queueMs_ = 0;
    uint64_t samples_ = 0;
    uint64_t lastAdjustMs_ = 0, lastLossMs_ = 0;
    // The lowest round trip of each second of the base window, newest at baseIndex_.
    static constexpr size_t kBaseSlots = 10;
    std::array<uint32_t, kBaseSlots> base_{};
    uint64_t baseSecond_ = 0;
    size_t baseIndex_ = 0;
    // Bytes sent in the current and the previous second (is the path in use?).
    uint64_t sentSecond_ = 0, sentNow_ = 0, sentLast_ = 0;
    bool appLimited(uint64_t nowMs) const;
};

}  // namespace dn
