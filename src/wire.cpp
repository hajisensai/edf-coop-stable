#include "wire.h"

#include <cstring>
#include <utility>

#include "auth.h"

namespace dn {
namespace {

// Members a Welcome or one Roster/Room page names (kRosterPage), the host included. A Welcome of 32 EOS ids
// is 1306 bytes with a Key tag (each more adds 33); 64 would not fit the 2048-byte receive buffer. Larger rooms
// send their lists in pages (RosterMsg::total/offset); a Welcome names the first 32 and the pages follow.
constexpr size_t kMaxRoster = kRosterPage;
constexpr size_t kHeaderBytes = 8;
constexpr size_t kCounterOffset = kHeaderBytes + 4;  // after the epoch
constexpr size_t kLinkOverhead = kCounterOffset + 8 + kLinkTagBytes;

class Writer {
public:
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void str(const std::string& s) {
        size_t n = s.size() < kMaxString ? s.size() : kMaxString;
        u8(static_cast<uint8_t>(n));
        raw(s.data(), n);
    }
    void raw(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    bool malformed() const { return malformed_; }
    // A field no valid sender writes (a length over its limit): the datagram is rejected.
    void reject() { malformed_ = true; }
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    template <size_t N>
    void fixed(std::array<uint8_t, N>& out) { raw(out.data(), N); }
    std::string str() {
        size_t n = u8();
        if (n > kMaxString) reject();
        if (malformed_ || !need(n)) return {};
        std::string s(reinterpret_cast<const char*>(p_ + pos_), n);
        pos_ += n;
        return s;
    }
    std::vector<uint8_t> bytes(size_t n) {
        if (!need(n)) return {};
        std::vector<uint8_t> v(p_ + pos_, p_ + pos_ + n);
        pos_ += n;
        return v;
    }

private:
    bool need(size_t n) {
        if (!ok_ || n_ - pos_ < n) ok_ = false;
        return ok_;
    }
    void raw(void* out, size_t n) {
        if (need(n)) {
            memcpy(out, p_ + pos_, n);
            pos_ += n;
        }
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
    bool malformed_ = false;
};

void writeRoster(Writer& w, const std::vector<std::string>& roster) {
    size_t n = roster.size() < kMaxRoster ? roster.size() : kMaxRoster;
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) w.str(roster[i]);
}

std::vector<std::string> readRoster(Reader& r) {
    size_t n = r.u8();
    if (n > kMaxRoster) {
        r.reject();
        return {};
    }
    std::vector<std::string> roster(n);
    for (auto& s : roster) s = r.str();
    return roster;
}

// A page of a member list (RosterMsg): which part of the whole list it is. Its members must lie inside the list.
void writePage(Writer& w, uint32_t version, uint16_t total, uint16_t offset, size_t count) {
    // A list of one page written without its paging filled in (total 0) is the whole list.
    w.u32(version);
    w.u16(total ? total : static_cast<uint16_t>(count));
    w.u16(offset);
}

void readPage(Reader& r, uint32_t& version, uint16_t& total, uint16_t& offset, size_t count) {
    version = r.u32();
    total = r.u16();
    offset = r.u16();
    if (r.ok() && size_t{offset} + count > total) r.reject();
}

void writeBody(Writer& w, const Message& m) {
    if (isLinkScoped(m.type)) {
        w.u32(m.epoch);
        w.u64(m.counter);
    }
    switch (m.type) {
        case MsgType::Hello:
            w.u32(m.hello.nonce);
            w.u64(m.hello.session);
            w.str(m.hello.puid);
            w.raw(m.hello.cookie.data(), m.hello.cookie.size());
            w.raw(m.hello.publicKey.data(), m.hello.publicKey.size());
            w.raw(m.hello.ecdh.data(), m.hello.ecdh.size());
            w.raw(m.hello.signature.data(), m.hello.signature.size());
            w.u32(m.hello.netProtocol);
            w.u32(m.hello.netCaps);
            break;
        case MsgType::Challenge:
            w.u32(m.challenge.clientNonce);
            w.raw(m.challenge.cookie.data(), m.challenge.cookie.size());
            break;
        case MsgType::Welcome:
            w.u32(m.welcome.hostNonce);
            w.u32(m.welcome.clientNonce);
            w.str(m.welcome.hostPuid);
            writeRoster(w, m.welcome.roster);
            w.raw(m.welcome.ecdh.data(), m.welcome.ecdh.size());
            w.raw(m.welcome.publicKey.data(), m.welcome.publicKey.size());
            w.raw(m.welcome.signature.data(), m.welcome.signature.size());
            break;
        case MsgType::Roster:
            w.u32(m.roster.hostNonce);
            writeRoster(w, m.roster.roster);
            writePage(w, m.roster.version, m.roster.total, m.roster.offset, m.roster.roster.size());
            break;
        case MsgType::Room:
            w.u32(m.room.hostNonce);
            writeRoster(w, m.room.members);
            writePage(w, m.room.version, m.room.total, m.room.offset, m.room.members.size());
            break;
        case MsgType::PeerQuery:
            w.str(m.peer.puid);
            break;
        case MsgType::PeerInfo:
            w.str(m.peer.puid);
            w.str(m.peer.address);
            break;
        case MsgType::Punch:
            break;
        case MsgType::Data:
            w.u32(m.data.seq);
            w.str(m.data.src);
            w.str(m.data.dst);
            w.str(m.data.socketName);
            w.u8(m.data.channel);
            w.u8(m.data.reliability);
            w.u16(static_cast<uint16_t>(m.data.payload.size()));
            w.raw(m.data.payload.data(), m.data.payload.size());
            w.u8(m.data.cls);
            break;
        case MsgType::Ack: {
            w.u32(m.ack.cumulative);
            size_t n = m.ack.ranges.size() < kMaxAckRanges ? m.ack.ranges.size() : kMaxAckRanges;
            w.u8(static_cast<uint8_t>(n));
            for (size_t i = 0; i < n; ++i) {
                w.u32(m.ack.ranges[i].first);
                w.u16(m.ack.ranges[i].count);
            }
            break;
        }
        case MsgType::Forward:
            w.u32(m.forward.floor);
            break;
        case MsgType::Ping:
        case MsgType::Pong:
            w.u64(m.ping.timeMs);
            break;
        case MsgType::Bye:
        case MsgType::Reset:
            break;
    }
}

bool readBody(Reader& r, Message& m) {
    if (isLinkScoped(m.type)) {
        m.epoch = r.u32();
        m.counter = r.u64();
    }
    switch (m.type) {
        case MsgType::Hello:
            m.hello.nonce = r.u32();
            m.hello.session = r.u64();
            m.hello.puid = r.str();
            r.fixed(m.hello.cookie);
            r.fixed(m.hello.publicKey);
            r.fixed(m.hello.ecdh);
            r.fixed(m.hello.signature);
            m.hello.netProtocol = r.u32();
            m.hello.netCaps = r.u32();
            return true;
        case MsgType::Challenge:
            m.challenge.clientNonce = r.u32();
            r.fixed(m.challenge.cookie);
            return true;
        case MsgType::Welcome:
            m.welcome.hostNonce = r.u32();
            m.welcome.clientNonce = r.u32();
            m.welcome.hostPuid = r.str();
            m.welcome.roster = readRoster(r);
            r.fixed(m.welcome.ecdh);
            r.fixed(m.welcome.publicKey);
            r.fixed(m.welcome.signature);
            return true;
        case MsgType::Roster:
            m.roster.hostNonce = r.u32();
            m.roster.roster = readRoster(r);
            readPage(r, m.roster.version, m.roster.total, m.roster.offset, m.roster.roster.size());
            return true;
        case MsgType::Room:
            m.room.hostNonce = r.u32();
            m.room.members = readRoster(r);
            readPage(r, m.room.version, m.room.total, m.room.offset, m.room.members.size());
            return true;
        case MsgType::PeerQuery:
            m.peer.puid = r.str();
            return true;
        case MsgType::PeerInfo:
            m.peer.puid = r.str();
            m.peer.address = r.str();
            return true;
        case MsgType::Punch:
            return true;
        case MsgType::Data:
            m.data.seq = r.u32();
            m.data.src = r.str();
            m.data.dst = r.str();
            m.data.socketName = r.str();
            m.data.channel = r.u8();
            m.data.reliability = r.u8();
            if (size_t n = r.u16(); n <= kMaxPayload)
                m.data.payload = r.bytes(n);
            else
                r.reject();
            m.data.cls = r.u8();
            if (r.ok() && m.data.cls > 3) r.reject();  // TrafficClass has four values
            return true;
        case MsgType::Ack: {
            m.ack.cumulative = r.u32();
            size_t n = r.u8();
            if (n > kMaxAckRanges) {
                r.reject();
                return true;
            }
            m.ack.ranges.resize(n);
            for (AckRange& range : m.ack.ranges) {
                range.first = r.u32();
                range.count = r.u16();
                if (r.ok() && range.count == 0) r.reject();  // an empty range: no valid sender writes one
            }
            return true;
        }
        case MsgType::Forward:
            m.forward.floor = r.u32();
            return true;
        case MsgType::Ping:
        case MsgType::Pong:
            m.ping.timeMs = r.u64();
            return true;
        case MsgType::Bye:
        case MsgType::Reset:
            return true;
    }
    return false;
}

void writeKey(Writer& w, const PublicKey& key) { w.raw(key.data(), key.size()); }

}  // namespace

std::vector<uint8_t> encode(const Message& msg, const std::string& key) {
    bool link = isLinkScoped(msg.type);
    Writer w;
    w.u32(kMagic);
    w.u8(static_cast<uint8_t>(msg.type));
    w.u8(key.empty() || link ? 0 : kFlagTagged);
    w.u16(kProtocol);
    writeBody(w, msg);
    if (link) {
        w.buf.resize(w.buf.size() + kLinkTagBytes);  // sealLink() writes the tag
    } else if (!key.empty()) {
        // Without a tag (crypto failure) the zero bytes simply fail verification on the other side.
        auto tag = hmacTag(key, w.buf.data(), w.buf.size()).value_or(std::array<uint8_t, kTagBytes>{});
        w.raw(tag.data(), tag.size());
    }
    return std::move(w.buf);
}

std::optional<Message> decode(const uint8_t* data, size_t size, const std::string& key, DecodeError* err) {
    auto fail = [&](DecodeError e) -> std::optional<Message> {
        if (err) *err = e;
        return std::nullopt;
    };
    if (err) *err = DecodeError::None;
    if (size < kHeaderBytes) return fail(DecodeError::Truncated);
    Reader header(data, kHeaderBytes);
    if (header.u32() != kMagic) return fail(DecodeError::BadMagic);
    Message m;
    m.type = static_cast<MsgType>(header.u8());
    uint8_t flags = header.u8();
    if (header.u16() != kProtocol) return fail(DecodeError::BadProtocol);

    size_t bodyEnd = size;
    bool tagged = (flags & kFlagTagged) != 0;
    if (isLinkScoped(m.type)) {
        // The shared Key is part of the link keys (deriveLinkKeys): the link tag covers it.
        if (size < kLinkOverhead) return fail(DecodeError::Truncated);
        bodyEnd = size - kLinkTagBytes;
    } else if (!key.empty() && !tagged) {
        return fail(DecodeError::TagMissing);
    } else if (key.empty() && tagged) {
        return fail(DecodeError::TagUnexpected);
    } else if (tagged) {
        if (size < kHeaderBytes + kTagBytes) return fail(DecodeError::Truncated);
        bodyEnd = size - kTagBytes;
        auto expect = hmacTag(key, data, bodyEnd);
        uint8_t diff = expect ? 0 : 1;  // no tag computed: reject
        for (size_t i = 0; expect && i < kTagBytes; ++i) diff |= (*expect)[i] ^ data[bodyEnd + i];  // constant time
        if (diff) return fail(DecodeError::TagMismatch);
    }
    Reader body(data + kHeaderBytes, bodyEnd - kHeaderBytes);
    bool known = readBody(body, m);  // false: a message type this version does not know
    if (!known || body.malformed()) return fail(DecodeError::Malformed);
    if (!body.ok()) return fail(DecodeError::Truncated);
    return m;
}

bool sealLink(std::vector<uint8_t>& datagram, uint64_t counter, LinkMac& mac) {
    if (datagram.size() < kLinkOverhead) return false;
    memcpy(datagram.data() + kCounterOffset, &counter, 8);
    size_t body = datagram.size() - kLinkTagBytes;
    return mac.tag(datagram.data(), body, datagram.data() + body);
}

bool linkTagValid(const uint8_t* data, size_t size, LinkMac& mac) {
    if (size < kLinkOverhead) return false;
    size_t body = size - kLinkTagBytes;
    return mac.check(data, body, data + body);
}

std::optional<Digest> helloDigest(const HelloMsg& hello) {
    Writer w;
    w.raw("EDF6DN hello 4", 14);
    w.raw(hello.cookie.data(), hello.cookie.size());
    w.u32(hello.nonce);
    w.u64(hello.session);
    w.str(hello.puid);
    writeKey(w, hello.ecdh);
    w.u32(hello.netProtocol);
    w.u32(hello.netCaps);
    return sha256(w.buf.data(), w.buf.size());
}

std::optional<Digest> welcomeDigest(const WelcomeMsg& welcome, const PublicKey& clientEcdh) {
    Writer w;
    w.raw("EDF6DN welcome 4", 16);
    w.u32(welcome.hostNonce);
    w.u32(welcome.clientNonce);
    w.str(welcome.hostPuid);
    writeKey(w, clientEcdh);
    writeKey(w, welcome.ecdh);
    writeKey(w, welcome.publicKey);
    writeRoster(w, welcome.roster);
    return sha256(w.buf.data(), w.buf.size());
}

std::optional<LinkKeys> deriveLinkKeys(const Digest& shared, const std::string& key, const HelloMsg& hello,
                                       const WelcomeMsg& welcome) {
    // Extract: a pseudorandom key from the shared secret, salted with the shared Key (if any).
    auto prk = hmacSha256("EDF6DN link 4|" + key, shared.data(), shared.size());
    if (!prk) return std::nullopt;
    // Expand: one key per direction, bound to this handshake. The cookie is left out: a client may
    // get a new one between its hello and the host's answer.
    std::string prkKey(reinterpret_cast<const char*>(prk->data()), prk->size());
    Writer info;
    info.u32(hello.nonce);
    info.u32(welcome.hostNonce);
    info.u32(linkEpoch(hello.nonce, welcome.hostNonce));
    info.u64(hello.session);
    info.str(hello.puid);
    info.str(welcome.hostPuid);
    writeKey(info, hello.ecdh);
    writeKey(info, welcome.ecdh);
    LinkKeys keys;
    for (auto [label, out] : {std::pair<const char*, LinkKey*>{"client->host", &keys.clientToHost},
                              std::pair<const char*, LinkKey*>{"host->client", &keys.hostToClient}}) {
        std::vector<uint8_t> in(label, label + strlen(label));
        in.insert(in.end(), info.buf.begin(), info.buf.end());
        auto k = hmacSha256(prkKey, in.data(), in.size());
        if (!k) return std::nullopt;
        *out = *k;
    }
    return keys;
}

bool ReplayWindow::fresh(uint64_t counter) const {
    if (counter == 0) return false;
    if (counter > highest_) return true;
    return highest_ - counter < kSize && !seen(counter);
}

void ReplayWindow::set(uint64_t counter, bool on) {
    uint64_t& word = bits_[(counter % kSize) / 64];
    uint64_t bit = uint64_t{1} << (counter % 64);
    word = on ? word | bit : word & ~bit;
}

void ReplayWindow::mark(uint64_t counter) {
    if (counter > highest_) {
        // The slots of the counters skipped over held counters now out of the window.
        if (counter - highest_ >= kSize)
            bits_.fill(0);
        else
            for (uint64_t c = highest_ + 1; c < counter; ++c) set(c, false);
        highest_ = counter;
    }
    set(counter, true);
}

const char* decodeErrorName(DecodeError e) {
    switch (e) {
        case DecodeError::None: return "none";
        case DecodeError::BadMagic: return "bad-magic";
        case DecodeError::BadProtocol: return "protocol-version-mismatch";
        case DecodeError::Truncated: return "truncated";
        case DecodeError::Malformed: return "malformed";
        case DecodeError::TagMissing: return "key-missing-on-sender";
        case DecodeError::TagUnexpected: return "key-missing-on-receiver";
        case DecodeError::TagMismatch: return "key-mismatch";
    }
    return "?";
}

}  // namespace dn
