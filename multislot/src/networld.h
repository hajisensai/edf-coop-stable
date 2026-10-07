#pragma once
#include <cstdint>
#include <vector>

#include "midhook.h"
#include "patches.h"

namespace multislot {

// Enemies, bullets and mission events online (netcode rewrite W4; reverse engineering in docs/net-re/world.md).
//
// Every machine simulates every enemy. The machine that registered an enemy owns it (NetworkObject flags at object
// +0x128: bit0 = another machine's, bit1 = ours; mission script spawns are the host's) and sends the copies on
// the other machines its pose, its random state and its attack target. Target choice itself was not the owner's
// alone: GameObjectBase::Update (54BE40) chooses the nearest enemy every +0x514 frames on every machine, the
// remote copies included, each from its own view of where the players are; the owner's choice arrives as
// message 1 and is overwritten again at the copy's next choice. A copy then chases another player than the
// owner's enemy does until the next pose correction pulls it back.
//
// With [Netcode] WorldAuthority=1 a remote copy whose owner sends its targets (+0x591 set, +0x2E8 clear) and whose
// current target is still alive and selectable keeps that target instead of choosing again; the owner's next
// message 1 replaces it as before. A copy without a target (just created, or its target died) still chooses one
// for itself until the owner's next message, as the game does. Nothing is sent that was not sent before, so a
// room with machines without this mod plays as it did: their copies choose for themselves.

// Offsets inside the game this module relies on (EDF.dll 678CCB46); checked by the tests.
constexpr std::uint32_t kRetargetSite = 0x54BF39;      // GameObjectBase::Update: `cmp ecx, [rdi+0x514]; jb skip`
constexpr std::uint32_t kRetargetSkip = 0x54C22C;      // where that jb goes (no target choice this frame)
constexpr std::uint32_t kObjectNetFlags = 0x128;       // NetworkObject +8: bit0 another machine's, bit1 ours
constexpr std::uint32_t kObjectHealth = 0x2F8;         // float; the locked target is dropped at <= 0 (54BFA8)
constexpr std::uint32_t kObjectNoTargetSync = 0x2E8;   // byte; non-zero: the owner sends no message 1 (54C183)
constexpr std::uint32_t kObjectState = 0x39C;          // the nearest-enemy search skips (state & ~2) != 0 (54A3BA)
constexpr std::uint32_t kObjectRetargetCounter = 0x510;
constexpr std::uint32_t kObjectRetargetPeriod = 0x514;  // frames; 60 from GameObjectBase's constructor (5464F2)
constexpr std::uint32_t kObjectTarget = 0x518;          // weak_ptr<GameObjectBase>: object, control block
constexpr std::uint32_t kObjectSendsTarget = 0x591;     // byte; the owner sends message 1 after each choice

// What the retarget decision looks at, read from an object and its target.
struct RetargetView {
    bool online = false;            // the game's InSession (7748F0)
    std::uint16_t netFlags = 0;     // object +0x128
    bool ownerSendsTarget = false;  // +0x591 set and +0x2E8 clear
    std::uint32_t period = 0;       // +0x514
    bool hasTarget = false;         // +0x518 points at a live object (its control block's use count is not 0)
    std::uint32_t targetState = 0;  // target +0x39C
    float targetHealth = 0.0f;      // target +0x2F8
};

// True when this frame's target choice is to be skipped: the object is another machine's, that machine sends
// its choice, and the target it last sent (or this copy chose before the first message) is still a valid one.
bool KeepOwnersTarget(const RetargetView& view);

// Hook sites of this module (all at RVAs listed in docs/net-re/world.md, "hook list").
std::vector<MidSite> WorldHooks();
MidHandler WorldHookHandler(std::uint32_t rva);
// The sites of WorldHooks() whose bytes at `base` are what this build expects, each wanted feature on; a site
// that differs is logged and left out (only this module stays off, the plugin goes on).
std::vector<MidSite> VerifiedWorldHooks(const unsigned char* base);

// Remote copies that kept their owner's target / chose one themselves since the start (tests, log).
std::uint64_t KeptTargets();
std::uint64_t LocalChoices();

// --- Random state of enemies (worldrng.h): changes what is sent, so only with NetFeatureActive(WorldAuthority) ---
// The owner sends from the retarget site above (its choice frames, at most every EnemyRngSyncMs); the copies take
// it at 54D79E, inside GameObjectBase NetworkObject slot 17 (54D770) after the game read the message type and
// found it is not 0 or 1: `mov [rsp+0x30], rsi` (displaced), r8d = type - 1 there, rbx the NetworkObject, rdi
// the stream. A type the game does not know goes on to `cmp r8d, 1; jne 54D86F` (return), as without the hook.
constexpr std::uint32_t kRngReceiveSite = 0x54D79E;
constexpr std::uint32_t kObjectRandom = 0x490;   // u64 LCG state (x * 0x5D588B656C078965 + 0x269EC3)
constexpr std::uint32_t kObjectRandom2 = 0x3E8;  // u64, seeded with it (545B85)
constexpr std::uint32_t kObjectTeam = 0x314;
constexpr std::uint32_t kObjectNetwork = 0x120;
constexpr std::uint32_t kDefaultRngSyncMs = 2000;

// --- Insect pose interval (docs/net-re/world.md section 2.2) ---
// InsectBase NetworkObject slot 5 (40C510) sends the pose every 40 frames (`mov eax, 0x28` at 40C558, the
// +0x2011 clear case) or 90 (`mov ecx, 0x5A` at 40C55F) when nothing faster applies. [Netcode] InsectPoseFrames
// = N (3..90) makes both at most N; 0 or 90 leaves the game's. Only what this machine sends changes.
constexpr std::uint32_t kInsectPoseFast = 0x40C558;
constexpr std::uint32_t kInsectPoseSlow = 0x40C55F;
constexpr int kDefaultInsectPoseFrames = 30;
// The two patches for `frames` (empty when the game's values stay: 0, 90 or outside 3..90).
std::vector<Patch> InsectPosePatches(int frames);

// Reads [Netcode] EnemyRngSyncMs and InsectPoseFrames and applies the insect pose patches (their bytes checked
// first; a mismatch is logged and leaves the game's). Before the hooks are installed.
void InitWorld(unsigned char* base, const wchar_t* iniPath);
std::uint32_t RngSyncIntervalMs();
void SetRngSyncIntervalMs(std::uint32_t ms);  // tests

// What the random state sync did since load.
struct RngCounters {
    std::uint64_t sent = 0, sendFailed = 0, applied = 0, stale = 0, malformed = 0, notRemote = 0, inactive = 0;
};
RngCounters RngSyncCounters();
// The copies' side, for the tests: whether the hook at kRngReceiveSite takes this message (type and feature
// checks happen before the stream is touched).
bool RngMessageType(std::uint64_t r8);

}  // namespace multislot
