#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace multislot {

// Hit authority (docs/net-re/damage.md): the machine that fired decides whether a shot hit. EDF.dll TimeDateStamp
// 0x678CCB46; RVAs below.
//
// Every damage the game deals is a message 0x10000000 whose payload is a GameDamageInfo (GDI, 0x90 bytes), sent to
// the target by three of its virtuals in a row: slot 10 (54AA50, the one implementation every GameObjectBase class
// shares: a pre-filter that may rewrite the message), slot 9 (the class's handler, which reaches the damage itself,
// 547C30) and slot 11 (54A970: in a session, adds the HP change to the delta the object later broadcasts, and on a
// kill queues the game's kill message). Online, 547C30 asks the target's slot 34 whether to take it:
//
//   GameObjectBase 54F8D0     take it if the attacker is ours, or the damage came over the network (GDI+0x60 & 0x40)
//   SoldierBase    5A3600     a player (+0x1ED0 set): take it if the TARGET is ours, or it came over the network
//   VehicleBase    6347C0     take it if the target is ours, or it came over the network
//
// Bullets are copied to every machine (NetworkBullet), so every machine runs every hit. Enemies, NPCs and objects
// were already decided where the shooter is; players and vehicles were decided where the victim is, against the
// shooter's bullet as it arrived late on the victim's machine: "I hit him, he did not get hit".
//
// With this feature every class is decided where the shooter is, and the damage itself is dealt where the target is:
// the shooter's machine turns a hit on someone else's object into a damage event sent to that object's owner, and
// drops it locally (the game's own drop: slot 10 rewrites the message to 0x4000004, which no handler takes). The
// owner checks the event and deals it through the same three virtuals, so the target's own reactions (knockback,
// stagger, death) run on the machine that owns it and the game's own HP broadcast carries the result to everyone.
// A copy of someone else's bullet hitting our object is dropped here: its shooter's machine sends the event.

// GDI fields (decoy-blast-re.md section 1.2, 76B3A0 for the ones the game itself sends with a kill).
constexpr std::size_t kGdiBytes = 0x90;
constexpr std::size_t kGdiOverride = 0x01;        // u8: the next byte replaces "is the attacker remote"
constexpr std::size_t kGdiOverrideRemote = 0x02;  // u8
constexpr std::size_t kGdiResult = 0x03;          // u8: what the damage did (the handlers set it)
constexpr std::size_t kGdiAttacker = 0x10;        // weak_ptr<SceneObject>: object, control block
constexpr std::size_t kGdiKind = 0x20;            // u32
constexpr std::size_t kGdiTeam = 0x24;            // i32: the attacker's team (friendly fire is judged by it)
constexpr std::size_t kGdiPoint = 0x30;           // float[4]: where it hit, w = 1
constexpr std::size_t kGdiImpulse = 0x40;         // float[4]
constexpr std::size_t kGdiDamage = 0x50;          // float: > 0 hurts, < 0 heals
constexpr std::size_t kGdiSize = 0x54;
constexpr std::size_t kGdiRadius = 0x58;          // AmmoExplosion
constexpr std::size_t kGdiExtra5C = 0x5C;
constexpr std::size_t kGdiFlags = 0x60;           // u16
constexpr std::size_t kGdiExtra64 = 0x64;
constexpr std::size_t kGdiExtra68 = 0x68;         // 1.0 unless set
constexpr std::size_t kGdiParts = 0x70;           // a container the constructor reserves 4 entries of 0x30 in
// The container's entries are the hit's sub-hits (the bullet's shape cast, 2321B0): +0x78 data, +0x80 capacity,
// +0x88 count (124190's layout); an entry is 0x30 bytes, +0x00 the world point (float[4]), +0x20 the Havok body id
// (u32) it touched. The classes that take damage on one body only (a weak point: UfoCarrier508 slot 16 4F7190,
// MovingFortress 4C6650, and plugin classes doing the same) take the entry nearest the hit point and map its body id
// to a ragdoll body (6EA430): no entries, no damage.
constexpr std::size_t kGdiHits = 0x78, kGdiHitCapacity = 0x80, kGdiHitCount = 0x88;
constexpr std::size_t kHitRecordBytes = 0x30, kHitRecordBody = 0x20;
// A ragdoll component (constructor 6E3B10; EventAnimationObject +0xAA0, UfoCarrier508 +0xB60, MovingFortress +0xB00):
// bodies at [+0x78], their count at [+0x88], 0xC0 bytes each; a body's Havok id is [[body+0x50]+0xF0], or, when
// body+0xB8 is set, [[[body+0xB0]+0x10]+0xF0] too (6EA430 compares both).
constexpr std::size_t kRagdollBodies = 0x78, kRagdollCount = 0x88, kRagdollBodyBytes = 0xC0;
constexpr std::size_t kRagdollMostBodies = 512;
// The body index the Havok body `id` has in `ragdoll` (6EA430, run on this memory), -1 none. Reads raw pointers:
// the game side calls it under an exception guard.
int RagdollIndexOf(const std::uint8_t* ragdoll, std::uint32_t id);
// The Havok id of body `index` (its first id, the one 6EA430 tries first); false when there is no such body.
bool RagdollIdOf(const std::uint8_t* ragdoll, int index, std::uint32_t* id);
constexpr std::uint16_t kGdiFromNetwork = 0x40;   // flag: dealt from a network message (the game's kill message)

