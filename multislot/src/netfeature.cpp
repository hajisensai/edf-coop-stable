#include "netfeature.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>

namespace multislot {
namespace {

constexpr std::uint32_t kFeatureCount = 1;
std::atomic<std::uint32_t> enabled{(1u << kFeatureCount) - 1};

}  // namespace

void LoadNetFeatures(const wchar_t* iniPath) {
    SetNetFeature(NetFeature::PlayerSync, GetPrivateProfileIntW(L"NetFeature", L"PlayerSync", 1, iniPath) != 0);
}

void SetNetFeature(NetFeature feature, bool on) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(feature);
    if (on)
        enabled.fetch_or(bit);
    else
        enabled.fetch_and(~bit);
}

bool NetFeatureActive(NetFeature feature) {
    return (enabled.load() >> static_cast<std::uint32_t>(feature) & 1u) != 0;
}

}  // namespace multislot
