#include "netclass.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dn {

uint64_t fnv1a64(const void* data, size_t size, uint64_t seed) {
    uint64_t h = seed;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

bool parseControllerDatagram(const uint8_t* plain, size_t size, std::vector<RecordInfo>& out) {
    out.clear();
    if (!plain || size < kControllerDatagramHeader) return false;
    size_t at = kControllerDatagramHeader;
    bool nextReliable = false;
    while (at < size) {
        if (size - at < 4) return false;
        uint32_t header = 0;
        memcpy(&header, plain + at, 4);
        const uint32_t length = header >> 20;
        const uint32_t type = header & 0xFFFFF;
        if (size - at - 4 < length) return false;
        at += 4 + static_cast<size_t>(length);
        if ((type & 0xFF00) == kRecordReliableEnvelope) {
            // The envelope (its sequence number) is the controller's; the record after it is the reliable one.
            out.push_back({type, length, false});
            nextReliable = true;
            continue;
        }
        out.push_back({type, length, nextReliable && !isControllerRecord(type)});
        nextReliable = false;
    }
    return true;
}

const char* trafficClassName(TrafficClass c) {
    switch (c) {
        case TrafficClass::State: return "state";
        case TrafficClass::Event: return "event";
        case TrafficClass::Control: return "control";
        case TrafficClass::Unknown: break;
    }
    return "unknown";
}

bool StateLearner::observe(const std::string& remote, uint32_t type, uint64_t nowMs) {
    if (type == kRecordEventBatch || isControllerRecord(type)) return false;
    std::lock_guard<std::mutex> lock(mu_);
    Run& run = runs_[{remote, type}];
    // Several records of a type in one datagram are one update, not a run of them.
    if (run.lastMs != 0 && nowMs == run.lastMs) return false;
    // A late update (the sender's frame hitched) halves the run instead of ending it; a type sent with gaps over and
    // over never gets there.
    if (run.lastMs != 0) run.steady = nowMs - run.lastMs <= kMaxIntervalMs ? run.steady + 1 : run.steady / 2;
    run.lastMs = nowMs;
    if (run.steady < kSamples) return false;
    bool& state = state_[type];
    const bool learnt = !state;
    state = true;
    return learnt;
}

bool StateLearner::isState(uint32_t type) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = state_.find(type);
    return it != state_.end() && it->second;
}

void StateLearner::forget() {
    std::lock_guard<std::mutex> lock(mu_);
    runs_.clear();
    state_.clear();
}

TrafficClass classify(const std::vector<RecordInfo>& records, bool parsed, const StateLearner& learner) {
    if (!parsed || records.empty()) return TrafficClass::Unknown;
    bool reliable = false, data = false, allState = true;
    for (const RecordInfo& r : records) {
        if (isControllerRecord(r.type)) continue;
        data = true;
        if (r.reliable) {
            reliable = true;
        } else if (!learner.isState(r.type)) {
            allState = false;
        }
    }
    if (!data) return TrafficClass::Control;
    // A reliable record makes the datagram an event even beside state: the game resends that record, and the
    // state next to it is replaced by the next update anyway. Unreliable records of an unknown type next to it
    // keep the old repair: nothing resends them.
    if (reliable) return allState ? TrafficClass::Event : TrafficClass::Unknown;
    return allState ? TrafficClass::State : TrafficClass::Unknown;
}

void RecordTypeMeter::record(const std::string& remote, const std::vector<RecordInfo>& records, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const RecordInfo& r : records) {
        Type& t = types_[r.type];
        ++t.records;
        t.bytes += 4 + r.size;
        t.largest = std::max(t.largest, r.size);
        if (r.reliable) ++t.reliable;
        uint64_t& last = last_[{remote, r.type}];
        if (last != 0 && nowMs > last) {
            const uint64_t gap = nowMs - last;
            ++t.intervals;
            t.intervalSumMs += gap;
            t.intervalMinMs = std::min(t.intervalMinMs, gap);
            t.intervalMaxMs = std::max(t.intervalMaxMs, gap);
        }
        last = nowMs;
    }
}

void RecordTypeMeter::recordDatagram(TrafficClass c, size_t bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto i = static_cast<size_t>(c) & 3;
    ++datagrams_[i];
    datagramBytes_[i] += bytes;
}

