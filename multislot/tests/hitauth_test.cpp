// Hit authority (src/hitauth.h, src/hitauthgame.h).
//   HitAuthTests                the event, the decision and the owner's checks
//   HitAuthTests <EDF.dll>      against the game's code: the hook sites, the game facts the rule rests on, the event's
//                               round trip through the game's own stream code, and the game's receive passing it by
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/hitauth.h"
#include "../src/hitauthgame.h"
#include "../src/netfeature.h"
#include "../src/patches.h"

using namespace multislot;

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const std::string& what) {
    ++checks;
    if (GetEnvironmentVariableA("HITAUTH_TRACE", nullptr, 0)) { std::printf("check: %s\n", what.c_str()); std::fflush(stdout); }
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

DamageEvent SampleEvent() {
    DamageEvent e;
    e.seq = 41;
    e.sender = 0x0123456789ABCDEFull;
    e.attackerRef = 0x1234;
    e.targetRef = -1;
    e.kind = 3;
    e.team = 0;
    e.point[0] = 10.0f, e.point[1] = 2.5f, e.point[2] = -7.0f;
    e.impulse[0] = 0.0f, e.impulse[1] = 1.0f, e.impulse[2] = 30.0f;
    e.damage = 125.5f;
    e.size = 0.3f;
    e.radius = 12.0f;
    e.extra5C = 0.25f;
    e.flags = 0x13;
    e.extra64 = 0.5f;
    e.extra68 = 1.0f;
    e.attackerPos[0] = 100.0f, e.attackerPos[1] = 0.0f, e.attackerPos[2] = 0.0f;
    return e;
}

bool Same(const DamageEvent& a, const DamageEvent& b) {
    return a.seq == b.seq && a.sender == b.sender && a.attackerRef == b.attackerRef && a.targetRef == b.targetRef && a.kind == b.kind &&
           a.team == b.team && !std::memcmp(a.point, b.point, sizeof(a.point)) &&
           !std::memcmp(a.impulse, b.impulse, sizeof(a.impulse)) && a.damage == b.damage && a.size == b.size &&
           a.radius == b.radius && a.extra5C == b.extra5C && a.flags == b.flags && a.extra64 == b.extra64 &&
           a.extra68 == b.extra68 && !std::memcmp(a.attackerPos, b.attackerPos, sizeof(a.attackerPos));
}

HitView OwnerView() {
    HitView v;
    v.targetLocal = true;
    v.targetAlive = true;
    v.targetPos[0] = 12.0f, v.targetPos[1] = 0.0f, v.targetPos[2] = -7.0f;
    return v;
}

void TestOwner() {
    Check(OwnerOf(0) == NetOwner::Unregistered, "flags 0: no network identity");
    Check(OwnerOf(1) == NetOwner::Remote, "bit 0: another machine's");
    Check(OwnerOf(2) == NetOwner::Local, "bit 1: ours");
    Check(OwnerOf(3) == NetOwner::Remote, "bit 0 wins, as the game reads it");
}

// The vehicle rule, every combination (all-forces online::Authority: no registered current driver -> host).
void TestVehicleShooter() {
    struct Row { bool noDriver; bool host; int runner; NetOwner want; const char* what; };
    const Row rows[] = {
        {false, false, 1, NetOwner::Local, "player rider of this machine: here"},
        {false, true, 1, NetOwner::Local, "host runs it: here"},
        {false, false, 2, NetOwner::Remote, "another machine runs it: there"},
        {false, true, 2, NetOwner::Remote, "the host, another machine runs it: there"},
        {false, false, 0, NetOwner::Remote, "nobody (no host fallback): not here"},
        {true, true, 1, NetOwner::Local, "NPC seat 0, on the host: here"},
        {true, true, 2, NetOwner::Local, "NPC seat 0, on the host, whatever 630F90 says: here"},
        {true, false, 1, NetOwner::Remote, "NPC seat 0 on a client that seated it too (630F90 says local): not here"},
        {true, false, 2, NetOwner::Remote, "NPC seat 0 on a client: not here"},
        {true, false, 0, NetOwner::Remote, "NPC seat 0 on a client, nobody: not here"},
        {true, false, 1, NetOwner::Remote, "empty client seat with its last local driver: only the host decides"},
        {true, true, 2, NetOwner::Local, "expired driver on the host with a remote last driver: the host decides"},
    };
    for (const Row& r : rows) Check(VehicleShooter(r.noDriver, r.host, r.runner) == r.want, r.what);
    // Exactly one machine decides: the host and a client that both seated an NPC.
    int deciders = 0;
    for (bool host : {true, false}) deciders += VehicleShooter(true, host, 1) == NetOwner::Local;
    Check(deciders == 1, "an NPC-driven vehicle seated on every machine has one decider");
}

void TestDecision() {
    HitInput in;
    in.forwarding = true;
    in.dropping = true;
    in.damage = 10.0f;
    in.attacker = NetOwner::Local;
    in.target = NetOwner::Local;
    Check(DecideHit(in) == HitVerdict::Deal, "our shot at our object is dealt here");
    in.target = NetOwner::Remote;
    Check(DecideHit(in) == HitVerdict::Forward, "our shot at another machine's object goes to its owner");
    in.attacker = NetOwner::Remote;
    Check(DecideHit(in) == HitVerdict::Drop, "a copy of someone else's shot at another's object is dropped");
    in.target = NetOwner::Local;
    Check(DecideHit(in) == HitVerdict::Drop, "a copy of someone else's shot at our object is dropped (its machine sends)");
    HitInput off = in;
    off.forwarding = off.dropping = false;
    Check(DecideHit(off) == HitVerdict::Vanilla, "off: the game's rule");
    // The two halves of the rule apart (HitRuleClock).
    HitInput h = in;
    h.dropping = false;
    h.attacker = NetOwner::Remote;
    h.target = NetOwner::Local;
    Check(DecideHit(h) == HitVerdict::Vanilla, "not dropping: a remote shot at our object keeps the game's rule");
    h.attacker = NetOwner::Local;
    h.target = NetOwner::Remote;
    Check(DecideHit(h) == HitVerdict::Forward, "not dropping, forwarding: our shot still goes to its owner");
    h.forwarding = false;
    h.dropping = true;
    Check(DecideHit(h) == HitVerdict::Vanilla, "not forwarding: our shot at another's object keeps the game's rule");
    h.attacker = NetOwner::Remote;
    Check(DecideHit(h) == HitVerdict::Drop, "dropping: a remote shot is dropped");
    HitInput net = in;
    net.fromNetwork = true;
    Check(DecideHit(net) == HitVerdict::Vanilla, "the game's kill message keeps the game's rule");
    HitInput heal = in;
    heal.damage = -5.0f;
    Check(DecideHit(heal) == HitVerdict::Vanilla, "heals keep the game's rule");
    heal.damage = 0.0f;
    Check(DecideHit(heal) == HitVerdict::Vanilla, "zero damage keeps the game's rule");
    HitInput nan = in;
    nan.damage = std::nanf("");
    Check(DecideHit(nan) == HitVerdict::Vanilla, "NaN damage keeps the game's rule");
    HitInput plugin = in;
    plugin.attacker = NetOwner::Unregistered;
    Check(DecideHit(plugin) == HitVerdict::Vanilla, "an attacker without network identity: the game's rule");
    plugin = in;
    plugin.target = NetOwner::Unregistered;
    Check(DecideHit(plugin) == HitVerdict::Vanilla, "a target without network identity: the game's rule");
    HitInput replay = in;
    replay.replaying = true;
    Check(DecideHit(replay) == HitVerdict::Deal, "an event being dealt by its owner is dealt");
}

void TestRuleClock() {
    HitRuleClock c(5000, 5000, 1000);
    Check(!c.Forwarding(0) && !c.Dropping(0), "nothing observed: neither");
    std::uint64_t t = 1000;
    c.Observe(true, t);
    Check(c.Forwarding(t) && !c.Dropping(t), "gate on: forwarding at once, not dropping yet");
    for (; t < 1000 + 5000; t += 250) c.Observe(true, t);
    Check(!c.Dropping(t - 250), "on for under 5 s: not dropping");
    c.Observe(true, t);
    Check(c.Dropping(t), "on for 5 s without a break: dropping");
    t += 250;
    c.Observe(false, t);
    Check(!c.Dropping(t), "gate off: dropping stops at once");
    Check(c.Forwarding(t), "and forwarding goes on");
    for (std::uint64_t end = t + 4500; t < end;) c.Observe(false, t += 250);
    Check(c.Forwarding(t), "4.75 s after the last on sample: still forwarding");
    c.Observe(false, t += 250);
    Check(!c.Forwarding(t), "5 s after: no longer");
    // A stalled sampler breaks the run: nothing is known of the gap.
    HitRuleClock g(5000, 5000, 1000);
    t = 0;
    for (; t <= 3000; t += 250) g.Observe(true, t);
    g.Observe(true, t = 6000);
    Check(!g.Dropping(t), "a gap of 3 s between samples starts the run again");
    Check(!g.Dropping(t + 2000), "and a stale last sample drops nothing");
    for (std::uint64_t end = t + 5000; t < end;) g.Observe(true, t += 250);
    Check(g.Dropping(t), "5 s after the gap: dropping");
    // A blink of the gate starts the run again.
    g.Observe(false, t += 250);
    g.Observe(true, t += 250);
    Check(!g.Dropping(t), "off for one sample: the run starts over");
}

void TestOwnerTakesEvent() {
    Check(OwnerTakesEvent(true, true), "dropping: an event for our player or vehicle is the hit");
    Check(OwnerTakesEvent(true, false), "dropping: an event for anything else is the hit");
    Check(!OwnerTakesEvent(false, true), "not dropping: our player or vehicle took the copy of the hit already");
    Check(OwnerTakesEvent(false, false), "not dropping: an enemy takes no remote hit itself, the event is its way in");
}

// How often one shot of machine S at an object of machine R is dealt, with S's and R's rule as given:
// S's hit on its copy of the target, and R's hit from its copy of S's bullet.
int TimesDealt(bool sForwarding, bool sDropping, bool rForwarding, bool rDropping, bool ownerDecides) {
    int dealt = 0;
    HitInput s;
    s.forwarding = sForwarding;
    s.dropping = sDropping;
    s.damage = 10.0f;
    s.attacker = NetOwner::Local;
    s.target = NetOwner::Remote;
    switch (DecideHit(s)) {
        case HitVerdict::Forward: dealt += OwnerTakesEvent(rDropping, ownerDecides); break;
        case HitVerdict::Vanilla:
        case HitVerdict::Deal: dealt += ownerDecides ? 0 : 1; break;  // slot 34 on S's copy: players/vehicles no
        case HitVerdict::Drop: break;
    }
    HitInput r;
    r.forwarding = rForwarding;
    r.dropping = rDropping;
    r.damage = 10.0f;
    r.attacker = NetOwner::Remote;
    r.target = NetOwner::Local;
    switch (DecideHit(r)) {
        case HitVerdict::Vanilla:
        case HitVerdict::Deal: dealt += ownerDecides ? 1 : 0; break;  // slot 34 on R: its own player yes, else no
        default: break;
    }
    return dealt;
}

// The review's two windows, and every state the clock allows (dropping implies forwarding on the other side).
void TestSwitchWindow() {
    for (bool od : {true, false}) {
        const char* what = od ? " (a player or vehicle)" : " (an enemy)";
        // a: the shooter switched on, the owner's gate is still off: it deals the event.
        Check(TimesDealt(true, false, false, false, od) == 1, std::string("a: shooter on, owner off: once") + what);
        // b: the shooter is off, the owner on but not settled: the owner keeps the game's rule.
        Check(TimesDealt(false, false, true, false, od) == 1, std::string("b: shooter off, owner on: once") + what);
        Check(TimesDealt(true, true, true, true, od) == 1, std::string("both settled: once") + what);
        Check(TimesDealt(false, false, false, false, od) == 1, std::string("both off: once (the game)") + what);
        Check(TimesDealt(true, false, true, true, od) == 1, std::string("shooter in its grace, owner dropping: once") + what);
    }
    Check(TimesDealt(false, false, true, true, true) == 0,
          "the one state to avoid: owner dropping while the shooter plays the game's rule (the clock rules it out)");

    // Two machines, the gate seen 2 s apart both ways round: a join turns it off, the joiner's entry turns it on.
    for (int lag : {2000, -2000}) {
        HitRuleClock s, r;
        auto gate = [](std::uint64_t t, std::uint64_t shift) {
            const std::uint64_t at = t - shift;  // this machine sees the room `shift` ms late
            return (at >= 1000 && at < 20000) || at >= 21500;  // on, a join at 20 s, on again 1.5 s later
        };
        const std::uint64_t sShift = lag > 0 ? 2000 : 0, rShift = lag > 0 ? 0 : 2000;
        bool once = true, sawDrop = false;
        for (std::uint64_t t = 5000; t < 40000; t += 50) {
            if (t % 250 == 0) {
                s.Observe(gate(t, sShift), t);
                r.Observe(gate(t, rShift), t);
            }
            sawDrop = sawDrop || r.Dropping(t);
            for (bool od : {true, false})
                once = once && TimesDealt(s.Forwarding(t), s.Dropping(t), r.Forwarding(t), r.Dropping(t), od) == 1 &&
                       TimesDealt(r.Forwarding(t), r.Dropping(t), s.Forwarding(t), s.Dropping(t), od) == 1;
        }
        Check(sawDrop, "the owner does drop once settled");
        Check(once, lag > 0 ? "every shot dealt once while the shooter's view lags 2 s"
                            : "every shot dealt once while the owner's view lags 2 s");
    }
}

void TestSerialization() {
    const DamageEvent e = SampleEvent();
    std::uint8_t bytes[kDamageEventBytes + 8]{};
    Check(kDamageEventBytes == 94, "the event is 94 bytes");
    const std::size_t n = WriteDamageEvent(e, bytes, sizeof(bytes));
    Check(n == kDamageEventBytes, "the whole event is written");
    Check(WriteDamageEvent(e, bytes, kDamageEventBytes - 1) == 0, "no room: nothing written");
    DamageEvent out;
    Check(ReadDamageEvent(bytes, n, out) && Same(e, out), "the event reads back as written");
    Check(!ReadDamageEvent(bytes, n - 1, out), "a short event is refused");
    Check(!ReadDamageEvent(bytes, n + 1, out), "a long event is refused");
    std::uint8_t bad[kDamageEventBytes];
    std::memcpy(bad, bytes, n);
    bad[0] = 'X';
    Check(!ReadDamageEvent(bad, n, out), "a wrong magic is refused");
    std::memcpy(bad, bytes, n);
    bad[2] = kDamageEventVersion + 1;
    Check(!ReadDamageEvent(bad, n, out), "another version is refused");
    DamageEvent net = e;
    net.flags |= kGdiFromNetwork;
    WriteDamageEvent(net, bytes, sizeof(bytes));
    Check(ReadDamageEvent(bytes, n, out) && !(out.flags & kGdiFromNetwork) && out.flags == e.flags,
          "the from-network flag never travels");
}

void TestGdi() {
    std::uint8_t gdi[kGdiBytes]{};
    const DamageEvent e = SampleEvent();
    std::uint8_t sentinel[0x10];
    std::memset(sentinel, 0x5A, sizeof(sentinel));
    std::memcpy(gdi + kGdiAttacker, sentinel, 0x10);
    std::memset(gdi + kGdiParts, 0x77, kGdiBytes - kGdiParts);
    EventToGdi(e, gdi);
    Check(gdi[kGdiOverride] == 1 && gdi[kGdiOverrideRemote] == 0, "a dealt event counts the attacker as ours");
    Check(!std::memcmp(gdi + kGdiAttacker, sentinel, 0x10), "the attacker reference is left alone");
    bool parts = true;
    for (std::size_t i = kGdiParts; i < kGdiBytes; ++i) parts = parts && gdi[i] == 0x77;
    Check(parts, "the parts container is left alone");
    float w = 0.0f;
    std::memcpy(&w, gdi + kGdiPoint + 12, 4);
    Check(w == 1.0f, "the hit point's w is 1");
    DamageEvent back;
    back.seq = e.seq;
    back.sender = e.sender;
    back.attackerRef = e.attackerRef;
    std::memcpy(back.attackerPos, e.attackerPos, sizeof(back.attackerPos));
    GdiToEvent(gdi, back);
    Check(Same(e, back), "GDI fields round trip");
    std::uint16_t flags = 0x40 | 0x2;
    std::memcpy(gdi + kGdiFlags, &flags, 2);
    GdiToEvent(gdi, back);
    Check(back.flags == 0x2, "the from-network flag is not taken from the GDI");
}

void TestCheck() {
    const HitLimits limits;
    const DamageEvent e = SampleEvent();
    HitView v = OwnerView();
    Check(CheckEvent(e, v, limits) == HitReject::None, "a plain hit passes");
    HitView notOwner = v;
    notOwner.targetLocal = false;
    Check(CheckEvent(e, notOwner, limits) == HitReject::NotOwner, "not the owner");
    HitView dead = v;
    dead.targetAlive = false;
    Check(CheckEvent(e, dead, limits) == HitReject::TargetDead, "a dead target");
    DamageEvent d = e;
    d.damage = 0.0f;
    Check(CheckEvent(d, v, limits) == HitReject::Damage, "no damage");
    d.damage = 2.0e7f;
    Check(CheckEvent(d, v, limits) == HitReject::Damage, "too much damage");
    d = e;
    d.point[1] = std::nanf("");
    Check(CheckEvent(d, v, limits) == HitReject::Malformed, "a NaN hit point");
    d = e;
    d.point[0] = v.targetPos[0] + limits.maxHitOffset + e.radius + 1.0f;
    d.attackerPos[0] = d.point[0];
    Check(CheckEvent(d, v, limits) == HitReject::Offset, "a hit point far from the target");
    d.point[0] = v.targetPos[0] + limits.maxHitOffset + e.radius - 1.0f;
    Check(CheckEvent(d, v, limits) == HitReject::None, "within the offset plus the blast radius");
    d = e;
    d.attackerPos[0] = e.point[0] + limits.maxRange + e.radius + 1.0f;
    Check(CheckEvent(d, v, limits) == HitReject::Range, "an attacker too far away (as the sender said)");
    HitView known = v;
    known.attackerKnown = true;
    known.attackerPos[0] = e.point[0] + 50.0f;
    Check(CheckEvent(d, known, limits) == HitReject::None, "the owner's own view of the attacker wins");
    known.attackerPos[0] = e.point[0] + limits.maxRange + e.radius + 1.0f;
    Check(CheckEvent(e, known, limits) == HitReject::Range, "the owner sees the attacker too far away");
}

void TestGate() {
    HitGate gate;
    const HitView v = OwnerView();
    DamageEvent e = SampleEvent();
    std::uint64_t now = 1000;
    e.seq = 100;
    Check(gate.Admit(e, v, now) == HitReject::None, "first event");
    Check(gate.Admit(e, v, now) == HitReject::Duplicate, "the same event twice");
    e.seq = 102;
    Check(gate.Admit(e, v, now) == HitReject::None, "a later event");
    e.seq = 101;
    Check(gate.Admit(e, v, now) == HitReject::None, "an earlier event that arrives late, once");
    Check(gate.Admit(e, v, now) == HitReject::Duplicate, "and not twice");
    e.seq = 100;
    Check(gate.Admit(e, v, now) == HitReject::Duplicate, "an old one again");
    e.seq = 102 + 200;
    Check(gate.Admit(e, v, now) == HitReject::None, "a jump ahead");
    e.seq = 102 + 200 - 64;
    Check(gate.Admit(e, v, now) == HitReject::Duplicate, "behind the window: refused");
    e.seq = 102 + 200 - 63;
    Check(gate.Admit(e, v, now) == HitReject::None, "inside the window");
    // Sequence wrap.
    HitGate wrap;
    e.seq = 0xFFFFFFFFu;
    Check(wrap.Admit(e, v, now) == HitReject::None, "the last sequence number");
    e.seq = 0;
    Check(wrap.Admit(e, v, now) == HitReject::None, "wraps to 0");
    e.seq = 0xFFFFFFFFu;
    Check(wrap.Admit(e, v, now) == HitReject::Duplicate, "and the one before the wrap is remembered");
    // Senders are apart: the same reference id on another machine (another mission, another NPC vehicle).
    DamageEvent elsewhere = SampleEvent();
    elsewhere.sender = 0x42;
    elsewhere.seq = 100;
    Check(gate.Admit(elsewhere, v, now) == HitReject::None, "the same attacker id from another sender is not a duplicate");
    elsewhere.seq = 101;
    Check(gate.Admit(elsewhere, v, now) == HitReject::None, "and its sequence goes on from its own");
    // Attackers are apart.
    DamageEvent other = SampleEvent();
    other.attackerRef = 0x999;
    other.seq = 100;
    Check(gate.Admit(other, v, now) == HitReject::None, "another attacker's sequence is its own");
    // A refused event is not remembered.
    DamageEvent quiet = SampleEvent();
    quiet.attackerRef = 0x77;
    quiet.seq = 5;
    HitView notOwner = v;
    notOwner.targetLocal = false;
    Check(gate.Admit(quiet, notOwner, now) == HitReject::NotOwner, "not the owner");
    Check(gate.Admit(quiet, v, now) == HitReject::None, "the same event, now at its owner, is dealt");
    // Forgetting a quiet attacker (its machine rejoined, its numbers start over).
    e = SampleEvent();
    e.seq = 1;
    Check(gate.Admit(e, v, now + 31000) == HitReject::None, "an attacker quiet for 30 s starts over");
}

void TestRate() {
    HitLimits limits;
    limits.eventsPerSecond = 10.0;
    limits.burst = 5.0;
    HitGate gate(limits);
    const HitView v = OwnerView();
    DamageEvent e = SampleEvent();
    std::uint64_t now = 5000;
    int dealt = 0;
    for (std::uint32_t i = 0; i < 8; ++i) {
        e.seq = 10 + i;
        dealt += gate.Admit(e, v, now) == HitReject::None;
    }
    Check(dealt == 5, "a burst is cut at the bucket's size");
    e.seq = 18;
    Check(gate.Admit(e, v, now) == HitReject::Rate, "over the rate");
    e.seq = 15;
    now += 200;  // two tokens back
    Check(gate.Admit(e, v, now) == HitReject::None, "a rate-refused event is not remembered as seen");
    e.seq = 19;
    Check(gate.Admit(e, v, now) == HitReject::None, "the second token");
    e.seq = 20;
    Check(gate.Admit(e, v, now) == HitReject::Rate, "and no third");
}

// ---- against the game's code ----

const unsigned char* base = nullptr;
template <typename F>
F Fn(std::uint32_t rva) {
    return reinterpret_cast<F>(const_cast<unsigned char*>(base) + rva);
}

bool Bytes(std::uint32_t rva, const char* hex) {
    std::vector<std::uint8_t> want;
    for (const char* p = hex; p[0] && p[1]; p += 2) {
        const char pair[3] = {p[0], p[1], 0};
        want.push_back(static_cast<std::uint8_t>(std::strtoul(pair, nullptr, 16)));
    }
    return !std::memcmp(base + rva, want.data(), want.size());
}

std::uint64_t Slot(std::uint32_t vtable, int index) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + vtable + 8 * index, 8);
    return value - reinterpret_cast<std::uint64_t>(base);
}

