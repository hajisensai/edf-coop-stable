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
//   3: hit events carry the shot's sub-hits as ragdoll body places (version 3), so a weak point's filter takes a
//      forwarded hit.
//   4: extension ABI v2 over direct-link protocol 8: extension state messages (sendState) and an extension
//      ordered domain of its own on every link (ExtAck), so plugin messages no longer share game sequencing.
constexpr std::int64_t kNetProtocol = 4;

}  // namespace multislot
