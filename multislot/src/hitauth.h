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
constexpr std::uint16_t kGdiFromNetwork = 0x40;   // flag: dealt from a network message (the game's kill message)

// Who owns a network object, by its NetworkObject+8 word (782880): bit 0 another machine, bit 1 this one. An object
// that never got a network identity (0) is "ours" to every machine; the plugins' own objects are such.
enum class NetOwner : std::uint8_t { Unregistered, Local, Remote };
NetOwner OwnerOf(std::uint32_t networkFlags);

struct HitInput {
    bool active = false;      // the room runs hit authority (netfeature.h)
    bool replaying = false;   // this is a damage event being dealt by its target's owner
    bool fromNetwork = false;  // GDI+0x60 & 0x40
    float damage = 0.0f;
    NetOwner attacker = NetOwner::Unregistered;
    NetOwner target = NetOwner::Unregistered;
};
enum class HitVerdict : std::uint8_t {
    Vanilla,  // not ours to decide: the game's own rule
    Deal,     // dealt here (and the game deals it too)
    Drop,     // someone else's shot: its machine decides
    Forward,  // our shot at someone else's object: an event to its owner, dropped here
};
HitVerdict DecideHit(const HitInput& input);

// The damage event, as it travels (little-endian, fixed layout behind a magic and a version).
struct DamageEvent {
    std::uint32_t seq = 0;          // the sender's, increasing
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
};
constexpr std::uint8_t kDamageEventMagic0 = 'H', kDamageEventMagic1 = 'A', kDamageEventVersion = 1;
constexpr std::size_t kDamageEventBytes = 4 + 4 + 4 + 4 + 4 + 4 + 12 + 12 + 4 * 4 + 2 + 4 + 4 + 12;
// Returns the bytes written, 0 when `capacity` is too small.
std::size_t WriteDamageEvent(const DamageEvent& event, std::uint8_t* out, std::size_t capacity);
// False for anything that is not a whole event of this version.
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
};
const char* HitRejectName(HitReject reject);

// The owner's view of an event's target and attacker.
struct HitView {
    bool targetLocal = false;
    bool targetAlive = false;
    float targetPos[3]{};
    bool attackerKnown = false;
    float attackerPos[3]{};
};

// Checks one event: its numbers and geometry, then duplicates (each attacker's sequence, a window of 64 behind the
// newest), then the attacker's rate. Only an event that passes is remembered and counted against the rate.
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
    void Forget(std::uint64_t nowMs);
    HitLimits limits_;
    std::unordered_map<std::int32_t, Attacker> attackers_;
    std::uint64_t lastSweepMs_ = 0;
};
// The geometry and numbers part of Admit, on its own.
HitReject CheckEvent(const DamageEvent& event, const HitView& view, const HitLimits& limits);

}  // namespace multislot
