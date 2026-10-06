// Fragments: one message of up to kMaxFragmentedBytes carried as several packets of at most EOS's 1170 bytes.
//
// EOS refuses any packet above 1170 bytes (EOS_P2P_MAX_PACKET_SIZE) and so does the direct link, while the game
// writes datagrams of up to 1408 bytes (one 1400-byte record, 12D0BE6, with the 8-byte header) and reads up to
// 4096 (12C8D1D): such a datagram never arrived, and the game resent it for ~26 s before it dropped the room. The
// plugin's own messages (loadout records of a large room, host data) can be far larger. Both go as fragments here,
// and only to a receiver that reads them (kCapFragments): any other would hand them to its game as datagrams that
// do not decrypt, which the game drops.
//
// Each fragment:
//   'EDFG' | u8 version (1) | u8 flags | u16 tag | u32 message id | u32 total bytes | u16 index | u16 count | bytes
// `tag`: the game channel a datagram was sent on, or the bulk handler's tag (flags & kFragmentBulk). Every
// fragment but the last carries kFragmentPayload bytes. Fragments are sent reliably (ordered or not: the
// reassembly takes them in any order) and a message is complete once every index arrived.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dn {

constexpr size_t kFragmentHeader = 20;
constexpr size_t kFragmentPacket = 1170;
constexpr size_t kFragmentPayload = kFragmentPacket - kFragmentHeader;  // 1150
constexpr size_t kMaxFragments = 1024;
// 1177600 bytes: a start message of 1024 players' loadout records (about 140 KB) fits eight times over.
constexpr size_t kMaxFragmentedBytes = kFragmentPayload * kMaxFragments;
constexpr uint8_t kFragmentBulk = 1;
// The channel fragments travel on: the game ignores the channel when it receives and sends everything on 0.
constexpr uint8_t kFragmentChannel = 0x4E;

// A receiver acknowledges every bulk message it completed (an ack: 'EDFA' | u32 message id, on kFragmentChannel); the
// sender sends it again until then (eos_hooks.cpp), so a direct link that drops in the middle loses nothing.
constexpr size_t kFragmentAckBytes = 8;
std::vector<uint8_t> fragmentAck(uint32_t id);
bool parseFragmentAck(const uint8_t* data, size_t size, uint32_t& id);

struct FragmentMessage {
    uint8_t flags = 0;
    uint16_t tag = 0;
    std::vector<uint8_t> bytes;
};

// The packets for message `id`; empty when the message is empty or larger than kMaxFragmentedBytes.
std::vector<std::vector<uint8_t>> splitIntoFragments(uint32_t id, uint8_t flags, uint16_t tag, const uint8_t* data,
                                                     size_t size);
bool isFragment(const uint8_t* data, size_t size);

// Puts messages back together per sender. Bounded: kMaxPartialPerSender messages per sender at a time, kMaxBuffered
// bytes in all, and kTimeoutMs for a message to complete; what does not fit or takes too long is dropped (counted),
// the oldest first. Not thread-safe.
class Reassembler {
public:
    static constexpr size_t kMaxPartialPerSender = 16;
    static constexpr size_t kMaxBuffered = 32u << 20;
    static constexpr uint64_t kTimeoutMs = 15000;
    // A fragment from `src`; the message once it is complete, exactly once: a fragment of a message completed in the
    // last kCompletedMs (resent, or a copy) gives nothing, and sets `*again` (its sender may need telling again that
    // it arrived). Malformed fragments are dropped (counted).
    static constexpr uint64_t kCompletedMs = 60000;
    static constexpr size_t kCompletedKept = 4096;
    std::optional<FragmentMessage> add(const std::string& src, const uint8_t* data, size_t size, uint64_t nowMs,
                                       bool* again = nullptr);
    // The id of a fragment (0 for anything else).
    static uint32_t idOf(const uint8_t* data, size_t size);
    void clear();
    uint64_t dropped() const { return dropped_; }
    uint64_t malformed() const { return malformed_; }
    size_t buffered() const { return buffered_; }

private:
    struct Partial {
        uint8_t flags = 0;
        uint16_t tag = 0;
        uint32_t total = 0;
        uint16_t count = 0;
        uint16_t have = 0;
        uint64_t firstMs = 0;
        std::vector<bool> got;
        std::vector<uint8_t> bytes;
    };
    using Map = std::map<std::pair<std::string, uint32_t>, Partial>;
    void expire(uint64_t nowMs);
    void drop(Map::iterator it);
    Map partial_;
    std::map<std::pair<std::string, uint32_t>, uint64_t> completed_;  // -> when
    size_t buffered_ = 0;
    uint64_t dropped_ = 0, malformed_ = 0;
};

}  // namespace dn