// Who owns a network object, by its NetworkObject+8 word (782880): bit 0 another machine, bit 1 this one. An object
// that never got a network identity (0) is "ours" to every machine; the plugins' own objects are such.
enum class NetOwner : std::uint8_t { Unregistered, Local, Remote };
NetOwner OwnerOf(std::uint32_t networkFlags);

struct HitInput {
    bool forwarding = false;  // this machine sends its hits on others' objects as events (HitRuleClock)
    bool dropping = false;    // this machine leaves others' hits on its objects to their machines (HitRuleClock)
    bool replaying = false;   // this is a damage event being dealt by its target's owner
    bool fromNetwork = false;  // GDI+0x60 & 0x40
    float damage = 0.0f;
    NetOwner attacker = NetOwner::Unregistered;
    NetOwner target = NetOwner::Unregistered;
    // The target's own rule (its slot 34) leaves a remote attacker's hit to the attacker's machine: enemies, NPCs,
    // objects (54F8D0), not players or vehicles.
    bool shooterDecides = false;
    // The attacker's owner as the game sees it (GDI+1 override, else its NetworkObject+8), before VehicleShooter.
    NetOwner gameAttacker = NetOwner::Unregistered;
};
enum class HitVerdict : std::uint8_t {
    Vanilla,  // not ours to decide: the game's own rule
    Deal,     // dealt here (and the game deals it too)
    Drop,     // someone else's shot: its machine decides
    Forward,  // our shot at someone else's object: an event to its owner, dropped here
};
HitVerdict DecideHit(const HitInput& input);

// The rule over time on this machine (docs/net-re/damage.md section 6.6). The room's gate (netfeature.h) flips on
// every machine at its own moment: a member joins and each machine sees it on its own lobby beat, 1-2 s apart. A
// sender that forwards while the target's owner still plays the game's rule is harmless (the owner takes no event
// for a target it deals itself, OwnerTakesEvent); an owner that drops a remote hit while its sender plays the
// game's rule loses the hit. So dropping must imply that every sender forwards: the owner drops only after the gate
// has been on without a break for `settleMs`, and a sender keeps forwarding for `graceMs` after its gate went off.
// Both are longer than the spread of the machines' views of the room. Fed by Observe (a sampler, every 250 ms);
// samples more than `maxGapMs` apart break the run (the sampler stalled: nothing is known of the gap).
class HitRuleClock {
public:
    explicit HitRuleClock(std::uint64_t settleMs = 5000, std::uint64_t graceMs = 5000, std::uint64_t maxGapMs = 1000)
        : settleMs_(settleMs), graceMs_(graceMs), maxGapMs_(maxGapMs) {}
    void Observe(bool gateOn, std::uint64_t nowMs);
    bool Forwarding(std::uint64_t nowMs) const;
    bool Dropping(std::uint64_t nowMs) const;

private:
    std::uint64_t settleMs_, graceMs_, maxGapMs_;
    bool any_ = false, on_ = false, everOn_ = false;
    std::uint64_t lastMs_ = 0, onSinceMs_ = 0, lastOnMs_ = 0;
};

// Whether the target's owner deals a damage event. `ownerDecides`: the game takes another machine's hits on this
// target itself (its slot 34 says yes to a remote attacker, not from the network: players and vehicles). While this
// machine does not drop, it deals such a target's hits from the bullet copies, as the game does; the sender's event
// would be the same hit a second time. Every other target takes no remote hit in the game's rule: the event is its
// only way in, whatever this machine's own gate says.
constexpr bool OwnerTakesEvent(bool dropping, bool ownerDecides) { return dropping || !ownerDecides; }

