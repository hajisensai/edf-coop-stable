#include "worldrng.h"

#include <cstring>
#include <iterator>

namespace multislot {
namespace {

constexpr std::uint16_t kRemoteOwned = 1, kLocallyOwned = 2;  // NetworkObject flags (782880)

void Put32(std::uint8_t* at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) at[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void Put64(std::uint8_t* at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) at[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint32_t Get32(const std::uint8_t* at) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(at[i]) << (8 * i);
    return value;
}
std::uint64_t Get64(const std::uint8_t* at) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(at[i]) << (8 * i);
    return value;
}

// See RngReplicatedByGame; checked against EDF.dll by NetWorldSitesMatchEDF.
constexpr std::uint32_t kReplicatedVtables[] = {
    0x17BB990,  // GiantAnt
    0x17BC840,  // GiantAntEX
    0x17BD0C0,  // GiantBee
    0x17BD818,  // GiantBeeEX
    0x17BE308,  // GiantSpider
    0x17BE9C8,  // GiantSpiderEX
    0x17C8638,  // Nephila
    0x17B8A58,  // EDF6_SquidSmall
};

}  // namespace

std::size_t WriteRngSync(const RngSync& sync, std::uint8_t* out, std::size_t capacity) {
    if (!out || capacity < kRngSyncBytes) return 0;
    out[0] = kRngSyncMagic0;
    out[1] = kRngSyncMagic1;
    out[2] = kRngSyncVersion;
    Put32(out + 3, sync.seq);
    Put64(out + 7, sync.state);
    Put64(out + 15, sync.state2);
    return kRngSyncBytes;
}

bool ReadRngSync(const std::uint8_t* data, std::size_t size, RngSync& out) {
    if (!data || size != kRngSyncBytes || data[0] != kRngSyncMagic0 || data[1] != kRngSyncMagic1 ||
        data[2] != kRngSyncVersion)
        return false;
    out.seq = Get32(data + 3);
    out.state = Get64(data + 7);
    out.state2 = Get64(data + 15);
    return true;
}

bool RngReplicatedByGame(std::uint32_t networkVtableRva) {
    for (const std::uint32_t vtable : kReplicatedVtables)
        if (vtable == networkVtableRva) return true;
    return false;
}

bool ShouldSyncRng(const RngSendView& view) {
    const bool owner = (view.netFlags & kLocallyOwned) != 0 && (view.netFlags & kRemoteOwned) == 0;
    return view.active && view.online && owner && view.team == kEnemyTeam && view.networkVtableRva != 0 &&
           !RngReplicatedByGame(view.networkVtableRva);
}

void RngSchedule::Prune(std::uint64_t nowMs) {
    if (nowMs - pruned_ < kForgetMs) return;
    pruned_ = nowMs;
    for (auto it = last_.begin(); it != last_.end();)
        it = nowMs - it->second > kForgetMs ? last_.erase(it) : std::next(it);
}

bool RngSchedule::Due(std::uint64_t object, std::uint64_t nowMs, std::uint32_t intervalMs) {
    Prune(nowMs);
    const auto found = last_.find(object);
    if (found != last_.end() && nowMs - found->second < intervalMs) return false;
    last_[object] = nowMs;
    return true;
}

void RngReceiver::Prune(std::uint64_t nowMs) {
    if (nowMs - pruned_ < kForgetMs) return;
    pruned_ = nowMs;
    for (auto it = last_.begin(); it != last_.end();)
        it = nowMs - it->second.at > kForgetMs ? last_.erase(it) : std::next(it);
}

bool RngReceiver::Accept(std::uint64_t object, std::uint32_t seq, std::uint64_t nowMs) {
    Prune(nowMs);
    const auto found = last_.find(object);
    if (found != last_.end() && static_cast<std::int32_t>(seq - found->second.seq) <= 0) {
        found->second.at = nowMs;
        return false;
    }
    last_[object] = {seq, nowMs};
    return true;
}

}  // namespace multislot
