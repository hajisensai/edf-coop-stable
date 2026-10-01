// EDF6DirectNet datagram format. All integers little endian.
//
// Handshake (Hello, Challenge, Welcome, Reset):
//   u32 magic 'EDN1' | u8 type | u8 flags | u16 protocol | body ... | [8-byte HMAC tag if flags&kFlagTagged]
// Link (Data, Ack, Forward, Ping, Pong, Roster, Bye), sent only on an established link:
//   u32 magic | u8 type | u8 flags | u16 protocol | u32 epoch | u64 counter | body ... | 16-byte tag
//
// Strings are u8 length + bytes (max 64). The handshake tag is keyed with the shared Key= (when set).
// The link tag is HMAC-SHA256 of everything before it with the sending direction's link key; the
// counter goes up by one per datagram and direction, so a receiver takes each datagram at most once.
//
// Protocol 2 was spoken up to 0.3.6. Protocol 3 (0.4.0) adds the Challenge and a proven Hello: a
// host keeps nothing for a sender until it echoes a cookie sent to its address, and accepts an EOS id
// only with a signature matching the identity that id published in the room. Protocol 4 adds link
// keys: both hellos and welcomes carry an ephemeral ECDH key signed with the sender's published
// identity (the joiner checks the room owner's), and every link datagram is authenticated with the
// keys agreed on. Protocol 5 acknowledges with ranges (every packet the receiver holds, not only the
// 256 after its cumulative point), carries the game's own reliability (0 = sent unreliably, carried
// with a sequence number but given up after a deadline) and adds Forward, which moves the receiver
// past packets the sender gave up. Peers of different protocols reject each other's datagrams
// (BadProtocol) and so keep using EOS with each other.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "auth.h"

namespace dn {

constexpr uint32_t kMagic = 0x314E4445;  // "EDN1"
constexpr uint16_t kProtocol = 5;
constexpr uint8_t kFlagTagged = 1;
constexpr size_t kTagBytes = 8;
// Longest id / socket name on the wire. Decoding rejects longer ones instead of reading a string the
// sender's encoder would have cut, so both sides always agree on an id. EOS ids and socket names are
// at most 32 characters.
constexpr size_t kMaxString = 64;
// Largest game packet carried: EOS_P2P_MAX_PACKET_SIZE. A Data datagram announcing more is rejected.
constexpr size_t kMaxPayload = 1170;
constexpr size_t kCookieBytes = 8;
using Cookie = std::array<uint8_t, kCookieBytes>;

enum class MsgType : uint8_t {
    Hello = 1,    // client -> host: I am <puid>, session nonce; proven once it carries a cookie
    Welcome = 2,  // host -> client: host puid + roster, echoes client nonce
    Roster = 3,   // host -> clients: current direct members
    Data = 4,     // game packet
    Ack = 5,      // reliable-link acknowledgement
    Ping = 6,
    Pong = 7,
    Bye = 8,
    Challenge = 9,  // host -> client: cookie for the client's address; hello again with it, signed
    // host -> a sender it has no link with (e.g. the host restarted). Unauthenticated, so only a hint:
    // a joiner acts on it only when its link has gone quiet anyway.
    Reset = 10,
    Forward = 11,  // sender -> receiver: every sequence number below this is acknowledged or given up
};

struct HelloMsg {
    uint32_t nonce = 0;
    uint64_t session = 0;  // Identity::nextSession(): newer sessions have higher numbers
    std::string puid;
    Cookie cookie{};  // all zero until the host sent one
    PublicKey publicKey{};
    PublicKey ecdh{};  // the client's ephemeral key for this session
    Signature signature{};  // over helloDigest()
};

struct ChallengeMsg {
    uint32_t clientNonce = 0;
    Cookie cookie{};
};

struct WelcomeMsg {
    uint32_t hostNonce = 0;
    uint32_t clientNonce = 0;
    std::string hostPuid;
    std::vector<std::string> roster;
    PublicKey ecdh{};  // the host's ephemeral key for this link
    PublicKey publicKey{};  // the host's identity, whose commitment the room owner published
    Signature signature{};  // over welcomeDigest()
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
    // EOS_EPacketReliability the game sent it with. With a sequence number and reliability 0 it is an
    // unreliable packet carried reliably until a deadline (DirectOptions::upgradeUnreliable).
    uint8_t reliability = 0;
    std::vector<uint8_t> payload;
};

// Sequence numbers first .. first + count - 1, all received.
struct AckRange {
    uint32_t first = 0;
    uint16_t count = 0;
};

// Ranges one Ack carries at most (6 bytes each). A receiver holding more gaps than this names the
// lowest ones and the one the acknowledged packet is in (see ReliableReceiver), so every packet it
// holds is acknowledged at the latest when the sender resends it.
constexpr size_t kMaxAckRanges = 32;

struct AckMsg {
    uint32_t cumulative = 0;  // every seq <= cumulative received (or given up by the sender)
    std::vector<AckRange> ranges;  // received above the cumulative point

