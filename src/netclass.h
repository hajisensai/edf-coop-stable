// What the game's packet controller puts in a datagram, measured and classified before the game encrypts it.
//
// EDF.dll 678CCB46, eos::packet::ControllerImpl (vtable 1AF4FF0), static reading:
//  - Records go into a per-peer buffer (12CFFD0) that is flushed (12CEA10) once a new record would take it past
//    1100 bytes (0x44C), and on the controller's 90 ms timer. The flush writes an 8-byte header (two random bytes,
//    a 16-bit datagram number, a CRC32C of the rest), encrypts everything after the header (AES-CTR, 7B8060, same
//    length), and hands the datagram to EOS_P2P_SendPacket on the same thread (12D0120 -> vtable slot 3 12D2EF0
//    -> 12C8BC0, always EOS_PR_UnreliableUnordered). Encryption keeps the length, so the datagram EOS sees has
//    the plaintext's size.
//  - Every record starts with u32 (size << 20 | type); `size` bytes follow (the receive loop 12CE515 advances by
//    size + 4 for every record). Data records carry a Transmit's type ((sub & 0xF) | id << 4) << 8 (12C2E59).
//  - Controller::SendUnreliable (12D1040) writes the data record alone: lost with its datagram, never sent again.
//  - Controller::SendReliable (12D0AC0) writes an envelope first: u32 0x00401200 (type 0x1200, size 4) and a u32
//    sequence number, then the data record. It keeps the record pending (6 tries, first resend after 700 ms,
//    1FEF040, growing by 1FEF048 each try) until the receiver acknowledges it; the receiver acknowledges every
//    envelope (12CE4FF -> 12CFCD0) and drops a record it has had (the flag in r15 skips it).
//  - 0x13xx, 0x14xx and 0x18xx records are the controller's own (acknowledgements and the like, 12CE2B7..12CE2D7).
//  - The receiver drops a datagram whose number and CRC it has seen (12CE1F3..12CE1FE): an identical datagram is
//    taken once by the game, which is why a copy sent over a second path costs nothing but bandwidth there.
//
// The transport below the game (EOS or the direct link) uses the class of a datagram to decide how to carry it
// (docs/netcode-rewrite-plan.md W1):
//   State   only unreliable data records of types that behave as state (sent again and again to the same
//           player, see StateLearner): carried unreliably, nothing resent - the next one replaces it.
//   Event   holds a reliable record: the game resends it itself, so the transport does not; it is sent over the
//           two best paths at once, which repairs a loss without waiting the game's 700 ms.
//   Control only the controller's own records (acknowledgements): small, sent like Event.
//   Unknown anything else - unreliable records of a type not (yet) known as state, a datagram that did not
//           parse, or one the flush hook never saw: carried as before this classification existed (an
//           unreliable packet repaired until ReliableSender::kExpireMs), so nothing loses what it had.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dn {

constexpr size_t kControllerDatagramHeader = 8;
constexpr uint32_t kRecordReliableEnvelope = 0x1200;
// The event controller's record type (missionsync.cpp kEventRecordType): event messages of every kind travel
// batched in it, reliable or not (761E60 picks SendUnreliable when the builder's +0x628 is set). An unreliable
// batch may carry a one-shot event whose loss nothing repairs, so it is never taken for state however often it
// goes out.
constexpr uint32_t kRecordEventBatch = 0x3000;

struct RecordInfo {
    uint32_t type = 0;   // low 20 bits of the record header
    uint32_t size = 0;   // payload bytes after the 4-byte header
    bool reliable = false;  // came after a reliable envelope
};

// The records of a plaintext controller datagram (header included). False when a record runs past the end
// (`out` then holds the ones before it).
bool parseControllerDatagram(const uint8_t* plain, size_t size, std::vector<RecordInfo>& out);

// The controller's own records (envelopes, acknowledgements): not game data.
inline bool isControllerRecord(uint32_t type) {
    const uint32_t kind = type & 0xFF00;
    return kind == 0x1200 || kind == 0x1300 || kind == 0x1400 || kind == 0x1800;
}