std::vector<std::string> RecordTypeMeter::take(double seconds, size_t maxLines) {
    std::map<uint32_t, Type> types;
    uint64_t datagrams[4], bytes[4];
    {
        std::lock_guard<std::mutex> lock(mu_);
        types.swap(types_);
        // Players who left would keep their last time forever: only intervals inside a period are measured.
        last_.clear();
        memcpy(datagrams, datagrams_, sizeof(datagrams));
        memcpy(bytes, datagramBytes_, sizeof(bytes));
        memset(datagrams_, 0, sizeof(datagrams_));
        memset(datagramBytes_, 0, sizeof(datagramBytes_));
    }
    std::vector<std::string> lines;
    if (seconds <= 0) seconds = 1;
    uint64_t total = 0;
    for (uint64_t d : datagrams) total += d;
    if (!total && types.empty()) return lines;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "NETCLASS datagrams: state %llu (%.0f kbps) event %llu (%.0f kbps) control %llu (%.0f kbps) unknown %llu "
             "(%.0f kbps), %llu without their plaintext",
             static_cast<unsigned long long>(datagrams[1]), bytes[1] * 8.0 / 1000.0 / seconds,
             static_cast<unsigned long long>(datagrams[2]), bytes[2] * 8.0 / 1000.0 / seconds,
             static_cast<unsigned long long>(datagrams[3]), bytes[3] * 8.0 / 1000.0 / seconds,
             static_cast<unsigned long long>(datagrams[0]), bytes[0] * 8.0 / 1000.0 / seconds,
             static_cast<unsigned long long>(unmatched_.exchange(0)));
    lines.emplace_back(buf);
    std::vector<std::pair<uint32_t, Type>> sorted(types.begin(), types.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    for (size_t i = 0; i < sorted.size() && i < maxLines; ++i) {
        const auto& [type, t] = sorted[i];
        const double avg = t.intervals ? static_cast<double>(t.intervalSumMs) / static_cast<double>(t.intervals) : 0.0;
        snprintf(buf, sizeof(buf),
                 "NETTYPE 0x%05X%s: %llu records (%.1f/s, %llu reliable), %.1f kbps, largest %u B, per player every "
                 "%.0f ms (min %llu, max %llu)",
                 type, isControllerRecord(type) ? " (controller)" : "", static_cast<unsigned long long>(t.records),
                 t.records / seconds, static_cast<unsigned long long>(t.reliable), t.bytes * 8.0 / 1000.0 / seconds,
                 t.largest, avg, static_cast<unsigned long long>(t.intervals ? t.intervalMinMs : 0),
                 static_cast<unsigned long long>(t.intervalMaxMs));
        lines.emplace_back(buf);
    }
    if (sorted.size() > maxLines) lines.push_back("NETTYPE ... and " + std::to_string(sorted.size() - maxLines) + " more types");
    return lines;
}

namespace {
struct Pending {
    bool set = false;
    size_t bytes = 0;
    PlainDatagram datagram;
};
thread_local Pending g_pending;
}  // namespace

void notePendingDatagram(const uint8_t* plain, size_t bytes) {
    g_pending.set = true;
    g_pending.bytes = bytes;
    g_pending.datagram.parsed = parseControllerDatagram(plain, bytes, g_pending.datagram.records);
}

void retargetPendingDatagram(size_t bytes) {
    if (g_pending.set) g_pending.bytes = bytes;
}

std::optional<PlainDatagram> takePendingDatagram(size_t bytes) {
    // Another size is a packet of the plugin's own sent in between (packetfit sends records ahead of the game's
    // datagram from inside the game's send): the noted datagram is still on its way.
    if (!g_pending.set || g_pending.bytes != bytes) return std::nullopt;
    g_pending.set = false;
    return std::move(g_pending.datagram);
}

bool DuplicateFilter::first(const std::string& src, const uint8_t* data, size_t size, uint64_t nowMs) {
    const uint64_t h = fnv1a64(data, size, fnv1a64(src.data(), src.size()));
    std::lock_guard<std::mutex> lock(mu_);
    std::deque<Seen>& seen = seen_[src];
    while (!seen.empty() && (seen.size() >= kWindow || nowMs - seen.front().ms > kWindowMs)) seen.pop_front();
    for (const Seen& s : seen)
        if (s.hash == h) {
            ++duplicates_;
            return false;
        }
    seen.push_back({h, nowMs});
    return true;
}

uint64_t DuplicateFilter::duplicates() const {
    std::lock_guard<std::mutex> lock(mu_);
    return duplicates_;
}

void DuplicateFilter::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    seen_.clear();
}

}  // namespace dn