struct Stream {
    alignas(16) std::uint8_t bytes[0x600]{};
};
constexpr std::uint32_t kStreamConstruct = 0x79A460, kStreamFrom = 0x12B4530, kWriteSmall = 0x12B5790,
                        kWriteInt = 0x12B5580, kWriteBlock = 0x12B5200, kReadValue = 0x12B4660;

std::int64_t Position(const Stream& s) {
    std::int64_t value = 0;
    std::memcpy(&value, s.bytes + 8, 8);
    return value;
}

// The bytes a written stream holds, as a stream to read (what the receiver gets after the object id).
void ToReader(const Stream& written, Stream& reader) {
    std::size_t size = 0;
    std::memcpy(&size, written.bytes + 0x5F0, 8);
    Fn<void(__fastcall*)(void*, const void*, std::size_t)>(kStreamFrom)(reader.bytes, written.bytes + 0x10, size);
}

// The image is mapped without its imports; the stream code copies with the C runtime's memcpy, through the import
// table. Only the C runtime's imports are filled in (from this process's own copies); nothing else of the game's
// imports is reachable from the code called here.
bool ResolveRuntimeImports(HMODULE module) {
    auto* image = reinterpret_cast<std::uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image + directory.VirtualAddress); d->Name; ++d) {
        const char* dll = reinterpret_cast<const char*>(image + d->Name);
        if (_strnicmp(dll, "VCRUNTIME", 9) && _strnicmp(dll, "api-ms-win-crt-", 15)) continue;
        const HMODULE runtime = LoadLibraryA(dll);
        if (!runtime) return false;
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(image + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<std::uint64_t*>(image + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image + names->u1.AddressOfData);
            const FARPROC proc = GetProcAddress(runtime, reinterpret_cast<const char*>(byName->Name));
            if (!proc) continue;
            DWORD old = 0;
            if (!VirtualProtect(slots, 8, PAGE_READWRITE, &old)) return false;
            *slots = reinterpret_cast<std::uint64_t>(proc);
            VirtualProtect(slots, 8, old, &old);
        }
    }
    return true;
}