// Whose a registered vehicle's shots are (docs/net-re/damage.md section 9; the same rule as all-forces'
// online::Authority). `noRegisteredDriver`: seat 0 is empty, expired, or holds an unregistered rider (such as
// RideAi's private DummyVehicleRider). Only the host decides in all three states; a client's last driver cannot
// keep authority while the host seats its Dummy. With a live registered rider, use 630F90: runner 1 local, else remote.
NetOwner VehicleShooter(bool noRegisteredDriver, bool host, int runner);

// A sub-hit as it travels: the Havok body id is this machine's alone (the physics world allocates it), so it goes as
// the body's place in the target - its ragdoll component's offset in the object (the same class on every machine)
// and the body's index there - which the owner turns back into its own copy's id.
struct HitBody {
    std::uint16_t component = 0;  // offset of the ragdoll component in the target object
    std::uint16_t index = 0;      // the body's index in it
    float point[3]{};             // the sub-hit's world point
};
constexpr int kMostHitBodies = 4;  // the GDI's container is reserved for 4 (22FE27)

// Where a ragdoll component sits in a game object, so where a sub-hit's `component` may point (EDF.dll 678CCB46): the
// object constructors that build one in place (`lea rcx, [obj+X]` before `call 6E3B10`: 265F39 +0xCC0,
// 29ACD3 +0xAC0, 309D67 +0xCD0, 309E2A +0x4F0, 314CBE +0xD80, 318E32 / 4F5B9C +0xB60 (UfoCarrier508), 3FF922 +0x5E0,
// 410DBA 449F86 4D59F3 4E8ECF 4ED6F2 5ABA75 5B601B 5CAE03 7C8365 +0xAA0 (EventAnimationObject and its kin), 412029
// 44ABB7 49D52B +0x650, 4983BE +0x420, 49C711 +0x500, 4A68CD +0xB10, 4B5422 +0xDA0, 4C0C45 4DBD72 51D9C8 533A0F +0xB00
// (MovingFortress among them), 4CEF3F +0xAF0, 4CFD0A +0x4E0, 4D663D +0x400, 4FE8B7 +0x6F0, 52879C +0x10F0, 5B7041
// +0xB40, 5E403B 5EA096 +0x520, 56A724 +0xD90, 629597 +0x1320) and the damage code that maps a body through one
// (`call 6EA430`), which adds +0xE00 (50499B). The ones built outside an object (3291AE: on the heap) or below +0x300
// (inside a part, 403BB4 +0x170, 5C4A07 +0x10) are no object's. The sender looks only here, the owner reads only here:
// a sub-hit naming any other place is no sender's and its event is refused (HitReject::BodyPlace), so the owner never
// reads a place the sender made up, possibly past the end of the object. A new place needs a new protocol.
constexpr std::uint16_t kRagdollPlaces[] = {0x400, 0x420, 0x4E0, 0x4F0, 0x500, 0x520, 0x5E0, 0x650, 0x6F0, 0xAA0, 0xAC0,
                                            0xAF0, 0xB00, 0xB10, 0xB40, 0xB60, 0xCC0, 0xCD0, 0xD80, 0xD90, 0xDA0, 0xE00,
                                            0x10F0, 0x1320};
bool KnownRagdollPlace(std::uint16_t offset);

