// Per-link reliability: sequence numbers, cumulative+bitmap ACKs, retransmission and in-order delivery.
// Pure logic; the caller injects time and does the socket I/O.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "wire.h"

namespace dn {

class ReliableSender {
public:
    struct Pending {
        std::vector<uint8_t> datagram;
        uint64_t firstSentMs = 0;
        uint64_t lastSentMs = 0;
        uint32_t sends = 0;
    };

    uint32_t nextSeq() { return next_++; }
    void track(uint32_t seq, std::vector<uint8_t> datagram, uint64_t nowMs);
    // Returns the number of packets newly acknowledged.
    size_t onAck(const AckMsg& ack, uint64_t nowMs);
    // Calls `resend` for every packet whose retransmission timer expired.
    void poll(uint64_t nowMs, const std::function<void(const std::vector<uint8_t>&)>& resend);
    // Age of the oldest unacknowledged packet (0 when nothing is pending).
    uint64_t oldestPendingAgeMs(uint64_t nowMs) const;
    size_t pendingCount() const { return pending_.size(); }
    uint32_t rtoMs() const;
    uint32_t srttMs() const { return static_cast<uint32_t>(srtt_); }
    uint64_t retransmits() const { return retransmits_; }

private:
    void sampleRtt(uint64_t rttMs);

    uint32_t next_ = 1;
    std::map<uint32_t, Pending> pending_;
    double srtt_ = 100.0;
    double rttvar_ = 50.0;
    bool haveRtt_ = false;
    uint64_t retransmits_ = 0;
};

class ReliableReceiver {
public:
    // Feeds a received reliable DataMsg. In-order messages (including ones released from the
    // reorder buffer) are appended to `deliver`. Returns the ACK to send back.
    AckMsg onData(DataMsg msg, std::vector<DataMsg>& deliver);
    uint32_t expected() const { return expected_; }
    size_t buffered() const { return buffer_.size(); }

private:
    AckMsg currentAck() const;

    uint32_t expected_ = 1;
    std::map<uint32_t, DataMsg> buffer_;
};

}  // namespace dn
