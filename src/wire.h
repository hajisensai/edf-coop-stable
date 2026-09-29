// EDF6DirectNet datagram format. All integers little endian.
//
//   u32 magic 'EDN1' | u8 type | u8 flags | u16 protocol | body ... | [8-byte HMAC tag if flags&kFlagTagged]
//
// Strings are u8 length + bytes (max 64).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dn {

constexpr uint32_t kMagic = 0x314E4445;  // "EDN1"
constexpr uint16_t kProtocol = 2;
constexpr uint8_t kFlagTagged = 1;
constexpr size_t kTagBytes = 8;
// Longest id / socket name on the wire. Decoding rejects longer ones instead of reading a string the
// sender's encoder would have cut, so both sides always agree on an id. EOS ids and socket names are
// at most 32 characters.
constexpr size_t kMaxString = 64;
// Largest game packet carried: EOS_P2P_MAX_PACKET_SIZE. A Data datagram announcing more is rejected.
constexpr size_t kMaxPayload = 1170;

enum class MsgType : uint8_t {
    Hello = 1,    // client -> host: I am <puid>, session nonce
    Welcome = 2,  // host -> client: host puid + roster, echoes client nonce
    Roster = 3,   // host -> clients: current direct members
    Data = 4,     // game packet
    Ack = 5,      // reliable-link acknowledgement
    Ping = 6,
    Pong = 7,
    Bye = 8,
};

struct HelloMsg {
    uint32_t nonce = 0;
    std::string puid;
};

struct WelcomeMsg {
    uint32_t hostNonce = 0;
    uint32_t clientNonce = 0;
    std::string hostPuid;
    std::vector<std::string> roster;
};

struct RosterMsg {
    uint32_t hostNonce = 0;
    std::vector<std::string> roster;
};

struct DataMsg {
    uint32_t seq = 0;  // 0 = unreliable
    std::string src;
    std::string dst;
    std::string socketName;
    uint8_t channel = 0;
    uint8_t reliability = 0;  // EOS_EPacketReliability
    std::vector<uint8_t> payload;
};

constexpr uint32_t kAckBits = 256;

struct AckMsg {
    uint32_t cumulative = 0;  // every seq <= cumulative received
    uint32_t bitmap[kAckBits / 32] = {};  // bit i => seq cumulative + 2 + i received

    bool has(uint32_t bit) const { return bit < kAckBits && (bitmap[bit / 32] >> (bit % 32)) & 1u; }
    void set(uint32_t bit) {
        if (bit < kAckBits) bitmap[bit / 32] |= 1u << (bit % 32);
    }
};

struct PingMsg {
    uint64_t timeMs = 0;
};

struct Message {
    MsgType type = MsgType::Ping;
    // Link session id for Data/Ack/Ping/Pong, derived from both sides' hello nonces. Packets from an
    // older session of the same link (in flight across a reconnect) carry a different epoch and are
    // dropped instead of acknowledging or duplicating packets of the new session.
    uint32_t epoch = 0;
    HelloMsg hello;
    WelcomeMsg welcome;
    RosterMsg roster;
    DataMsg data;
    AckMsg ack;
    PingMsg ping;  // also used for Pong
};

inline bool isLinkScoped(MsgType t) {
    return t == MsgType::Data || t == MsgType::Ack || t == MsgType::Ping || t == MsgType::Pong;
}

inline uint32_t linkEpoch(uint32_t clientNonce, uint32_t hostNonce) {
    return clientNonce * 0x9E3779B1u ^ hostNonce;
}

// Encodes a message; when `key` is non-empty an HMAC tag is appended.
std::vector<uint8_t> encode(const Message& msg, const std::string& key);

enum class DecodeError { None, BadMagic, BadProtocol, Truncated, Malformed, TagMissing, TagUnexpected, TagMismatch };

// Decodes and authenticates a datagram. `key` must match the sender's key (both empty is fine).
std::optional<Message> decode(const uint8_t* data, size_t size, const std::string& key, DecodeError* err);

const char* decodeErrorName(DecodeError e);

}  // namespace dn