enum class TrafficClass : uint8_t { Unknown = 0, State = 1, Event = 2, Control = 3 };
const char* trafficClassName(TrafficClass c);

// Which unreliable record types behave as state: sent to the same player kSamples times, each within kMaxIntervalMs
// of the one before (a later one halves the count: a hitch does not undo the run, a type sent with gaps never
// learns). A type once learnt stays state for the room (forget() on leaving it):
// a player that stops moving sends less often, and its next update still replaces the last one. Thread-safe.
class StateLearner {
public:
    static constexpr uint32_t kSamples = 20;
    static constexpr uint64_t kMaxIntervalMs = 300;  // three times the controller's 90 ms beat
    // True when this update made `type` state (once per type).
    bool observe(const std::string& remote, uint32_t type, uint64_t nowMs);
    bool isState(uint32_t type) const;
    void forget();

private:
    struct Run {
        uint64_t lastMs = 0;
        uint32_t steady = 0;  // consecutive short intervals
    };
    mutable std::mutex mu_;
    std::map<std::pair<std::string, uint32_t>, Run> runs_;
    std::map<uint32_t, bool> state_;
};

// The class of a datagram whose records are `records` (see the file comment). `parsed` false: Unknown.
TrafficClass classify(const std::vector<RecordInfo>& records, bool parsed, const StateLearner& learner);

// Per record type: how often, how big, how far apart (per player), for the TRAFFIC log (P0 of the plan).
class RecordTypeMeter {
public:
    void record(const std::string& remote, const std::vector<RecordInfo>& records, uint64_t nowMs);
    // Datagrams by class, with their bytes.
    void recordDatagram(TrafficClass c, size_t bytes);
    // One line per record type seen since the last call (busiest first, at most `maxLines`), then starts over.
    // `seconds`: the period the counts cover.
    std::vector<std::string> take(double seconds, size_t maxLines = 24);

private:
    struct Type {
        uint64_t records = 0, bytes = 0, reliable = 0;
        uint64_t intervals = 0, intervalSumMs = 0, intervalMinMs = UINT64_MAX, intervalMaxMs = 0;
        uint32_t largest = 0;
    };
    std::mutex mu_;
    std::map<uint32_t, Type> types_;
    std::map<std::pair<std::string, uint32_t>, uint64_t> last_;  // per player and type: when last sent
    uint64_t datagrams_[4] = {}, datagramBytes_[4] = {};
};

// The flush hook (multislot nettraffic.cpp) hands over the records of the datagram it is about to pass to the
// game's encryption; the EOS send hook takes them on the same thread right after (the game sends synchronously, see
// above) and classifies them with the remote it is for. `bytes`: the datagram's size, which the encrypted one keeps;
// a send of another size is not that datagram.
struct PlainDatagram {
    std::vector<RecordInfo> records;
    bool parsed = false;
};
void notePendingDatagram(const uint8_t* plain, size_t bytes);
// The records noted for a datagram of `bytes`, once; nullopt when none was noted on this thread or the size differs
// (a packet of another size leaves the note for the datagram it belongs to).
std::optional<PlainDatagram> takePendingDatagram(size_t bytes);

// Drops a game packet that arrived once already (from the same player, the same bytes) over another path: a
// datagram sent over two paths at once, or over the direct link and EOS. The game's datagrams are never byte for
// byte the same (each has its own number, random bytes and CRC), so only copies are dropped. Remembers the last
// kWindow per player for kWindowMs. Thread-safe.
class DuplicateFilter {
public:
    static constexpr size_t kWindow = 256;
    static constexpr uint64_t kWindowMs = 4000;
    // True the first time; false for a copy.
    bool first(const std::string& src, const uint8_t* data, size_t size, uint64_t nowMs);
    uint64_t duplicates() const;
    void clear();

private:
    struct Seen {
        uint64_t hash;
        uint64_t ms;
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::deque<Seen>> seen_;
    uint64_t duplicates_ = 0;
};

uint64_t fnv1a64(const void* data, size_t size, uint64_t seed = 1469598103934665603ull);

}  // namespace dn