// The damage event, as it travels (little-endian, fixed layout behind a magic and a version).
struct DamageEvent {
    std::uint32_t seq = 0;          // the sender's, increasing
    std::uint64_t sender = 0;       // the sending process, random at its start (HitSenderId): dedup and rate key
    std::int32_t attackerRef = -1;  // the game's object reference id (785050) of the attacker, -1 none
    std::int32_t targetRef = -1;    // reserved (-1): the event is addressed by the target's own NetworkObject
    std::uint32_t kind = 0;
    std::int32_t team = -1;
    float point[3]{};
    float impulse[3]{};
    float damage = 0.0f;
    float size = 0.0f;
    float radius = 0.0f;
    float extra5C = 0.0f;
    std::uint16_t flags = 0;  // never with kGdiFromNetwork
    float extra64 = 0.0f;
    float extra68 = 1.0f;
    float attackerPos[3]{};  // where the attacker was on the sender's machine
    std::uint8_t bodyCount = 0;  // version 3: the sub-hits the owner rebuilds (a weak point's filter needs them)
    HitBody bodies[kMostHitBodies]{};
};
// Version 2: the 94-byte event without sub-hits. Version 3 (netprotocol 3): the same 94 bytes, then a count and that
// many 16-byte sub-hits. The reader takes both (a version 2 event is one without sub-hits); the writer writes 3.
constexpr std::uint8_t kDamageEventMagic0 = 'H', kDamageEventMagic1 = 'A', kDamageEventVersion = 3;
constexpr std::uint8_t kDamageEventVersionNoBodies = 2;
constexpr std::size_t kDamageEventBaseBytes = 4 + 4 + 8 + 4 + 4 + 4 + 4 + 12 + 12 + 4 * 4 + 2 + 4 + 4 + 12;
constexpr std::size_t kHitBodyBytes = 2 + 2 + 12;
constexpr std::size_t kDamageEventBytes = kDamageEventBaseBytes + 1 + kMostHitBodies * kHitBodyBytes;  // the most
// Returns the bytes written, 0 when `capacity` is too small (or the event has more sub-hits than it can carry).
std::size_t WriteDamageEvent(const DamageEvent& event, std::uint8_t* out, std::size_t capacity);
// False for anything that is not a whole event of version 2 or 3.
bool ReadDamageEvent(const std::uint8_t* data, std::size_t size, DamageEvent& out);

// Between an event and a GDI's bytes. GdiToEvent leaves seq, the refs and attackerPos alone. EventToGdi writes only
// the fields the event carries, plus the override that makes the attacker count as ours (a damage event is dealt
// as if this machine had fired it), and never touches the attacker reference or the parts container.
void GdiToEvent(const std::uint8_t* gdi, DamageEvent& event);
void EventToGdi(const DamageEvent& event, std::uint8_t* gdi);

// What the owner checks before dealing an event.
struct HitLimits {
    float maxHitOffset = 300.0f;  // m between the hit point and the target, plus the blast radius: the target
                                  // moved while the event travelled, and the giants are big
    float maxRange = 6000.0f;     // m between the attacker (as this machine has it) and the hit point
    float maxDamage = 1.0e7f;
    double eventsPerSecond = 750.0;  // per attacker: a penetrating minigun through a swarm, and blasts
    double burst = 1500.0;
    std::uint64_t forgetMs = 30000;  // an attacker quiet this long starts over (its machine rejoined)
};
enum class HitReject : std::uint8_t {
    None, Malformed, NotOwner, TargetDead, Damage, Offset, Range, Duplicate, Rate,
    BodyPlace,  // a sub-hit names a place in the target that holds no ragdoll in any class (kRagdollPlaces)
};
constexpr int kHitRejectCount = static_cast<int>(HitReject::BodyPlace) + 1;
const char* HitRejectName(HitReject reject);

// The owner's view of an event's target and attacker.
struct HitView {
    bool targetLocal = false;
    bool targetAlive = false;
    float targetPos[3]{};
    bool attackerKnown = false;
    float attackerPos[3]{};
};

// Checks one event: its numbers and geometry, then duplicates (a window of 64 behind the newest sequence), then the
// rate. Both per (sender, attacker): an attacker's reference id is only unique on one machine and for one mission
// (a vehicle's driver changes machine, a mission restarts, every machine has its own NPC-driven vehicle), while the
// sequence is the sender's alone. Only an event that passes is remembered and counted against the rate.
class HitGate {
public:
    explicit HitGate(const HitLimits& limits = HitLimits{}) : limits_(limits) {}
    HitReject Admit(const DamageEvent& event, const HitView& view, std::uint64_t nowMs);
    std::size_t Attackers() const { return attackers_.size(); }

private:
    struct Attacker {
        std::uint32_t newest = 0;
        std::uint64_t seen = 0;  // bit n: newest - n arrived
        double tokens = 0.0;
        std::uint64_t lastMs = 0;
        bool any = false;
    };
    struct Key {
        std::uint64_t sender;
        std::int32_t attacker;
        bool operator==(const Key& o) const { return sender == o.sender && attacker == o.attacker; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return static_cast<std::size_t>(k.sender * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint32_t>(k.attacker));
        }
    };
    void Forget(std::uint64_t nowMs);
    HitLimits limits_;
    std::unordered_map<Key, Attacker, KeyHash> attackers_;
    std::uint64_t lastSweepMs_ = 0;
};
// The geometry and numbers part of Admit, on its own.
HitReject CheckEvent(const DamageEvent& event, const HitView& view, const HitLimits& limits);

}  // namespace multislot
