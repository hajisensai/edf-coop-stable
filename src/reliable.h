// Per-link reliability: sequence numbers, cumulative+range ACKs, retransmission and in-order delivery.
// Pure logic; the caller injects time and does the socket I/O.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "wire.h"

namespace dn {

// Retransmission bytes per second all links of a process may send together (see kRetransmitBytesPerSecond).
// Each link's own limit follows what that link delivers; this one bounds the sum, so a host with many
// lossy links cannot fill its uplink with resends. Thread-safe: every DirectNet instance has its own thread.
class RetransmitBudget {
public:
    // Takes `bytes` of the allowance; false when it is used up (the resend waits for the next poll).
    bool take(size_t bytes, uint64_t nowMs);
    // Resends held back because the allowance was used up, since start.
    uint64_t refusals() const;

private:
    mutable std::mutex mu_;
    double tokens_ = 64.0 * 1024;  // bytes, see kRetransmitBytesPerSecond
    uint64_t lastMs_ = 0;
    uint64_t refusals_ = 0;
};

// The budget shared by every link of this process (DirectOptions::retransmitBudget left null).
std::shared_ptr<RetransmitBudget> processRetransmitBudget();

class ReliableSender {
public:
    // An unreliable game packet carried reliably (DirectOptions::upgradeUnreliable) is resent only this
    // long after it was first sent, then given up. Real links have a 150 ms RTO (the floor), so a lost
    // packet still goes out at 0, 150, 450 and 1050 ms: four tries, lost only to four losses in a row.
    // EDF6 sends such state 15-20 times a second per player, so after 2 s it has been replaced many
    // times over, and a packet the network keeps losing costs at most these few resends.
    static constexpr uint64_t kExpireMs = 2000;

    struct Pending {
        std::vector<uint8_t> datagram;
        uint64_t firstSentMs = 0;
        uint64_t lastSentMs = 0;
        uint32_t sends = 0;
        uint64_t expiresMs = 0;  // given up at this time; 0: never (the game sent it reliably)
    };

    uint32_t nextSeq() { return next_++; }
    // `expires`: an unreliable game packet, given up kExpireMs after now (see Forward in wire.h).
    void track(uint32_t seq, std::vector<uint8_t> datagram, uint64_t nowMs, bool expires = false);
    // Returns the number of packets newly acknowledged.
    size_t onAck(const AckMsg& ack, uint64_t nowMs);
    // Gives up expired packets and calls `resend` for packets whose retransmission timer expired, as
    // far as the link's retransmit credit and `shared` (null: no shared limit) allow. `resend` may
    // rewrite the datagram in place (a new link counter and tag) but not change its size; it returns
    // false when the socket refused the datagram, which counts as congestion (see sendRefused).
    void poll(uint64_t nowMs, const std::function<bool(std::vector<uint8_t>&)>& resend,
              RetransmitBudget* shared = nullptr);
    // The floor to send in a Forward now, if the peer has to learn that we gave packets up: it has not
    // acknowledged up to the floor yet. Repeated every RTO until an acknowledgement shows it arrived.
    std::optional<uint32_t> forwardDue(uint64_t nowMs);
    // The socket refused a datagram (its send buffer is full): congestion on our side. Retransmissions
    // wait until acknowledgements earn new credit.
    void sendRefused() { credit_ = 0.0; }
    // Age of the oldest unacknowledged packet the game sent reliably (0 when none is pending). Packets
    // that expire do not count: they cannot hold a link up for longer than kExpireMs.
    uint64_t oldestReliableAgeMs(uint64_t nowMs) const;
    size_t pendingCount() const { return pending_.size(); }
    size_t pendingBytes() const { return pendingBytes_; }
    // More unacknowledged data than any live link builds up (see kMaxPendingBytes): the peer is not
    // acknowledging on purpose. The link must be closed; nothing is dropped from it silently.
    bool overloaded() const;
    uint32_t rtoMs() const;
    uint32_t srttMs() const { return static_cast<uint32_t>(srtt_); }
    uint64_t retransmits() const { return retransmits_; }
    // Expiring packets given up unacknowledged, since start.
    uint64_t abandoned() const { return abandoned_; }
    // Time retransmissions were due but held back by the link's credit or the shared budget, since start.
    uint64_t limitedMs() const { return limitedMs_; }
    // Retransmissions the link may send right now.
    double credit() const { return credit_; }

private:
    void sampleRtt(uint64_t rttMs);
    uint32_t floor() const { return pending_.empty() ? next_ : pending_.begin()->first; }

    uint32_t next_ = 1;
    std::map<uint32_t, Pending> pending_;
    size_t pendingBytes_ = 0;
    double srtt_ = 100.0;
    double rttvar_ = 50.0;
    bool haveRtt_ = false;
    uint64_t retransmits_ = 0;
    uint64_t abandoned_ = 0;
    uint64_t limitedMs_ = 0;
    double credit_ = 64.0;  // retransmissions, see kRetransmitsPerAck
    uint64_t lastPollMs_ = 0;
    uint32_t peerCumulative_ = 0;  // highest cumulative point the peer acknowledged
    uint64_t lastForwardMs_ = 0;
    bool forwardNow_ = false;  // packets were just given up: tell the peer without waiting an RTO
};

class ReliableReceiver {
public:
    // Feeds a received reliable DataMsg and appends deliverable messages to `deliver`, each exactly
    // once. ReliableOrdered (2) messages are delivered in sequence order; the others (ReliableUnordered
    // and unreliable ones carried with a sequence number) as soon as they arrive. Returns the ACK to
    // send back.
    AckMsg onData(DataMsg msg, std::vector<DataMsg>& deliver);
    // The sender gave up everything below `floor` it did not see acknowledged: moves the cumulative
    // point there, delivering ordered packets that only waited for those. Returns the ACK to send back.
    AckMsg onForward(uint32_t floor, std::vector<DataMsg>& deliver);
    uint32_t expected() const { return expected_; }
    size_t buffered() const { return buffer_.size(); }
    // Reliable packets received again after they had arrived: set against the sender's retransmits,
    // the share of its resends that were not needed.
    uint64_t duplicates() const { return duplicates_; }
    // Sequence numbers the sender gave up that never arrived here.
    uint64_t skipped() const { return skipped_; }

private:
    // The ACK for the current state; `arrived` (when held above the cumulative point) is always named.
    AckMsg currentAck(uint32_t arrived) const;
    // Moves the cumulative point over packets now contiguous with it.
    void releaseContiguous(std::vector<DataMsg>& deliver);

    struct Slot {
        DataMsg msg;
        bool delivered = false;  // unordered packet already handed out; kept only for dedup/ACK
    };

    uint32_t expected_ = 1;
    std::map<uint32_t, Slot> buffer_;
    uint64_t duplicates_ = 0;
    uint64_t skipped_ = 0;
};

}  // namespace dn
