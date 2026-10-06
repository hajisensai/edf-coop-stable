#pragma once
// Netcode feature gates (docs/netcode-rewrite-plan.md §2.1).
//
// MINIMAL STUB owned by the W2 player sync branch. The W1 transport branch provides the real implementation (the
// host publishes a netcode version and capability bits, members that do not match fall back to the game's own
// protocol); on integration this file is replaced by W1's. Until then a feature is active when the INI switch
// [NetFeature] <name> is on (default on), and every caller still falls back per member: a player whose machine
// does not send the new data is driven by the game's own code.
#include <cstdint>

namespace multislot {

enum class NetFeature : std::uint32_t {
    PlayerSync = 0,  // W2: velocity + timestamp block on player records, extrapolated remote players (netplayer.h)
};

// Reads [NetFeature] from the INI. Missing keys default to on.
void LoadNetFeatures(const wchar_t* iniPath);
void SetNetFeature(NetFeature feature, bool on);
bool NetFeatureActive(NetFeature feature);

}  // namespace multislot
