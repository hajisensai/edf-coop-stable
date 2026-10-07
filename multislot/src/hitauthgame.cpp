#define _CRT_RAND_S  // rand_s: the sender id (InitHitAuthority)
#include "hitauthgame.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>

#include <cstdlib>
#include <cstring>

#include "hitauth.h"
#include "log.h"
#include "netfeature.h"

namespace multislot {
namespace {

// EDF.dll (RVAs).
constexpr std::uint32_t kLockNetworkObject = 0x22FCA0;  // shared_ptr<NetworkObject>(out, const weak_ptr<SceneObject>*)
constexpr std::uint32_t kReferenceId = 0x785050;        // int*(int* out, weak_ptr<SceneObject> by value): consumes its copy
constexpr std::uint32_t kResolveReference = 0x784C60;   // weak_ptr<SceneObject>*(out, const int* id)
constexpr std::uint32_t kStreamConstruct = 0x79A460;    // the game's message stream (0x5F8 bytes), as 54CD28 builds one
constexpr std::uint32_t kStreamDestroy = 0x760180;
constexpr std::uint32_t kWriteSmall = 0x12B5790;        // a small int (-14..15) in one byte
constexpr std::uint32_t kWriteBlock = 0x12B5200;        // a byte block (0xA0 | len >> 8, len, bytes)
constexpr std::uint32_t kReadValue = 0x12B4660;         // the reader of anything the int writers wrote
constexpr std::uint32_t kReadBlock = 0x12B4900;         // a block into another stream
constexpr std::uint32_t kStreamMark = 0x12B48A0;        // the read position (577C4D, 6325D4)
constexpr std::uint32_t kStreamRewind = 0x12B4FD0;      // back to it (577C6E, 63260D)
constexpr std::uint32_t kPartsReserve = 0x124190;       // GDI+0x70: the constructor's reserve(4) (22FE27)
constexpr std::uint32_t kPartsRelease = 0x11E9D0;       // and its release
constexpr std::uint32_t kVehicleAccept = 0x6347C0;      // VehicleBase slot 34: marks the vehicle classes
constexpr std::uint32_t kVehicleRunner = 0x630F90;      // int(veh, hostFallback, preferSeat0): 1 this machine, 2 another
constexpr std::uint32_t kIsHost = 0x784210;             // bool(null): this machine owns the room (true offline)

// Object fields (GameObjectBase).
constexpr std::size_t kPosition = 0x90;      // float[4] (54A6F2)
constexpr std::size_t kNetworkObject = 0x120;
constexpr std::size_t kNetworkFlags = 0x128;  // NetworkObject+8
// Dead: 547C30 sets it when the HP reaches 0 (548419..548429, +0x2E9 = "just died") and leaves the HP alone while
// it is set (548156); the per-frame update sends no target message for it (54C183, networld.h's kObjectNoTargetSync).
constexpr std::size_t kDead = 0x2E8;
constexpr std::size_t kHealth = 0x2F8;
constexpr std::size_t kHealthDelta = 0x5B4;  // what 54CBE0 broadcasts (type 2), summed by slot 11 (54A9F6)
constexpr std::size_t kSeats = 0x608;        // VehicleBase seat array (630FEF), 0x340 bytes a seat (631016)
constexpr std::size_t kSeatCount = 0x618;    // VehicleBase seats (630FD9); 630F90 reads seat 0 when there are any
constexpr std::size_t kSeatRider = 0x260, kSeatRiderControl = 0x268;  // weak_ptr to the rider (631020, 631043)
constexpr std::size_t kAcceptSlot = 34 * 8;  // bool(obj, attackerRemote, fromNetwork): take this damage?
// Virtual slots (byte offsets): the object's message trio (543ACE..543AEE), and NetworkObject's send to the copies.
constexpr std::size_t kHandleSlot = 0x48, kPreSlot = 0x50, kPostSlot = 0x58, kSendToCopies = 0x80;
constexpr std::size_t kStreamBytes = 0x5F8, kStreamData = 0x10, kStreamSize = 0x5F0;

struct GameRef {  // shared_ptr / weak_ptr: object, control block (uses +8, weaks +0xC)
    void* object = nullptr;
    void* control = nullptr;
};

const unsigned char* game = nullptr;
template <typename F>
F Fn(std::uint32_t rva) {
    return reinterpret_cast<F>(const_cast<unsigned char*>(game) + rva);
}

template <typename T>
T Field(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}
template <typename T>
void SetField(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

long* Count(void* control, std::size_t offset) {
    return reinterpret_cast<long*>(static_cast<std::uint8_t*>(control) + offset);
}
using ControlVirtual = void(__fastcall*)(void* control);
void ControlCall(void* control, std::size_t slot) {
    const auto table = *static_cast<ControlVirtual* const*>(control);
    table[slot](control);
}
// As the game lets go of its references (the `lock xadd` pairs after every lock).
void ReleaseShared(GameRef& ref) {
    if (ref.control) {
        if (_InterlockedExchangeAdd(Count(ref.control, 8), -1) == 1) {
            ControlCall(ref.control, 0);
            if (_InterlockedExchangeAdd(Count(ref.control, 0xC), -1) == 1) ControlCall(ref.control, 1);
        }
    }
    ref = {};
}
void ReleaseWeak(GameRef& ref) {
    if (ref.control && _InterlockedExchangeAdd(Count(ref.control, 0xC), -1) == 1) ControlCall(ref.control, 1);
    ref = {};
}
bool Alive(const GameRef& ref) { return ref.object && ref.control && *Count(ref.control, 8) > 0; }

// The game's message stream, on this stack.
class GameStream {
public:
    GameStream() { Fn<void*(__fastcall*)(void*, int)>(kStreamConstruct)(bytes_, 0x40); }
    ~GameStream() { Fn<void(__fastcall*)(void*)>(kStreamDestroy)(bytes_); }
    GameStream(const GameStream&) = delete;
    GameStream& operator=(const GameStream&) = delete;
    void* get() { return bytes_; }
    const std::uint8_t* data() const { return bytes_ + kStreamData; }
    std::size_t size() const { return Field<std::size_t>(bytes_, kStreamSize); }

private:
    alignas(16) std::uint8_t bytes_[0x600]{};
};
static_assert(sizeof(GameStream) >= kStreamBytes, "the game's stream fits");

std::int32_t ReferenceId(const void* weak) {
    // MSVC passes this by-value weak_ptr indirectly. 78511A..785130 destroys that argument (control +0xC),
    // even when no reference manager exists. Give it an owned copy, never the GDI's borrowed reference.
    GameRef owned;
    std::memcpy(&owned, weak, sizeof(owned));
    if (owned.control) _InterlockedIncrement(Count(owned.control, 0xC));
    std::int32_t id = -1;
    Fn<std::int32_t*(__fastcall*)(std::int32_t*, GameRef*)>(kReferenceId)(&id, &owned);
    return id;
}

// The caller checked the seat count. An empty/expired seat or an unregistered Dummy belongs to the host for
// damage authority, even if 630F90 would keep using this client's last driver. Read on the game thread, using
// the same weak_ptr liveness fields as 630F90 (uses at control +8).
bool NoRegisteredDriver(const std::uint8_t* vehicle) {
    const auto seat = Field<const std::uint8_t*>(vehicle, kSeats);
    if (!seat) return true;
    const auto control = Field<const std::uint8_t*>(seat, kSeatRiderControl);
    const auto rider = Field<const std::uint8_t*>(seat, kSeatRider);
    if (!control || !rider || Field<std::uint32_t>(control, 8) == 0) return true;
    return OwnerOf(Field<std::uint32_t>(rider, kNetworkFlags)) == NetOwner::Unregistered;
}

NetOwner AttackerOwner(const std::uint8_t* gdi) {
    // The override the game honours first (547E6D), then the attacker's own NetworkObject.
    if (gdi[kGdiOverride]) return gdi[kGdiOverrideRemote] ? NetOwner::Remote : NetOwner::Local;
    GameRef net;
    Fn<GameRef*(__fastcall*)(GameRef*, const void*)>(kLockNetworkObject)(&net, gdi + kGdiAttacker);
    NetOwner owner = net.object ? OwnerOf(Field<std::uint32_t>(net.object, 8)) : NetOwner::Unregistered;
    if (owner != NetOwner::Unregistered) {
        // A vehicle's registration owner is whoever created it (the host, for a delivered one), not whoever fires
        // from it. Its shots follow a registered current driver through 630F90; without one, only the host decides.
        // A private host Dummy and an empty client seat must not split authority with the client's last driver
        // (VehicleShooter; docs/net-re/damage.md section 9). The vehicle classes are
        // the 27 that share slot 34 6347C0; their NetworkObject sits at +0x120 like every GameObjectBase's.
        const auto* vehicle = static_cast<const std::uint8_t*>(net.object) - kNetworkObject;
        const auto table = *reinterpret_cast<const std::uint8_t* const*>(vehicle);
        const auto slot34 = reinterpret_cast<const unsigned char*>(Field<const void*>(table, kAcceptSlot));
        if (slot34 == game + kVehicleAccept && Field<std::uint64_t>(vehicle, kSeatCount) > 0) {
            const bool noDriver = NoRegisteredDriver(vehicle);
            const bool host = noDriver && Fn<bool(__fastcall*)(const void*)>(kIsHost)(nullptr);
            const int runner = noDriver ? 0 : Fn<int(__fastcall*)(const void*, bool, bool)>(kVehicleRunner)(vehicle, true, true);
            owner = VehicleShooter(noDriver, host, runner);
        }
    }
    ReleaseShared(net);
    return owner;
}

// Counters for the summary line, and the gate. The hooks run on the game's thread; the lock is for the log line.
SRWLOCK lock = SRWLOCK_INIT;
HitGate gate;
std::uint32_t nextSeq = 0;
std::uint64_t senderId = 0;  // this process, random (InitHitAuthority)
// The rule over time (HitRuleClock), fed by SampleRule; the tests set it outright.
constexpr DWORD kRuleSampleMs = 250;
HitRuleClock rule;
bool ruleForced = false, forcedForwarding = false, forcedDropping = false;
struct Stats {
    std::uint64_t forwarded = 0, forwardFailed = 0, dropped = 0, dealt = 0, received = 0, malformed = 0, ownerRule = 0;
    std::uint64_t rejected[9]{};
    std::uint64_t lastLogMs = 0;
} stats;
thread_local const std::uint8_t* replaying = nullptr;

void Summarize(std::uint64_t now) {
    if (now - stats.lastLogMs < 30000) return;
    if (!stats.forwarded && !stats.dropped && !stats.received && !stats.forwardFailed) return;
    stats.lastLogMs = now;
    char reasons[256]{};
    int used = 0;
    for (int i = 1; i < 9 && used >= 0; ++i)
        if (stats.rejected[i])
            used += _snprintf_s(reasons + used, sizeof(reasons) - used, _TRUNCATE, "%s%s %llu", used ? ", " : "",
                                HitRejectName(static_cast<HitReject>(i)), stats.rejected[i]);
    Log("NetHit: sent %llu hits to their owners (%llu not sent), dropped %llu copies of others' hits; received %llu, "
        "dealt %llu (%llu unreadable, %llu left to this machine's own copy of the hit)%s%s",
        stats.forwarded, stats.forwardFailed, stats.dropped, stats.received, stats.dealt, stats.malformed,
        stats.ownerRule, used > 0 ? "; refused: " : "", reasons);
}

struct RuleNow {
    bool forwarding = false, dropping = false;
};
RuleNow ReadRule(std::uint64_t now) {
    AcquireSRWLockShared(&lock);
    const RuleNow r = ruleForced ? RuleNow{forcedForwarding, forcedDropping}
                                 : RuleNow{rule.Forwarding(now), rule.Dropping(now)};
    ReleaseSRWLockShared(&lock);
    return r;
}

// The room's gate, four times a second: the hooks never ask it themselves (it builds a string per question).
DWORD WINAPI SampleRule(void*) {
    RuleNow said;
    for (;;) {
        const bool on = NetFeatureActive(NetFeature::HitAuthority);
        const std::uint64_t now = GetTickCount64();
        AcquireSRWLockExclusive(&lock);
        rule.Observe(on, now);
        const RuleNow r{rule.Forwarding(now), rule.Dropping(now)};
        ReleaseSRWLockExclusive(&lock);
        if (r.forwarding != said.forwarding || r.dropping != said.dropping) {
            Log("NetHit: %s our hits on others' objects to their owners; %s others' hits on ours to their machines",
                r.forwarding ? "sending" : "not sending", r.dropping ? "leaving" : "dealing (as the game does)");
            said = r;
        }
        Sleep(kRuleSampleMs);
    }
}

// The game's own answer to "would you take another machine's hit, not from the network" (slot 34): yes for a
// player or vehicle of this machine (5A3600, 6347C0), no for everything else (54F8D0).
bool TakesRemoteHits(const std::uint8_t* target) {
    const auto table = *reinterpret_cast<const std::uint8_t* const*>(target);
    return reinterpret_cast<bool(__fastcall*)(const void*, bool, bool)>(Field<void*>(table, kAcceptSlot))(target, true,
                                                                                                        false);
}

// Our shot at another machine's object: an event to the object's copies, which only its owner deals.
bool Forward(std::uint8_t* target, const std::uint8_t* gdi) {
    DamageEvent event;
    GdiToEvent(gdi, event);
    // The attacker's reference id, as the game's kill message names it (76B583); the target needs none, the event
    // travels through its own NetworkObject (and asking would give every object hit an entry in the id table).
    // Only for a live attacker: an empty reference would be given an id of its own.
    const GameRef attacker{Field<void*>(gdi, kGdiAttacker), Field<void*>(gdi, kGdiAttacker + 8)};
    if (Alive(attacker)) {
        event.attackerRef = ReferenceId(gdi + kGdiAttacker);
        for (int i = 0; i < 3; ++i) event.attackerPos[i] = Field<float>(attacker.object, kPosition + 4 * i);
    }
    AcquireSRWLockExclusive(&lock);
    event.seq = ++nextSeq;
    ReleaseSRWLockExclusive(&lock);
    event.sender = senderId;
    GameStream stream;
    if (!WriteHitEventMessage(stream.get(), event)) return false;
    void* net = target + kNetworkObject;
    const auto send = *reinterpret_cast<bool(__fastcall* const*)(void*, void*)>(*static_cast<std::uint8_t**>(net) +
                                                                                   kSendToCopies);
    return send(net, stream.get());
}

// The owner deals an event through the target's own message trio, as the bullet code does (543920), with the
// attacker counted as ours (EventToGdi's override) so that every class's slot 34 takes it and slot 11 broadcasts
// the result and queues the kill message, as for a hit this machine decided itself.
void Deal(std::uint8_t* target, const DamageEvent& event, GameRef& attacker) {
    alignas(16) std::uint8_t gdi[kGdiBytes]{};
    SetField<float>(gdi, kGdiExtra68, 1.0f);
    Fn<bool(__fastcall*)(void*, std::size_t)>(kPartsReserve)(gdi + kGdiParts, 4);
    EventToGdi(event, gdi);
    const bool known = Alive(attacker);
    if (known) {
        SetField<void*>(gdi, kGdiAttacker, attacker.object);
        SetField<void*>(gdi, kGdiAttacker + 8, attacker.control);
    } else {
        // Without an attacker a guest's slot 10 drops damage (774600: only the host deals it) unless it came over
        // the network; so it is marked so, and the HP change slot 11 then skips is added to the broadcast here.
        SetField<std::uint16_t>(gdi, kGdiFlags, Field<std::uint16_t>(gdi, kGdiFlags) | kGdiFromNetwork);
    }
    const float before = Field<float>(target, kHealth);
    std::uint32_t message = kDamageMessage;
    void* payload = gdi;
    const auto table = *reinterpret_cast<std::uint8_t* const*>(target);
    replaying = gdi;
    reinterpret_cast<void(__fastcall*)(void*, std::uint32_t*, void**)>(Field<void*>(table, kPreSlot))(target, &message,
                                                                                                     &payload);
    reinterpret_cast<void(__fastcall*)(void*, std::uint32_t, void*)>(Field<void*>(table, kHandleSlot))(target, message,
                                                                                                      payload);
    reinterpret_cast<void(__fastcall*)(void*, std::uint32_t, void*)>(Field<void*>(table, kPostSlot))(target, message,
                                                                                                    payload);
    replaying = nullptr;
    if (!known) {
        const float after = Field<float>(target, kHealth);
        SetField<float>(target, kHealthDelta, Field<float>(target, kHealthDelta) + (after - before));
    }
    if (known) {
        GameRef held{Field<void*>(gdi, kGdiAttacker), Field<void*>(gdi, kGdiAttacker + 8)};
        ReleaseWeak(held);
        attacker = {};
    }
    Fn<void(__fastcall*)(void*)>(kPartsRelease)(gdi + kGdiParts);
}

void Received(std::uint8_t* target, const DamageEvent& event) {
    const std::uint64_t now = GetTickCount64();
    HitView view;
    view.targetLocal = OwnerOf(Field<std::uint32_t>(target, kNetworkFlags)) == NetOwner::Local;
    // The sender dropped its own copy of this hit; whatever this machine's gate says, the event is the hit, unless
    // this machine still deals that target's remote hits itself (OwnerTakesEvent, damage.md section 6.6).
    if (view.targetLocal && !OwnerTakesEvent(ReadRule(now).dropping, TakesRemoteHits(target))) {
        AcquireSRWLockExclusive(&lock);
        ++stats.ownerRule;
        ReleaseSRWLockExclusive(&lock);
        return;
    }
    view.targetAlive = Field<std::uint8_t>(target, kDead) == 0;
    for (int i = 0; i < 3; ++i) view.targetPos[i] = Field<float>(target, kPosition + 4 * i);
    GameRef attacker;
    if (view.targetLocal && event.attackerRef != -1)
        Fn<GameRef*(__fastcall*)(GameRef*, const std::int32_t*)>(kResolveReference)(&attacker, &event.attackerRef);
    view.attackerKnown = Alive(attacker);
    if (view.attackerKnown)
        for (int i = 0; i < 3; ++i) view.attackerPos[i] = Field<float>(attacker.object, kPosition + 4 * i);
    AcquireSRWLockExclusive(&lock);
    ++stats.received;
    const HitReject reject = gate.Admit(event, view, now);
    // Copies that are not the owner's see every event: not counted as refusals.
    if (reject == HitReject::NotOwner) {
        ++stats.rejected[static_cast<int>(reject)];
    } else if (reject != HitReject::None) {
        if (stats.rejected[static_cast<int>(reject)]++ < 3)
            Log("NetHit: refused a hit on %p from attacker %d (seq %u, %.0f damage): %s", static_cast<void*>(target),
                event.attackerRef, event.seq, event.damage, HitRejectName(reject));
    }
    if (reject == HitReject::None) ++stats.dealt;
    Summarize(now);
    ReleaseSRWLockExclusive(&lock);
    if (reject == HitReject::None) Deal(target, event, attacker);
    ReleaseWeak(attacker);
}

}  // namespace

std::vector<MidSite> HitAuthorityHooks() {
    // mov [rsp+0x10], rbx: the first instruction of both, run after the handler.
    return {
        {"GameObjectBase message pre-filter (hit authority)", kHitPreFilter, {0x48, 0x89, 0x5C, 0x24, 0x10}, 0, 5},
        {"GameObjectBase copies' message receive (damage events)", kHitObjectReceive, {0x48, 0x89, 0x5C, 0x24, 0x10}, 0, 5},
    };
}

void InitHitAuthority(const unsigned char* base) {
    game = base;
    unsigned int high = 0, low = 0;
    if (rand_s(&high) || rand_s(&low)) high = static_cast<unsigned int>(GetTickCount64()), low = GetCurrentProcessId();
    senderId = (static_cast<std::uint64_t>(high) << 32 | low) | 1;  // never 0
}

std::uint64_t HitSenderId() { return senderId; }

void SetHitRuleForTest(bool forwarding, bool dropping) {
    AcquireSRWLockExclusive(&lock);
    ruleForced = true;
    forcedForwarding = forwarding;
    forcedDropping = dropping;
    ReleaseSRWLockExclusive(&lock);
}

void ClearHitRuleForTest() {
    AcquireSRWLockExclusive(&lock);
    ruleForced = false;
    ReleaseSRWLockExclusive(&lock);
}

HitCounters HitAuthorityCounters() {
    AcquireSRWLockShared(&lock);
    HitCounters c;
    c.forwarded = stats.forwarded;
    c.forwardFailed = stats.forwardFailed;
    c.dropped = stats.dropped;
    c.received = stats.received;
    c.dealt = stats.dealt;
    c.malformed = stats.malformed;
    c.ownerRule = stats.ownerRule;
    for (int i = 1; i < 9; ++i)
        if (i != static_cast<int>(HitReject::NotOwner)) c.refused += stats.rejected[i];
    c.notOwner = stats.rejected[static_cast<int>(HitReject::NotOwner)];
    ReleaseSRWLockShared(&lock);
    return c;
}

void HitPreFilterHandler(CpuContext* context) {
    // Every message every object gets passes here: damage is told apart before anything else is asked.
    auto* message = reinterpret_cast<std::uint32_t*>(context->rdx);
    auto** payload = reinterpret_cast<std::uint8_t**>(context->r8);
    if (!message || !payload || *message != kDamageMessage || !*payload) return;
    const RuleNow now = ReadRule(GetTickCount64());
    if (!now.forwarding && !now.dropping) return;
    auto* target = reinterpret_cast<std::uint8_t*>(context->rcx);
    const std::uint8_t* gdi = *payload;
    HitInput input;
    input.forwarding = now.forwarding;
    input.dropping = now.dropping;
    input.replaying = gdi == replaying;
    input.fromNetwork = (Field<std::uint16_t>(gdi, kGdiFlags) & kGdiFromNetwork) != 0;
    input.damage = Field<float>(gdi, kGdiDamage);
    input.target = OwnerOf(Field<std::uint32_t>(target, kNetworkFlags));
    // Cheap answers first: the attacker is only looked up when it decides something.
    if (input.replaying || input.fromNetwork || !(input.damage > 0.0f) || input.target == NetOwner::Unregistered)
        return;
    input.attacker = AttackerOwner(gdi);
    const HitVerdict verdict = DecideHit(input);
    if (verdict != HitVerdict::Drop && verdict != HitVerdict::Forward) return;
    // A hit that could not be sent stays the game's: dealt here as it would be (or not, by its own rule).
    const bool sent = verdict == HitVerdict::Forward && Forward(target, gdi);
    if (verdict == HitVerdict::Drop || sent) *message = kDroppedMessage;
    AcquireSRWLockExclusive(&lock);
    if (verdict == HitVerdict::Drop) ++stats.dropped;
    else if (sent) ++stats.forwarded;
    else ++stats.forwardFailed;
    Summarize(GetTickCount64());
    ReleaseSRWLockExclusive(&lock);
}

bool WriteHitEventMessage(void* stream, const DamageEvent& event) {
    std::uint8_t bytes[kDamageEventBytes];
    const std::size_t size = WriteDamageEvent(event, bytes, sizeof(bytes));
    return size && Fn<bool(__fastcall*)(void*, std::int8_t)>(kWriteSmall)(stream, kHitEventTag) &&
           Fn<bool(__fastcall*)(void*, const void*, std::size_t)>(kWriteBlock)(stream, bytes, size);
}

HitMessage PeekHitEventMessage(void* stream, DamageEvent& out) {
    std::int64_t mark = 0;
    Fn<void(__fastcall*)(void*, std::int64_t*)>(kStreamMark)(stream, &mark);
    HitMessage result = HitMessage::Other;
    if (Fn<std::int64_t(__fastcall*)(void*)>(kReadValue)(stream) == kHitEventTag) {
        GameStream block;
        result = Fn<bool(__fastcall*)(void*, void*)>(kReadBlock)(stream, block.get()) &&
                         ReadDamageEvent(block.data(), block.size(), out)
                     ? HitMessage::Event
                     : HitMessage::Malformed;
    }
    // The game's own receive reads the type again; ours it does not take.
    Fn<void(__fastcall*)(void*, std::int64_t)>(kStreamRewind)(stream, mark);
    return result;
}

void HitObjectReceiveHandler(CpuContext* context) {
    void* stream = reinterpret_cast<void*>(context->rdx);
    if (!stream) return;
    DamageEvent event;
    const HitMessage message = PeekHitEventMessage(stream, event);
    if (message == HitMessage::Event) {
        Received(reinterpret_cast<std::uint8_t*>(context->rcx) - kNetworkObject, event);
    } else if (message == HitMessage::Malformed) {
        AcquireSRWLockExclusive(&lock);
        ++stats.malformed;
        ReleaseSRWLockExclusive(&lock);
    }
}

bool InstallHitAuthority(unsigned char* base) {
    static ThunkPage page;
    InitHitAuthority(base);
    const auto sites = HitAuthorityHooks();
    const MidHandler handlers[] = {&HitPreFilterHandler, &HitObjectReceiveHandler};
    for (const auto& site : sites) {
        const Patch verify{site.name, site.rva, site.original, site.original};
        if (site.rva + site.original.size() > kImageSize || !Matches(base + site.rva, verify)) {
            Log("NetHit: EDF+%X (%s) is not the expected code; hit authority is off, hits are decided as the game "
                "does",
                site.rva, site.name);
            return false;
        }
    }
    if (!page.Allocate(base)) {
        Log("NetHit: no memory for hook thunks near EDF.dll; hit authority is off");
        return false;
    }
    std::vector<Patch> writes;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        const auto& site = sites[i];
        unsigned char* at = base + site.rva;
        const auto resume = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at + site.original.size()));
        const unsigned char* thunk = EmitMidThunk(page, handlers[i], at + site.displacedOffset, site.displacedSize, resume);
        auto bytes = thunk ? JumpBytes(at, thunk, site.original.size()) : std::vector<std::uint8_t>{};
        if (bytes.empty()) {
            Log("NetHit: hook thunk for %s out of reach; hit authority is off", site.name);
            page.Release();
            return false;
        }
        writes.push_back({site.name, site.rva, site.original, std::move(bytes)});
    }
    if (!page.Seal()) {
        Log("NetHit: could not make the hook thunks executable (error %lu); hit authority is off", GetLastError());
        page.Release();
        return false;
    }
    for (std::size_t i = 0; i < writes.size(); ++i) {
        if (WriteCode(base + writes[i].rva, writes[i].replacement.data(), writes[i].replacement.size())) continue;
        Log("NetHit: could not write EDF+%X (error %lu); hit authority is off", writes[i].rva, GetLastError());
        for (std::size_t j = i; j-- > 0;) WriteCode(base + writes[j].rva, writes[j].original.data(), writes[j].original.size());
        page.Release();
        return false;
    }
    if (HANDLE sampler = CreateThread(nullptr, 0, &SampleRule, nullptr, 0, nullptr)) CloseHandle(sampler);
    else Log("NetHit: no thread to watch the room's gate (error %lu); hits stay decided as the game does", GetLastError());
    Log("NetHit: hits are decided by the shooter's machine and dealt by the target's owner (%s); hooks at EDF+%X "
        "and EDF+%X",
        NetFeatureActive(NetFeature::HitAuthority) ? "on" : "off for now", kHitPreFilter, kHitObjectReceive);
    return true;
}

}  // namespace multislot
