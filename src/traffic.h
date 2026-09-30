// Measures how much the game itself sends, to see whether it runs into its own bandwidth budget.
//
// EDF6 keeps its routine state sync under about 320 kbps and all traffic under 1 Mbps, skipping
// less important updates near that limit (the developer's own description); desync on screen can
// come from that. A minute average hides the bursts the budget acts on, so the busiest second is
// tracked too. Packets whose payload equals the previous one sent to another player count as
// "copies": the game sending the same data to everyone, which a relay could send once.
//
// Packets whose payload equals one sent to the same player within kRepeatWindowMs count as "repeats":
// retransmissions of the game's own, if it has any on top of the unreliable transport (whether it does
// decides what ReliableGameTraffic costs). A retransmission that changes a sequence or time field on the
// way is not a repeat by this measure, and neither is data the game resends only after a longer silence.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace dn {

struct TrafficSummary {
    uint64_t bytes = 0;
    uint64_t packets = 0;
    size_t peers = 0;              // distinct destinations
    uint64_t copyBytes = 0;        // payload identical to the previous packet, sent to another player
    uint64_t repeatPackets = 0;    // payload identical to a recent packet to the same player
    uint64_t busiestSecondBytes = 0;
    uint32_t largestPacket = 0;
};

class TrafficMeter {
public:
    static constexpr uint64_t kRepeatWindowMs = 5000;
    static constexpr size_t kRepeatHistory = 64;  // packets remembered per player
    // A game packet of `size` bytes to `remote`; `payloadHash` identifies its content.
    void record(const std::string& remote, uint32_t size, uint64_t payloadHash, uint64_t nowMs);
    // Returns what was recorded since the last call and starts over.
    TrafficSummary take();

    static uint64_t hash(const void* data, size_t size);  // FNV-1a

private:
    std::mutex mu_;
    TrafficSummary cur_;
    std::unordered_set<std::string> peers_;
    uint64_t second_ = 0;  // nowMs / 1000 of the running second
    uint64_t secondBytes_ = 0;
    uint64_t lastHash_ = 0;
    uint32_t lastSize_ = 0;
    std::string lastRemote_;
    struct Sent {
        uint64_t hash;
        uint32_t size;
        uint64_t ms;
    };
    std::unordered_map<std::string, std::deque<Sent>> recent_;  // per player, newest last
};

}  // namespace dn
