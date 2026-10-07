#pragma once
#include <cstdint>
#include <vector>

#include "code.h"
#include "midhook.h"
#include "patches.h"

namespace multislot {

// The game side of hit authority (hitauth.h, docs/net-re/damage.md). Two hooks, both at the first instruction of a
// function (`mov [rsp+0x10], rbx`, displaced and run after the handler):
//
//   54AA50  GameObjectBase slot 10, the message pre-filter every class derived from GameObjectBase shares (rcx the
//           object, rdx u32* message, r8 void** payload). For a damage message it decides (DecideHit): a hit that
//           is not ours to decide, or that goes to its owner as an event, is rewritten to 0x4000004 - the value the
//           game itself gives damage it drops (774260) - so the class's handler and slot 11 leave it alone.
//   54D770  GameObjectBase NetworkObject slot 17, the receive of the messages one machine sends an object's copies
//           (NetworkObject +0x80 = 773DA0, broadcast as event 7; HumanBase 577C30 and VehicleBase 6325B0 hand it
//           every type they do not take themselves). A damage event is type kHitEventTag, which no handler of the
//           game takes (they return): the hook reads it and rewinds, and the game then reads the tag and returns.
constexpr std::uint32_t kHitPreFilter = 0x54AA50;
constexpr std::uint32_t kHitObjectReceive = 0x54D770;
constexpr std::int8_t kHitEventTag = 13;  // a small int (12B5790); GameObjectBase takes 0-3, VehicleBase 4-5
constexpr std::uint32_t kDamageMessage = 0x10000000;
constexpr std::uint32_t kDroppedMessage = 0x4000004;

std::vector<MidSite> HitAuthorityHooks();

// Verifies both sites against `base`, emits their thunks into a page of its own and writes the jumps; all or
// nothing. False (and nothing changed, logged) when a site is not the expected code.
bool InstallHitAuthority(unsigned char* base);

struct DamageEvent;
// A damage event as the copies' message: the tag, then the event's bytes as one block (12B5200), into a game
// stream (0x5F8 bytes, built by 79A460).
bool WriteHitEventMessage(void* stream, const DamageEvent& event);
// Whether the stream's next value is a damage event (read into `out`); its read position is put back either way,
// as the game's own receives do before handing a message on (577C6E).
enum class HitMessage { Other, Event, Malformed };
HitMessage PeekHitEventMessage(void* stream, DamageEvent& out);

// What the hooks did since load (the 30-second summary line says the same).
struct HitCounters {
    std::uint64_t forwarded = 0, forwardFailed = 0, dropped = 0, received = 0, dealt = 0, malformed = 0;
    std::uint64_t ownerRule = 0;  // events for a target this machine still deals remote hits on itself (OwnerTakesEvent)
    std::uint64_t refused = 0;  // events the owner refused (not counting the copies that are not the owner)
    std::uint64_t notOwner = 0;
};
HitCounters HitAuthorityCounters();

// The handlers, for the tests.
void HitPreFilterHandler(CpuContext* context);
void HitObjectReceiveHandler(CpuContext* context);
void InitHitAuthority(const unsigned char* base);
// This process's sender id in its events (random, non-zero; set by InitHitAuthority).
std::uint64_t HitSenderId();
// Tests only: the rule as given, whatever the room runs (no sampler runs in the tests), until ClearHitRuleForTest.
void SetHitRuleForTest(bool forwarding, bool dropping);
void ClearHitRuleForTest();

}  // namespace multislot
