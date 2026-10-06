#pragma once
// The netcode rewrite's protocol number on its own, so that code which must not pull in the gate (the game-code
// tests' driver, GameNetTests) can name the same value the plugin publishes (netfeature.h).
#include <cstdint>

namespace multislot {

// Bumped whenever a message, a wire format or a game rule of the netcode rewrite changes in a way an older build
// would read differently. Builds of different protocols never run new netcode together.
//   1: the first gate, transport classes, mesh, fragments, compression.
//   2: hit events carry their sender (version 2), enemy dice carry their sender (version 2), fragment ids carry
//      the sender's epoch.
constexpr std::int64_t kNetProtocol = 2;

}  // namespace multislot