    bool has(uint32_t seq) const {
        if (seq <= cumulative) return true;
        for (const AckRange& r : ranges)
            if (seq >= r.first && uint64_t{seq} < uint64_t{r.first} + r.count) return true;
        return false;
    }
};

// Like PR-SCTP's FORWARD-TSN: the sender will not send any sequence number below `floor` again, and
// every one of them it did not see acknowledged it gave up (an unreliable game packet past its
// deadline). The receiver moves its cumulative point there instead of waiting for them.
struct ForwardMsg {
    uint32_t floor = 0;
};

struct PingMsg {
    uint64_t timeMs = 0;
};

struct Message {
    MsgType type = MsgType::Ping;
    // Link session id of a link message, derived from both sides' hello nonces: it tells the receiver
    // which link's key to check the datagram with. Packets of an older session of the same link (in
    // flight across a reconnect) carry a different epoch and are dropped.
    uint32_t epoch = 0;
    uint64_t counter = 0;  // link messages: the sender's datagram counter, see sealLink()
    HelloMsg hello;
    ChallengeMsg challenge;
    WelcomeMsg welcome;
    RosterMsg roster;
    DataMsg data;
    AckMsg ack;
    ForwardMsg forward;
    PingMsg ping;  // also used for Pong
};

// Messages sent only on an established link, authenticated with its keys (see sealLink()).
inline bool isLinkScoped(MsgType t) {
    return t == MsgType::Data || t == MsgType::Ack || t == MsgType::Forward || t == MsgType::Ping ||
           t == MsgType::Pong || t == MsgType::Roster || t == MsgType::Bye;
}

inline uint32_t linkEpoch(uint32_t clientNonce, uint32_t hostNonce) {
    return clientNonce * 0x9E3779B1u ^ hostNonce;
}

// Encodes a message. A handshake message gets an HMAC tag when `key` is non-empty. A link message
// ignores `key` and leaves room for its counter and tag: sealLink() fills them in before sending.
std::vector<uint8_t> encode(const Message& msg, const std::string& key);

// Stamps an encoded link message with `counter` and its tag; false when crypto fails. May be called
// again on the same datagram (a retransmission goes out with a new counter).
bool sealLink(std::vector<uint8_t>& datagram, uint64_t counter, LinkMac& mac);
// True when a link datagram carries a valid tag for `mac`, i.e. its sender holds the link key.
bool linkTagValid(const uint8_t* data, size_t size, LinkMac& mac);

enum class DecodeError { None, BadMagic, BadProtocol, Truncated, Malformed, TagMissing, TagUnexpected, TagMismatch };

// Decodes a datagram and checks a handshake message's tag. `key` must match the sender's key (both
// empty is fine). A link message's tag is not checked here: the caller knows the link (linkTagValid).
std::optional<Message> decode(const uint8_t* data, size_t size, const std::string& key, DecodeError* err);

const char* decodeErrorName(DecodeError e);

// What a proven hello signs: its cookie, nonce, session, id and ECDH key. nullopt when crypto fails.
std::optional<Digest> helloDigest(const HelloMsg& hello);
// What a host signs in its welcome: both nonces, its id, both ECDH keys, its identity and the member
// list. The client's ECDH key is new every session, so a welcome answers exactly one hello.
std::optional<Digest> welcomeDigest(const WelcomeMsg& welcome, const PublicKey& clientEcdh);

struct LinkKeys {
    LinkKey clientToHost{};
    LinkKey hostToClient{};
};

// Per-link keys: HMAC-SHA256 key derivation from the ECDH shared secret (salted with the shared Key
// when set), bound to both nonces, the client's session and id, the host's id and both ECDH keys.
// nullopt when crypto fails.
std::optional<LinkKeys> deriveLinkKeys(const Digest& shared, const std::string& key, const HelloMsg& hello,
                                       const WelcomeMsg& welcome);

// Takes each link datagram counter at most once: anything newer than the highest seen, and older
// ones up to kSize back that have not arrived yet (the network may reorder). Counters start at 1.
class ReplayWindow {
public:
    static constexpr uint64_t kSize = 1024;
    bool fresh(uint64_t counter) const;
    void mark(uint64_t counter);
    uint64_t highest() const { return highest_; }

private:
    bool seen(uint64_t counter) const { return (bits_[(counter % kSize) / 64] >> (counter % 64)) & 1u; }
    void set(uint64_t counter, bool on);

    uint64_t highest_ = 0;
    std::array<uint64_t, kSize / 64> bits_{};  // counters in (highest_ - kSize, highest_], by counter % kSize
};

}  // namespace dn
