#include "fragment.h"

#include <algorithm>
#include <cstring>

namespace dn {
namespace {

constexpr uint8_t kMagic[4] = {'E', 'D', 'F', 'G'};
constexpr uint8_t kAckMagic[4] = {'E', 'D', 'F', 'A'};
constexpr uint8_t kVersion = 1;

template <typename T>
void put(uint8_t* at, T value) {
    memcpy(at, &value, sizeof(value));
}
template <typename T>
T get(const uint8_t* at) {
    T value{};
    memcpy(&value, at, sizeof(value));
    return value;
}

size_t fragmentsFor(size_t total) { return (total + kFragmentPayload - 1) / kFragmentPayload; }

}  // namespace

std::vector<std::vector<uint8_t>> splitIntoFragments(uint32_t id, uint8_t flags, uint16_t tag, const uint8_t* data,
                                                     size_t size) {
    std::vector<std::vector<uint8_t>> out;
    if (!data || size == 0 || size > kMaxFragmentedBytes) return out;
    const size_t count = fragmentsFor(size);
    for (size_t i = 0; i < count; ++i) {
        const size_t at = i * kFragmentPayload;
        const size_t n = std::min(kFragmentPayload, size - at);
        std::vector<uint8_t>& f = out.emplace_back(kFragmentHeader + n);
        memcpy(f.data(), kMagic, 4);
        f[4] = kVersion;
        f[5] = flags;
        put<uint16_t>(f.data() + 6, tag);
        put<uint32_t>(f.data() + 8, id);
        put<uint32_t>(f.data() + 12, static_cast<uint32_t>(size));
        put<uint16_t>(f.data() + 16, static_cast<uint16_t>(i));
        put<uint16_t>(f.data() + 18, static_cast<uint16_t>(count));
        memcpy(f.data() + kFragmentHeader, data + at, n);
    }
    return out;
}

std::vector<uint8_t> fragmentAck(uint32_t id) {
    std::vector<uint8_t> out(kFragmentAckBytes);
    memcpy(out.data(), kAckMagic, 4);
    put<uint32_t>(out.data() + 4, id);
    return out;
}

bool parseFragmentAck(const uint8_t* data, size_t size, uint32_t& id) {
    if (!data || size != kFragmentAckBytes || memcmp(data, kAckMagic, 4) != 0) return false;
    id = get<uint32_t>(data + 4);
    return true;
}

uint32_t Reassembler::idOf(const uint8_t* data, size_t size) {
    return isFragment(data, size) ? get<uint32_t>(data + 8) : 0;
}

bool isFragment(const uint8_t* data, size_t size) {
    return data && size >= kFragmentHeader && memcmp(data, kMagic, 4) == 0;
}

void Reassembler::drop(Map::iterator it) {
    buffered_ -= it->second.bytes.size();
    partial_.erase(it);
    ++dropped_;
}

void Reassembler::expire(uint64_t nowMs) {
    for (auto it = partial_.begin(); it != partial_.end();) {
        auto next = std::next(it);
        if (nowMs - it->second.firstMs > kTimeoutMs) drop(it);
        it = next;
    }
}

std::optional<FragmentMessage> Reassembler::add(const std::string& src, const uint8_t* data, size_t size,
                                                uint64_t nowMs, bool* again) {
    expire(nowMs);
    if (again) *again = false;
    for (auto it = completed_.begin(); it != completed_.end();)
        it = nowMs - it->second > kCompletedMs ? completed_.erase(it) : std::next(it);
    if (!isFragment(data, size) || data[4] != kVersion) {
        ++malformed_;
        return std::nullopt;
    }
    const uint8_t flags = data[5];
    const uint16_t tag = get<uint16_t>(data + 6);
    const uint32_t id = get<uint32_t>(data + 8);
    const uint32_t total = get<uint32_t>(data + 12);
    const uint16_t index = get<uint16_t>(data + 16);
    const uint16_t count = get<uint16_t>(data + 18);
    const size_t n = size - kFragmentHeader;
    // What a valid sender writes: the count its total needs, a full payload in all but the last fragment.
    const bool valid = total > 0 && total <= kMaxFragmentedBytes && count == fragmentsFor(total) && index < count &&
                       n == std::min<size_t>(kFragmentPayload, total - size_t{index} * kFragmentPayload);
    if (!valid) {
        ++malformed_;
        return std::nullopt;
    }
    const auto key = std::make_pair(src, id);
    if (completed_.count(key)) {  // arrived whole already: a resend or a copy
        if (again) *again = true;
        return std::nullopt;
    }
    auto it = partial_.find(key);
    if (it == partial_.end()) {
        size_t mine = 0;
        auto oldest = partial_.end();
        for (auto p = partial_.lower_bound({src, 0}); p != partial_.end() && p->first.first == src; ++p) {
            ++mine;
            if (oldest == partial_.end() || p->second.firstMs < oldest->second.firstMs) oldest = p;
        }
        if (mine >= kMaxPartialPerSender && oldest != partial_.end()) drop(oldest);
        // Room for the whole message, the oldest message of anyone making way.
        while (buffered_ + total > kMaxBuffered && !partial_.empty()) {
            auto first = std::min_element(partial_.begin(), partial_.end(),
                                          [](const auto& a, const auto& b) { return a.second.firstMs < b.second.firstMs; });
            drop(first);
        }
        Partial p;
        p.flags = flags;
        p.tag = tag;
        p.total = total;
        p.count = count;
        p.firstMs = nowMs;
        p.got.assign(count, false);
        p.bytes.assign(total, 0);
        buffered_ += total;
        it = partial_.emplace(key, std::move(p)).first;
    }
    Partial& p = it->second;
    if (p.total != total || p.count != count || p.flags != flags || p.tag != tag) {
        ++malformed_;  // another message under the same id: the sender's ids wrapped, or it lies
        return std::nullopt;
    }
    if (p.got[index]) return std::nullopt;  // a copy (resent, or sent over two paths)
    p.got[index] = true;
    ++p.have;
    memcpy(p.bytes.data() + size_t{index} * kFragmentPayload, data + kFragmentHeader, n);
    if (p.have < p.count) return std::nullopt;
    FragmentMessage done{p.flags, p.tag, std::move(p.bytes)};
    buffered_ -= total;
    partial_.erase(it);
    if (completed_.size() >= kCompletedKept) completed_.erase(completed_.begin());
    completed_[key] = nowMs;
    return done;
}

void Reassembler::clear() {
    partial_.clear();
    completed_.clear();
    buffered_ = 0;
}

}  // namespace dn
