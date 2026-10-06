#pragma once
#include <cstdint>

namespace multislot {

// Per-class switches of the netcode rewrite (docs/netcode-rewrite-plan.md section 2.2): each kind of object can be
// put back on the game's own behaviour on its own. W1 (feat/net-transport) owns the real version of this file,
// which also carries the room's protocol version; this is the minimal stand-in the world part (networld.h) builds
// against until the branches are integrated. Only the values used on this branch are listed.
enum class NetFeature : std::uint32_t {
    // Remote copies of enemies keep the target their owner chose instead of choosing their own (networld.h).
    // Local behaviour only, nothing on the wire changes: see docs/net-re/world.md section 7.
    EnemyTargets = 0,
    Count
};

// Reads [Netcode] from the INI (each switch defaults to on). Before it is called every feature is off.
void InitNetFeatures(const wchar_t* iniPath);
bool NetFeatureActive(NetFeature feature);
// The INI key of a feature in [Netcode], for log lines.
const wchar_t* NetFeatureKey(NetFeature feature);

}  // namespace multislot
