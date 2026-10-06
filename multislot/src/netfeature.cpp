#include "netfeature.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>

namespace multislot {
namespace {

constexpr auto kFeatureCount = static_cast<std::size_t>(NetFeature::Count);
std::atomic<bool> active[kFeatureCount]{};

std::size_t Index(NetFeature feature) { return static_cast<std::size_t>(feature); }

}  // namespace

const wchar_t* NetFeatureKey(NetFeature feature) {
    switch (feature) {
        case NetFeature::EnemyTargets: return L"EnemyTargets";
        case NetFeature::Count: break;
    }
    return L"";
}

void InitNetFeatures(const wchar_t* iniPath) {
    // Spelled out per key (not through NetFeatureKey) so that DefaultIniTests finds every key the plugin reads.
    active[Index(NetFeature::EnemyTargets)].store(GetPrivateProfileIntW(L"Netcode", L"EnemyTargets", 1, iniPath) != 0);
}

bool NetFeatureActive(NetFeature feature) {
    const std::size_t index = Index(feature);
    return index < kFeatureCount && active[index].load();
}

}  // namespace multislot
