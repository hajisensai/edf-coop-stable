#include "traffic.h"

#include <algorithm>

namespace dn {

uint64_t TrafficMeter::hash(const void* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

void TrafficMeter::record(const std::string& remote, uint32_t size, uint64_t payloadHash, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    cur_.bytes += size;
    ++cur_.packets;
    cur_.largestPacket = std::max(cur_.largestPacket, size);
    peers_.insert(remote);
    if (payloadHash == lastHash_ && size == lastSize_ && remote != lastRemote_) cur_.copyBytes += size;
    lastHash_ = payloadHash;
    lastSize_ = size;
    lastRemote_ = remote;
    uint64_t second = nowMs / 1000;
    if (second != second_) {
        second_ = second;
        secondBytes_ = 0;
    }
    secondBytes_ += size;
    cur_.busiestSecondBytes = std::max(cur_.busiestSecondBytes, secondBytes_);
}

TrafficSummary TrafficMeter::take() {
    std::lock_guard<std::mutex> lock(mu_);
    TrafficSummary s = cur_;
    s.peers = peers_.size();
    cur_ = {};
    peers_.clear();
    secondBytes_ = 0;  // the running second's bytes were counted above; a new minute starts clean
    return s;
}

}  // namespace dn
