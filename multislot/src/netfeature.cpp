#include "netfeature.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace multislot {
namespace {

bool hitAuthority = false;

}  // namespace

void InitNetFeatures(const wchar_t* iniPath) {
    hitAuthority = GetPrivateProfileIntW(L"NetHit", L"Enabled", 1, iniPath) != 0;
}

bool NetFeatureActive(NetFeature feature) {
    switch (feature) {
        case NetFeature::HitAuthority: return hitAuthority;
    }
    return false;
}

void SetNetFeatureForTest(NetFeature feature, bool on) {
    switch (feature) {
        case NetFeature::HitAuthority: hitAuthority = on; break;
    }
}

}  // namespace multislot
