#pragma once

namespace multislot {

// Netcode features that change the room's rules (docs/netcode-rewrite-plan.md section 2.1). A feature may only run when
// every member of the room runs it, or two machines disagree about who decides what.
//
// MINIMAL STUB for the hit authority work (W3, feat/net-hit-authority). The netcode version gate belongs to W1
// (feat/net-transport), whose netfeature.h replaces this file at integration: it publishes the version and the
// feature bits in the room and answers NetFeatureActive by what the whole room supports. This stub only knows this
// machine's switch, so until the gate is merged a room where some member lacks the feature would mix rules.
enum class NetFeature {
    HitAuthority,  // [NetHit] Enabled: hits are decided by the shooter's machine (hitauth.h)
};

// Reads this machine's switches from the INI. Called once at load, before anything asks.
void InitNetFeatures(const wchar_t* iniPath);
// Whether `feature` runs now. Cheap: asked on every damage message.
bool NetFeatureActive(NetFeature feature);
// Tests: sets a feature's switch directly.
void SetNetFeatureForTest(NetFeature feature, bool on);

}  // namespace multislot
