#include "wire.h"

#include <cstring>

#include "auth.h"

namespace dn {
namespace {

constexpr size_t kMaxString = 64;
constexpr size_t kMaxRoster = 32;
constexpr size_t kHeaderBytes = 8;

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
    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    std::string str() {
        size_t n = u8();
        if (!need(n)) return {};
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
};

void writeRoster(Writer& w, const std::vector<std::string>& roster) {
    size_t n = roster.size() < kMaxRoster ? roster.size() : kMaxRoster;
    w.u8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) w.str(roster[i]);
}

std::vector<std::string> readRoster(Reader& r) {
    std::vector<std::string> roster(r.u8());
    for (auto& s : roster) s = r.str();
    return roster;
}

void writeBody(Writer& w, const Message& m) {
    if (isLinkScoped(m.type)) w.u32(m.epoch);
    switch (m.type) {
        case MsgType::Hello:
            w.u32(m.hello.nonce);
            w.str(m.hello.puid);
            break;
        case MsgType::Welcome:
            w.u32(m.welcome.hostNonce);
            w.u32(m.welcome.clientNonce);
            w.str(m.welcome.hostPuid);
            writeRoster(w, m.welcome.roster);
            break;
        case MsgType::Roster:
            w.u32(m.roster.hostNonce);
            writeRoster(w, m.roster.roster);
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
            break;
        case MsgType::Ack:
            w.u32(m.ack.cumulative);
            for (uint32_t word : m.ack.bitmap) w.u32(word);
            break;
        case MsgType::Ping:
        case MsgType::Pong:
            w.u64(m.ping.timeMs);
            break;
        case MsgType::Bye:
            break;
    }
}

bool readBody(Reader& r, Message& m) {
    if (isLinkScoped(m.type)) m.epoch = r.u32();
    switch (m.type) {
        case MsgType::Hello:
            m.hello.nonce = r.u32();
            m.hello.puid = r.str();
            return true;
        case MsgType::Welcome:
            m.welcome.hostNonce = r.u32();
            m.welcome.clientNonce = r.u32();
            m.welcome.hostPuid = r.str();
            m.welcome.roster = readRoster(r);
            return true;
        case MsgType::Roster:
            m.roster.hostNonce = r.u32();
            m.roster.roster = readRoster(r);
            return true;
        case MsgType::Data:
            m.data.seq = r.u32();
            m.data.src = r.str();
            m.data.dst = r.str();
            m.data.socketName = r.str();
            m.data.channel = r.u8();
            m.data.reliability = r.u8();
            m.data.payload = r.bytes(r.u16());
            return true;
        case MsgType::Ack:
            m.ack.cumulative = r.u32();
            for (uint32_t& word : m.ack.bitmap) word = r.u32();
            return true;
        case MsgType::Ping:
        case MsgType::Pong:
            m.ping.timeMs = r.u64();
            return true;
        case MsgType::Bye:
            return true;
    }
    return false;
}

}  // namespace

std::vector<uint8_t> encode(const Message& msg, const std::string& key) {
    Writer w;
    w.u32(kMagic);
    w.u8(static_cast<uint8_t>(msg.type));
    w.u8(key.empty() ? 0 : kFlagTagged);
    w.u16(kProtocol);
    writeBody(w, msg);
    if (!key.empty()) {
        auto tag = hmacTag(key, w.buf.data(), w.buf.size());
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

    bool tagged = (flags & kFlagTagged) != 0;
    if (!key.empty() && !tagged) return fail(DecodeError::TagMissing);
    if (key.empty() && tagged) return fail(DecodeError::TagUnexpected);
    size_t bodyEnd = size;
    if (tagged) {
        if (size < kHeaderBytes + kTagBytes) return fail(DecodeError::Truncated);
        bodyEnd = size - kTagBytes;
        auto expect = hmacTag(key, data, bodyEnd);
        if (memcmp(expect.data(), data + bodyEnd, kTagBytes) != 0) return fail(DecodeError::TagMismatch);
    }
    Reader body(data + kHeaderBytes, bodyEnd - kHeaderBytes);
    if (!readBody(body, m) || !body.ok()) return fail(DecodeError::Truncated);
    return m;
}

const char* decodeErrorName(DecodeError e) {
    switch (e) {
        case DecodeError::None: return "none";
        case DecodeError::BadMagic: return "bad-magic";
        case DecodeError::BadProtocol: return "protocol-version-mismatch";
        case DecodeError::Truncated: return "truncated";
        case DecodeError::TagMissing: return "key-missing-on-sender";
        case DecodeError::TagUnexpected: return "key-missing-on-receiver";
        case DecodeError::TagMismatch: return "key-mismatch";
    }
    return "?";
}

}  // namespace dn