void TestGameCode() {
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    Check(nt->FileHeader.TimeDateStamp == kImageTimeDateStamp, "EDF.dll is the supported build");

    // The hook sites.
    for (const auto& site : HitAuthorityHooks()) {
        const Patch verify{site.name, site.rva, site.original, site.original};
        Check(Matches(base + site.rva, verify), std::string("hook site holds the expected code: ") + site.name);
    }
    // The three slot 34 rules (docs/net-re/damage.md section 2), whole.
    Check(Bytes(0x54F8D0, "84d2410fb6c8b8010000000f45c1c3"), "GameObjectBase slot 34: attacker ours, or from the network");
    Check(Bytes(0x5A3600, "4883b9d01e0000007414f6812801000001b801000000410fb6c80f45c1c3"),
          "SoldierBase slot 34: a player is decided where it is");
    Check(Bytes(0x6347C0, "f6812801000001b901000000410fb6c00f44c1c3"), "VehicleBase slot 34: decided where it is");
    Check(Bytes(0x630F90, "48895c2420448844241888542410"), "630F90 (who runs a vehicle) is the expected function");
    Check(Bytes(0x630FD9, "4c39a118060000"), "630F90 counts seats at +0x618");
    Check(Bytes(0x630FEF, "498b8e08060000"), "630F90 reads the seat array at +0x608");
    Check(Bytes(0x631016, "4869dd400300004803d9488b9368020000"), "seats are 0x340 bytes, the rider's control block at +0x268");
    Check(Bytes(0x631043, "4c8bbb60020000488b9b68020000"), "the rider at seat +0x260");
    Check(Bytes(0x6310FD, "0fb7b828010000"), "630F90 reads the rider's network word at +0x128");
    Check(Bytes(0x784210, "48895c240848897424105748"), "784210 (this machine hosts) is the expected function");
    // The game's own drop of a damage message, and the handler's range that leaves it out.
    Check(Bytes(0x774614, "41c70704000004"), "774614 writes 0x4000004 over damage it drops");
    Check(Bytes(0x54A555, "81c2000000f083fa0f"), "slot 9 takes 0x10000000..0x1000000F only");
    // Vtables: every class's slot 10 is the hooked pre-filter; the copies' send and receive.
    constexpr std::uint32_t kGameObjectVtable = 0x17CD3F0, kGameObjectNetVtable = 0x17CD560;
    Check(Slot(kGameObjectVtable, 10) == kHitPreFilter, "GameObjectBase slot 10 is 54AA50");
    Check(Slot(kGameObjectVtable, 34) == 0x54F8D0, "GameObjectBase slot 34 is 54F8D0");
    Check(Slot(kGameObjectNetVtable, 16) == 0x773DA0, "NetworkObject +0x80 is the send to the copies (773DA0)");
    Check(Slot(kGameObjectNetVtable, 17) == kHitObjectReceive, "NetworkObject slot 17 is 54D770");
    Check(CallTargets(base + 0x577C79, 0x577C79, kHitObjectReceive), "HumanBase hands unknown types to 54D770");
    Check(CallTargets(base + 0x632618, 0x632618, kHitObjectReceive), "VehicleBase hands unknown types to 54D770");
    Check(CallTargets(base + 0x54A586, 0x54A586, 0x547C30), "slot 9 reaches the damage (547C30)");
    Check(CallTargets(base + 0x54AA97, 0x54AA97, 0x774260), "the pre-filter's network part (774260)");

    // The event through the game's own stream code, as the hooks write and read it.
    InitHitAuthority(base);
    const DamageEvent e = SampleEvent();
    Stream written;
    Fn<void*(__fastcall*)(void*, int)>(kStreamConstruct)(written.bytes, 0x40);
    Check(WriteHitEventMessage(written.bytes, e), "the event is written into a game stream");
    Stream reader;
    ToReader(written, reader);
    const std::int64_t start = Position(reader);
    DamageEvent out;
    Check(PeekHitEventMessage(reader.bytes, out) == HitMessage::Event && Same(e, out),
          "the event reads back through the game's readers");
    Check(Position(reader) == start, "the read position is put back");
    Check(Fn<std::int64_t(__fastcall*)(void*)>(kReadValue)(reader.bytes) == kHitEventTag,
          "the game's type reader sees the tag");

    // The game's own receive (54D770) reads our type and returns, touching nothing.
    Stream again;
    ToReader(written, again);
    alignas(16) std::uint8_t copy[0x700];
    std::memset(copy, 0xCD, sizeof(copy));
    std::uint8_t* net = copy + 0x200;  // obj+0x120 inside a fake object
    Fn<void(__fastcall*)(void*, void*)>(kHitObjectReceive)(net, again.bytes);
    bool untouched = true;
    for (std::uint8_t b : copy) untouched = untouched && b == 0xCD;
    Check(untouched, "the game's receive leaves a damage event alone");
    Check(Position(again) == 1, "it read the type byte only");

    // A message of the game's (type 2: an HP delta and a sequence) is not ours, and stays where it was.
    Stream game;
    Fn<void*(__fastcall*)(void*, int)>(kStreamConstruct)(game.bytes, 0x40);
    Fn<bool(__fastcall*)(void*, std::int8_t)>(kWriteSmall)(game.bytes, 2);
    Fn<bool(__fastcall*)(void*, int)>(kWriteInt)(game.bytes, -150);
    Fn<bool(__fastcall*)(void*, int)>(kWriteInt)(game.bytes, 7);
    Stream gameReader;
    ToReader(game, gameReader);
    Check(PeekHitEventMessage(gameReader.bytes, out) == HitMessage::Other && Position(gameReader) == 0,
          "the game's own message passes untouched");
    // Our tag with something that is not an event.
    Stream junk;
    Fn<void*(__fastcall*)(void*, int)>(kStreamConstruct)(junk.bytes, 0x40);
    const std::uint8_t garbage[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    Fn<bool(__fastcall*)(void*, std::int8_t)>(kWriteSmall)(junk.bytes, kHitEventTag);
    Fn<bool(__fastcall*)(void*, const void*, std::size_t)>(kWriteBlock)(junk.bytes, garbage, sizeof(garbage));
    Stream junkReader;
    ToReader(junk, junkReader);
    Check(PeekHitEventMessage(junkReader.bytes, out) == HitMessage::Malformed && Position(junkReader) == 0,
          "a tagged block that is not an event is malformed");

    // The receive hook on a copy that is not the owner: counted, not dealt (nothing of the game's is touched).
    SetHitRuleForTest(true, true);
    alignas(16) std::uint8_t object[0x800]{};
    const std::uint32_t remote = 1;
    std::memcpy(object + 0x128, &remote, 4);
    Stream hooked;
    ToReader(written, hooked);
    CpuContext ctx{};
    ctx.rcx = reinterpret_cast<std::uint64_t>(object + 0x120);
    ctx.rdx = reinterpret_cast<std::uint64_t>(hooked.bytes);
    const HitCounters before = HitAuthorityCounters();
    HitObjectReceiveHandler(&ctx);
    const HitCounters after = HitAuthorityCounters();
    Check(after.received == before.received + 1 && after.notOwner == before.notOwner + 1 && after.dealt == before.dealt,
          "a copy that is not the owner reads the event and leaves it");
    Check(Position(hooked) == 0, "and puts the read position back");
    ToReader(junk, hooked);
    HitObjectReceiveHandler(&ctx);
    Check(HitAuthorityCounters().malformed == after.malformed + 1, "a malformed event is counted");

    // At the owner, whatever its own gate says: an event for a target that takes no remote hit itself (54F8D0) is
    // dealt; one for our player or vehicle (6347C0) only while dropping, else that copy of the hit was dealt already.
    // The event carries an impossible damage so that it stops at the owner's checks, before the game is called.
    DamageEvent probe = SampleEvent();
    probe.attackerRef = -1;
    probe.damage = 1.0e9f;
    Stream probeWritten;
    Fn<void*(__fastcall*)(void*, int)>(kStreamConstruct)(probeWritten.bytes, 0x40);
    WriteHitEventMessage(probeWritten.bytes, probe);
    struct ReceiveCase {
        const char* what;
        std::uint32_t slot34;
        bool dropping;
        bool reachesChecks;
    };
    const ReceiveCase receives[] = {
        {"owner not dropping, an enemy: the event reaches the owner's checks", 0x54F8D0, false, true},
        {"owner not dropping, its vehicle: left to its own copy of the hit", 0x6347C0, false, false},
        {"owner dropping, its vehicle: the event reaches the owner's checks", 0x6347C0, true, true},
        {"owner dropping, an enemy: the event reaches the owner's checks", 0x54F8D0, true, true},
    };
    for (const auto& c : receives) {
        SetHitRuleForTest(false, c.dropping);
        std::uint64_t table[40]{};
        table[34] = reinterpret_cast<std::uint64_t>(base + c.slot34);
        alignas(16) std::uint8_t owned[0x800]{};
        const void* tablePtr = table;
        std::memcpy(owned, &tablePtr, 8);
        const std::uint32_t mine = 2;
        std::memcpy(owned + 0x128, &mine, 4);
        Stream in;
        ToReader(probeWritten, in);
        CpuContext rc{};
        rc.rcx = reinterpret_cast<std::uint64_t>(owned + 0x120);
        rc.rdx = reinterpret_cast<std::uint64_t>(in.bytes);
        const HitCounters b = HitAuthorityCounters();
        HitObjectReceiveHandler(&rc);
        const HitCounters a = HitAuthorityCounters();
        const bool checked = a.refused == b.refused + 1 && a.ownerRule == b.ownerRule;
        const bool held = a.ownerRule == b.ownerRule + 1 && a.refused == b.refused;
        Check(c.reachesChecks ? checked : held, c.what);
    }
    ClearHitRuleForTest();
}

// The send the forward goes through (NetworkObject +0x80), faked: its answer is the test's.
bool fakeSendResult = false;
int fakeSends = 0;
bool __fastcall FakeSend(void*, void*) {
    ++fakeSends;
    return fakeSendResult;
}

// The pre-filter's decisions on fake objects (the forward itself needs a game session: gamenet/real machine).
void TestPreFilter() {
    struct Case {
        const char* what;
        std::uint32_t targetFlags;
        bool attackerRemote;
        float damage;
        std::uint16_t flags;
        bool forwarding, dropping;
        bool sendWorks;
        std::uint32_t expect;
    };
    const Case cases[] = {
        {"someone else's shot at our player: dropped", 2, true, 50.0f, 0, true, true, true, kDroppedMessage},
        {"someone else's shot at a third machine's object: dropped", 1, true, 50.0f, 0, true, true, true,
         kDroppedMessage},
        {"not yet dropping: someone else's shot at our player is the game's", 2, true, 50.0f, 0, true, false, true,
         kDamageMessage},
        {"our shot at our object: the game deals it", 2, false, 50.0f, 0, true, true, true, kDamageMessage},
        {"our shot at another's object, sent: dropped here", 1, false, 50.0f, 0, true, false, true, kDroppedMessage},
        {"our shot at another's object, not sent: the game's", 1, false, 50.0f, 0, true, true, false, kDamageMessage},
        {"not forwarding: our shot at another's object is the game's", 1, false, 50.0f, 0, false, true, true,
         kDamageMessage},
        {"the game's kill message: untouched", 1, true, 50.0f, kGdiFromNetwork, true, true, true, kDamageMessage},
        {"a heal: untouched", 2, true, -50.0f, 0, true, true, true, kDamageMessage},
        {"a target without network identity: untouched", 0, true, 50.0f, 0, true, true, true, kDamageMessage},
        {"feature off: untouched", 2, true, 50.0f, 0, false, false, true, kDamageMessage},
    };
    std::uint64_t netTable[20]{};
    netTable[0x80 / 8] = reinterpret_cast<std::uint64_t>(&FakeSend);
    for (const auto& c : cases) {
        SetHitRuleForTest(c.forwarding, c.dropping);
        fakeSendResult = c.sendWorks;
        alignas(16) std::uint8_t object[0x800]{};
        const void* netTablePtr = netTable;
        std::memcpy(object + 0x120, &netTablePtr, 8);  // its NetworkObject's vtable: the send
        std::memcpy(object + 0x128, &c.targetFlags, 4);
        alignas(16) std::uint8_t gdi[kGdiBytes]{};
        gdi[kGdiOverride] = 1;  // the attacker as the override says: no game object needed
        gdi[kGdiOverrideRemote] = c.attackerRemote ? 1 : 0;
        std::memcpy(gdi + kGdiDamage, &c.damage, 4);
        std::memcpy(gdi + kGdiFlags, &c.flags, 2);
        std::uint32_t message = kDamageMessage;
        void* payload = gdi;
        CpuContext ctx{};
        ctx.rcx = reinterpret_cast<std::uint64_t>(object);
        ctx.rdx = reinterpret_cast<std::uint64_t>(&message);
        ctx.r8 = reinterpret_cast<std::uint64_t>(&payload);
        HitPreFilterHandler(&ctx);
        Check(message == c.expect, c.what);
    }
    // Other messages pass whatever they carry.
    alignas(16) std::uint8_t object[0x800]{};
    const std::uint32_t local = 2;
    std::memcpy(object + 0x128, &local, 4);
    std::uint32_t message = 0x10000007;
    void* payload = object;
    CpuContext ctx{};
    ctx.rcx = reinterpret_cast<std::uint64_t>(object);
    ctx.rdx = reinterpret_cast<std::uint64_t>(&message);
    ctx.r8 = reinterpret_cast<std::uint64_t>(&payload);
    SetHitRuleForTest(true, true);
    HitPreFilterHandler(&ctx);
    Check(message == 0x10000007, "a message that is not damage passes");
    Check(fakeSends == 2, "the two forwards went through the object's send");
    const HitCounters counters = HitAuthorityCounters();
    Check(counters.forwardFailed >= 1, "the send that failed is counted");
    ClearHitRuleForTest();
}

// ReferenceId takes a weak_ptr by value at the binary boundary: its real epilogue destroys the argument.
// An empty reference manager lets us execute that path without starting a game session or replacing game code.
void TestForwardReferenceOwnership() {
    constexpr std::uint32_t kWorld = 0x20B2AC0;
    Check(Bytes(0x78506F, "488b054ada9201"), "ReferenceId reads the expected world singleton");
    Check(Bytes(0x78511A, "498b4e084885c97410f00fc1790c83ff017506488b01ff5008"),
          "ReferenceId destroys its by-value weak argument through the real game epilogue");
    alignas(16) std::uint8_t world[0x100]{};
    void* singleton = world + 0x98;
    void* previous = nullptr;
    auto* address = const_cast<unsigned char*>(base) + kWorld;
    std::memcpy(&previous, address, sizeof(previous));
    DWORD old = 0;
    const bool writable = VirtualProtect(address, sizeof(singleton), PAGE_READWRITE, &old) != FALSE;
    Check(writable, "the private DLL mapping's world singleton can be set for the reference test");
    if (!writable) return;
    std::memcpy(address, &singleton, sizeof(singleton));

    struct Control {
        using Call = void(__fastcall*)(void*);
        Call* table;
        long uses = 1;
        long weaks = 2;  // the shared owner's implicit weak, plus the GDI's weak
        int deleted = 0;
    };
    Control::Call controlTable[] = {
        [](void*) {},
        [](void* p) { ++static_cast<Control*>(p)->deleted; },
    };
    Control control{controlTable};
    alignas(16) std::uint8_t attacker[0x100]{};
    void* weak[2] = {attacker, &control};
    // Prove the fixture reaches the real callee's ownership transfer, including when no id can be assigned.
    std::int32_t id = 0;
    Fn<std::int32_t*(__fastcall*)(std::int32_t*, void*)>(0x785050)(&id, weak);
    Check(id == -1 && control.weaks == 1, "the real ReferenceId consumes one weak even without a reference manager");
    control.weaks = 2;

    std::uint64_t netTable[20]{};
    netTable[0x80 / 8] = reinterpret_cast<std::uint64_t>(&FakeSend);
    alignas(16) std::uint8_t target[0x800]{};
    const void* table = netTable;
    const std::uint32_t remote = 1;
    std::memcpy(target + 0x120, &table, sizeof(table));
    std::memcpy(target + 0x128, &remote, sizeof(remote));
    alignas(16) std::uint8_t gdi[kGdiBytes]{};
    gdi[kGdiOverride] = 1;  // our live attacker, without needing its full NetworkObject
    const float damage = 50.0f;
    std::memcpy(gdi + kGdiDamage, &damage, sizeof(damage));
    std::memcpy(gdi + kGdiAttacker, weak, sizeof(weak));
    SetHitRuleForTest(true, false);
    const int sendsBefore = fakeSends;
    for (bool sent : {true, false, true}) {
        fakeSendResult = sent;
        std::uint32_t message = kDamageMessage;
        void* payload = gdi;
        CpuContext ctx{};
        ctx.rcx = reinterpret_cast<std::uint64_t>(target);
        ctx.rdx = reinterpret_cast<std::uint64_t>(&message);
        ctx.r8 = reinterpret_cast<std::uint64_t>(&payload);
        HitPreFilterHandler(&ctx);
        Check(control.uses == 1 && control.weaks == 2 && control.deleted == 0,
              "forwarding a live attack preserves the GDI's weak ownership, including failed sends");
        Check(!std::memcmp(gdi + kGdiAttacker, weak, sizeof(weak)), "forwarding leaves the GDI's reference intact");
        Check(message == (sent ? kDroppedMessage : kDamageMessage), "reference ownership preserves forwarding decisions");
    }
    Check(fakeSends == sendsBefore + 3, "the ownership regression executes all three production forwards");
    ClearHitRuleForTest();
    std::memcpy(address, &previous, sizeof(previous));
    VirtualProtect(address, sizeof(singleton), old, &old);
}

// Execute the production attacker lookup with real RTTI, weak locking, host lookup and vehicle runner code.
// The host may have seated its private Dummy while the client still has an empty seat and a local last driver.
void TestVehicleSeatAuthority() {
    alignas(16) std::uint8_t world[0x100]{}, session[0x100]{}, lobby[0x40]{};
    auto pointer = [](void* at, const void* value) { std::memcpy(at, &value, sizeof(value)); };
    pointer(world + 0xC0, session);  // online (7859A0)
    pointer(world + 0xD0, session);
    pointer(session + 0x30, lobby);  // 734F00 compares the local member with the lobby owner
    auto* singleton = const_cast<unsigned char*>(base) + 0x20B2AC0;
    void* previous = nullptr;
    std::memcpy(&previous, singleton, sizeof(previous));
    DWORD old = 0;
    const bool writable = VirtualProtect(singleton, sizeof(previous), PAGE_READWRITE, &old) != FALSE;
    Check(writable, "the vehicle test can supply a private world snapshot");
    if (!writable) return;
    pointer(singleton, world + 0x98);
    struct Control { void* table = nullptr; long uses = 1, weaks = 2; } vehicleControl, riderControl, lastControl;
    alignas(16) std::uint8_t vehicle[0x1000]{}, seat[0x340]{}, rider[0x200]{}, last[0x200]{}, target[0x800]{};
    // This real vehicle vtable carries the RTTI that 22FCA0 uses to return its NetworkObject at +0x120.
    Check(Slot(0x17D8B50, 34) == 0x6347C0, "the native vehicle fixture has the VehicleBase acceptance rule");
    pointer(vehicle, base + 0x17D8B50);
    pointer(vehicle + 0x608, seat);
    const std::uint64_t seats = 1;
    std::memcpy(vehicle + 0x618, &seats, sizeof(seats));
    const std::uint32_t local = 2;
    std::memcpy(vehicle + 0x128, &local, sizeof(local));
    std::memcpy(target + 0x128, &local, sizeof(local));
    std::memcpy(last + 0x128, &local, sizeof(local));
    pointer(seat + 0x300, last);
    pointer(seat + 0x308, &lastControl);  // the client's previous local driver, still alive
    alignas(16) std::uint8_t gdi[kGdiBytes]{};
    pointer(gdi + kGdiAttacker, vehicle);
    pointer(gdi + kGdiAttacker + 8, &vehicleControl);
    const float damage = 50.0f;
    std::memcpy(gdi + kGdiDamage, &damage, sizeof(damage));
    SetHitRuleForTest(true, true);
    int deciders = 0;
    for (int state = 0; state < 6; ++state) {
        const bool host = state == 0 || state == 4;
        pointer(lobby + 0x18, host ? nullptr : lobby);  // local member is null: equal only on the host
        Check(Fn<bool(__fastcall*)(const void*)>(0x784210)(nullptr) == host, "native host lookup matches the machine snapshot");
        pointer(seat + 0x260, state == 1 ? nullptr : rider);
        pointer(seat + 0x268, state == 1 ? nullptr : &riderControl);
        riderControl.uses = state == 2 ? 0 : 1;  // host Dummy, client empty / expired / Dummy
        const std::uint32_t riderFlags = state == 4 ? 1 : state == 5 ? 2 : 0;
        std::memcpy(rider + 0x128, &riderFlags, sizeof(riderFlags));
        Check(Fn<int(__fastcall*)(const void*, bool, bool)>(0x630F90)(vehicle, true, true) == (state == 4 ? 2 : 1),
              "stock runner reads current registered riders, or the Dummy and the client's last local driver");
        std::uint32_t message = kDamageMessage;
        void* payload = gdi;
        CpuContext ctx{};
        ctx.rcx = reinterpret_cast<std::uint64_t>(target);
        ctx.rdx = reinterpret_cast<std::uint64_t>(&message);
        ctx.r8 = reinterpret_cast<std::uint64_t>(&payload);
        HitPreFilterHandler(&ctx);
        const bool decides = message == kDamageMessage;
        if (state < 2) deciders += decides;
        Check(decides == (state < 4 ? host : state == 5),
              "only a live registered driver replaces the host as vehicle damage authority");
        Check(vehicleControl.uses == 1 && vehicleControl.weaks == 2, "the native attacker lookup balances its references");
    }
    Check(deciders == 1, "host Dummy plus client empty seat has exactly one damage authority");
    ClearHitRuleForTest();
    std::memcpy(singleton, &previous, sizeof(previous));
    VirtualProtect(singleton, sizeof(previous), old, &old);
}

}  // namespace

int main(int argc, char** argv) {
    TestOwner();
    TestVehicleShooter();
    TestDecision();
    TestRuleClock();
    TestOwnerTakesEvent();
    TestSwitchWindow();
    TestSerialization();
    TestGdi();
    TestCheck();
    TestGate();
    TestRate();
    if (argc > 1) {
        const std::string path = argv[1];
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::printf("SKIP: no EDF.dll at %s\n", path.c_str());
            return 77;
        }
        // Mapped and relocated, its imports left unresolved and nothing of it run: only code that needs no import is
        // called (the stream code, the game's receive with our type).
        const HMODULE game = LoadLibraryExA(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!game) {
            std::printf("FAIL: EDF.dll cannot be mapped (error %lu)\n", GetLastError());
            return 1;
        }
        if (!ResolveRuntimeImports(game)) {
            std::printf("FAIL: the C runtime imports of EDF.dll cannot be filled in (error %lu)\n", GetLastError());
            return 1;
        }
        base = reinterpret_cast<const unsigned char*>(game);
        TestGameCode();
        TestPreFilter();
        TestForwardReferenceOwnership();
        TestVehicleSeatAuthority();
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
