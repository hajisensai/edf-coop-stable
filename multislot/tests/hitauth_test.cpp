// Hit authority (src/hitauth.h, src/hitauthgame.h).
//   HitAuthTests                the event, the decision and the owner's checks
//   HitAuthTests <EDF.dll>      against the game's code: the hook sites, the game facts the rule rests on, the event's
//                               round trip through the game's own stream code, and the game's receive passing it by
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "native_runtime.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
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

    // A target the game already leaves to the shooter's machine (enemies, NPCs, objects): the game's rule.
    HitInput enemy;
    enemy.forwarding = enemy.dropping = true;
    enemy.damage = 10.0f;
    enemy.shooterDecides = true;
    enemy.attacker = enemy.gameAttacker = NetOwner::Local;
    enemy.target = NetOwner::Remote;
    Check(DecideHit(enemy) == HitVerdict::Vanilla,
          "our shot at another machine's enemy: dealt on our copy as the game does, no event");
    enemy.attacker = enemy.gameAttacker = NetOwner::Remote;
    enemy.target = NetOwner::Local;
    Check(DecideHit(enemy) == HitVerdict::Vanilla, "a remote shot at our enemy: the game's rule (its machine deals it)");
    // A vehicle registered on the host, fired by our driver: the game would leave the hit to the host.
    enemy.attacker = NetOwner::Local;
    enemy.gameAttacker = NetOwner::Remote;
    enemy.target = NetOwner::Remote;
    Check(DecideHit(enemy) == HitVerdict::Forward, "our driver's shot from the host's vehicle still goes as an event");
    enemy.attacker = NetOwner::Remote;
    enemy.gameAttacker = NetOwner::Local;
    enemy.target = NetOwner::Local;
    Check(DecideHit(enemy) == HitVerdict::Drop, "the host drops its copy of a client driver's shot from its vehicle");
    HitInput player = enemy;
    player.shooterDecides = false;
    player.attacker = player.gameAttacker = NetOwner::Local;
    player.target = NetOwner::Remote;
    Check(DecideHit(player) == HitVerdict::Forward, "our shot at another machine's player or vehicle goes as an event");
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
    s.attacker = s.gameAttacker = NetOwner::Local;
    s.target = NetOwner::Remote;
    s.shooterDecides = !ownerDecides;
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
    r.attacker = r.gameAttacker = NetOwner::Remote;
    r.target = NetOwner::Local;
    r.shooterDecides = !ownerDecides;
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
    Check(kDamageEventBaseBytes == 94, "the event without sub-hits is 94 bytes (version 2's size)");
    Check(kDamageEventBytes == 94 + 1 + 4 * 16, "with four sub-hits 159");
    const std::size_t n = WriteDamageEvent(e, bytes, sizeof(bytes));
    Check(n == kDamageEventBaseBytes + 1 && bytes[2] == 3, "an event without sub-hits: version 3, a zero count");
    Check(WriteDamageEvent(e, bytes, n - 1) == 0, "no room: nothing written");
    DamageEvent out;
    Check(ReadDamageEvent(bytes, n, out) && Same(e, out) && out.bodyCount == 0, "the event reads back as written");
    Check(!ReadDamageEvent(bytes, n - 1, out), "a short event is refused");
    Check(!ReadDamageEvent(bytes, n + 1, out), "a long event is refused");
    std::uint8_t bad[kDamageEventBytes];
    std::memcpy(bad, bytes, n);
    bad[0] = 'X';
    Check(!ReadDamageEvent(bad, n, out), "a wrong magic is refused");
    std::memcpy(bad, bytes, n);
    bad[2] = kDamageEventVersion + 1;
    Check(!ReadDamageEvent(bad, n, out), "another version is refused");
    // Version 2 (the build before): the same 94 bytes and no count; read as an event without sub-hits.
    std::memcpy(bad, bytes, kDamageEventBaseBytes);
    bad[2] = kDamageEventVersionNoBodies;
    Check(ReadDamageEvent(bad, kDamageEventBaseBytes, out) && Same(e, out) && out.bodyCount == 0,
          "a version 2 event reads as one without sub-hits");
    Check(!ReadDamageEvent(bad, kDamageEventBaseBytes + 1, out), "a version 2 event has exactly 94 bytes");
    DamageEvent net = e;
    net.flags |= kGdiFromNetwork;
    WriteDamageEvent(net, bytes, sizeof(bytes));
    Check(ReadDamageEvent(bytes, n, out) && !(out.flags & kGdiFromNetwork) && out.flags == e.flags,
          "the from-network flag never travels");
    // Sub-hits.
    DamageEvent hit = e;
    hit.bodyCount = 2;
    hit.bodies[0] = HitBody{0xAA0, 7, {1.0f, 2.0f, 3.0f}};
    hit.bodies[1] = HitBody{0xB60, 0, {-4.0f, 5.5f, 6.0f}};
    const std::size_t m = WriteDamageEvent(hit, bytes, sizeof(bytes));
    Check(m == kDamageEventBaseBytes + 1 + 2 * kHitBodyBytes, "two sub-hits: 16 bytes each after the count");
    Check(ReadDamageEvent(bytes, m, out) && Same(hit, out) && out.bodyCount == 2 && out.bodies[0].component == 0xAA0 &&
              out.bodies[0].index == 7 && out.bodies[1].component == 0xB60 && out.bodies[1].point[1] == 5.5f,
          "the sub-hits read back as written");
    Check(!ReadDamageEvent(bytes, m - 1, out) && !ReadDamageEvent(bytes, m + 16, out), "the count fixes the size");
    std::memcpy(bad, bytes, m);
    bad[kDamageEventBaseBytes] = 5;
    Check(!ReadDamageEvent(bad, kDamageEventBaseBytes + 1 + 5 * kHitBodyBytes, out), "more than four sub-hits are refused");
    const float nan = std::nanf("");
    std::memcpy(bad, bytes, m);
    std::memcpy(bad + kDamageEventBaseBytes + 1 + 4, &nan, 4);
    Check(!ReadDamageEvent(bad, m, out), "a sub-hit point that is not a number is refused");
    hit.bodyCount = kMostHitBodies + 1;
    Check(WriteDamageEvent(hit, bytes, sizeof(bytes)) == 0, "an event with more sub-hits than it carries is not written");
}

// A ragdoll component as 6E3B10 / 6EA430 lay it out: bodies [+0x78], count [+0x88], 0xC0 each, the rigid body
// +0x50 with its Havok id at +0xF0; body+0xB8 set means a second id through [[+0xB0]+0x10]+0xF0.
struct FakeRagdoll {
    alignas(16) std::uint8_t component[0x100]{};
    alignas(16) std::uint8_t bodies[4 * kRagdollBodyBytes]{};
    alignas(16) std::uint8_t rigid[4][0x100]{};
    alignas(16) std::uint8_t second[0x100]{}, secondHolder[0x20]{};
    explicit FakeRagdoll(std::uint32_t firstId) {
        const void* b = bodies;
        std::memcpy(component + kRagdollBodies, &b, 8);
        const std::size_t count = 4;
        std::memcpy(component + kRagdollCount, &count, 8);
        for (int i = 0; i < 4; ++i) {
            const void* r = rigid[i];
            std::memcpy(bodies + i * kRagdollBodyBytes + 0x50, &r, 8);
            const std::uint32_t id = firstId + static_cast<std::uint32_t>(i);
            std::memcpy(rigid[i] + 0xF0, &id, 4);
        }
        // body 2 also answers to firstId + 100 (its second id)
        const void* holder = secondHolder;
        std::memcpy(bodies + 2 * kRagdollBodyBytes + 0xB0, &holder, 8);
        const std::uint64_t set = 1;
        std::memcpy(bodies + 2 * kRagdollBodyBytes + 0xB8, &set, 8);
        const void* sec = second;
        std::memcpy(secondHolder + 0x10, &sec, 8);
        const std::uint32_t id2 = firstId + 100;
        std::memcpy(second + 0xF0, &id2, 4);
    }
};

void TestRagdollBodies() {
    FakeRagdoll r(500);
    Check(RagdollIndexOf(r.component, 501) == 1 && RagdollIndexOf(r.component, 503) == 3, "a body id's index");
    Check(RagdollIndexOf(r.component, 600) == 2, "the second id (6EA46C) finds its body too");
    Check(RagdollIndexOf(r.component, 77) == -1, "an id of no body: -1");
    std::uint32_t id = 0;
    Check(RagdollIdOf(r.component, 3, &id) && id == 503, "an index's id");
    Check(!RagdollIdOf(r.component, 4, &id) && !RagdollIdOf(r.component, -1, &id), "no such body");
    std::uint8_t empty[0x100]{};
    Check(RagdollIndexOf(empty, 500) == -1 && !RagdollIdOf(empty, 0, &id), "no component: nothing");
}

// The sender's search for a class's ragdoll (hitauthgame.cpp CollectHitBodies), without the game: a stand-in object
// with its ragdoll at +0xAA0 (EventAnimationObject's place) and GDIs of sub-hits.
void TestHitBodySearch() {
    struct Object {
        alignas(16) std::uint8_t bytes[0x2100]{};
        FakeRagdoll ragdoll;
        Object(std::uint32_t firstId, const void* vtable, std::size_t at = 0xAA0) : ragdoll(firstId) {
            std::memcpy(bytes, &vtable, 8);
            std::memcpy(bytes + at + kRagdollBodies, ragdoll.component + kRagdollBodies, 0x18);
        }
    };
    struct Gdi {
        alignas(16) std::uint8_t bytes[kGdiBytes]{};
        alignas(16) std::uint8_t hits[4 * kHitRecordBytes]{};
        explicit Gdi(std::initializer_list<std::uint32_t> ids) {
            std::size_t n = 0;
            for (const std::uint32_t id : ids) {
                const float point[4] = {static_cast<float>(n), 1.0f, 2.0f, 1.0f};
                std::memcpy(hits + n * kHitRecordBytes, point, 16);
                std::memcpy(hits + n * kHitRecordBytes + kHitRecordBody, &id, 4);
                ++n;
            }
            const void* at = hits;
            std::memcpy(bytes + kGdiHits, &at, 8);
            std::memcpy(bytes + kGdiHitCount, &n, 8);
        }
    };
    // The first sub-hit is another object's body, the second this one's: the search tries both.
    static const std::uint64_t firstTable[4]{};
    static Object first(700, firstTable);
    DamageEvent e;
    HitBodiesFromGdi(first.bytes, Gdi{55555, 702}.bytes, e, 10000);
    Check(e.bodyCount == 1 && e.bodies[0].component == 0xAA0 && e.bodies[0].index == 2 && e.bodies[0].point[0] == 1.0f,
          "a first sub-hit on another object's body does not hide the target's own");
    // A class's first shot touches none of its bodies: the class is not given up, the next shot finds the ragdoll.
    static const std::uint64_t laterTable[4]{};
    static Object later(800, laterTable);
    HitBodiesFromGdi(later.bytes, Gdi{55555}.bytes, e, 20000);
    Check(e.bodyCount == 0, "no body of the target: nothing sent");
    HitBodiesFromGdi(later.bytes, Gdi{803}.bytes, e, 20000 + 10);
    Check(e.bodyCount == 0, "the class is searched again only after a while");
    HitBodiesFromGdi(later.bytes, Gdi{803}.bytes, e, 20000 + 1000);
    Check(e.bodyCount == 1 && e.bodies[0].component == 0xAA0 && e.bodies[0].index == 3,
          "a class whose first search found nothing is searched again and its ragdoll found");
    HitBodiesFromGdi(later.bytes, Gdi{55555, 801, 800}.bytes, e, 20000 + 1001);
    Check(e.bodyCount == 2 && e.bodies[0].index == 1 && e.bodies[1].index == 0, "then the class's offset is used");

    // A ragdoll at a place no class has one (+0x7A0) is not looked for: nothing is sent that the owner would refuse.
    static const std::uint64_t strayTable[4]{};
    static Object stray(900, strayTable, 0x7A0);
    HitBodiesFromGdi(stray.bytes, Gdi{902}.bytes, e, 30000);
    Check(e.bodyCount == 0, "the sender looks only at the known ragdoll places");

    // The owner reads only known places, and for a class whose place it knows (it found it shooting one) only that.
    struct OwnerGdi {
        alignas(16) std::uint8_t bytes[kGdiBytes]{};
        alignas(16) std::uint8_t hits[4 * kHitRecordBytes]{};
        OwnerGdi() {
            const void* at = hits;
            const std::size_t capacity = 4;
            std::memcpy(bytes + kGdiHits, &at, 8);
            std::memcpy(bytes + kGdiHitCapacity, &capacity, 8);
        }
        std::size_t count() const {
            std::size_t n = 0;
            std::memcpy(&n, bytes + kGdiHitCount, 8);
            return n;
        }
        std::uint32_t id(std::size_t k) const {
            std::uint32_t v = 0;
            std::memcpy(&v, hits + k * kHitRecordBytes + kHitRecordBody, 4);
            return v;
        }
    };
    DamageEvent forwarded = SampleEvent();
    forwarded.bodyCount = 3;
    forwarded.bodies[0] = HitBody{0xAA0, 1, {0, 0, 0}};  // `later`'s place: its body 1 is 801
    forwarded.bodies[1] = HitBody{0xB60, 0, {0, 0, 0}};  // a known place, but not this class's
    forwarded.bodies[2] = HitBody{0x7A0, 0, {0, 0, 0}};  // no class's place
    OwnerGdi rebuilt;
    HitBodiesToGdi(later.bytes, rebuilt.bytes, forwarded);
    Check(rebuilt.count() == 1 && rebuilt.id(0) == 801, "only the class's own place is read on the owner");
    static const std::uint64_t unseenTable[4]{};
    static Object unseen(600, unseenTable);  // a class this machine never shot: any known place, none other
    forwarded.bodies[0] = HitBody{0x7A0, 0, {0, 0, 0}};
    forwarded.bodies[1] = HitBody{0xAA0, 2, {0, 0, 0}};
    forwarded.bodyCount = 2;
    OwnerGdi other;
    HitBodiesToGdi(unseen.bytes, other.bytes, forwarded);
    Check(other.count() == 1 && other.id(0) == 602, "a class without a found place: known places only");
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
    // Sub-hits name only places where a class has a ragdoll (kRagdollPlaces): any other place is no sender's, and the
    // owner would read it in its object - the event is refused, and not remembered.
    DamageEvent placed = SampleEvent();
    placed.attackerRef = 0x78;
    placed.seq = 7;
    placed.bodyCount = 2;
    placed.bodies[0] = HitBody{0xB60, 2, {1, 2, 3}};
    placed.bodies[1] = HitBody{0x7A0, 0, {1, 2, 3}};  // 8-aligned, in the old search range, and no class's
    Check(gate.Admit(placed, v, now) == HitReject::BodyPlace, "a sub-hit at a place where no class has a ragdoll");
    placed.bodies[1] = HitBody{0x1F00, 0, {1, 2, 3}};
    Check(gate.Admit(placed, v, now) == HitReject::BodyPlace, "far into (or past) the object: refused");
    placed.bodies[1] = HitBody{0xAA0, 1, {1, 2, 3}};
    Check(gate.Admit(placed, v, now) == HitReject::None, "the same event with known places is dealt (not remembered "
                                                         "as seen before)");
    placed.seq = 8;
    placed.bodyCount = 1;
    placed.bodies[0] = HitBody{0xFFFF, 0, {1, 2, 3}};
    Check(gate.Admit(placed, notOwner, now) == HitReject::NotOwner, "a copy that is not the owner reads nothing anyway");
    Check(KnownRagdollPlace(0xB60) && KnownRagdollPlace(0xAA0) && KnownRagdollPlace(0xB00) && !KnownRagdollPlace(0) &&
              !KnownRagdollPlace(0x300) && !KnownRagdollPlace(0xB61),
          "UfoCarrier508 (+0xB60), EventAnimationObject (+0xAA0) and MovingFortress (+0xB00) are known places");
    Check(std::string(HitRejectName(HitReject::BodyPlace)) != "?", "the refusal has a name for the log");
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

// Class vtables for the fixtures, by their slot 34 rule: 54F8D0, 5A3600, 6347C0.
constexpr std::uint32_t kGameObjectVtable = 0x17CD3F0, kSoldierVtable = 0x17D24D8, kVehicleVtable = 0x17D8B50;

// The pre-filter's decisions on fake objects (the forward itself needs a game session: gamenet/real machine).
void TestPreFilter() {
    Check(Slot(kGameObjectVtable, 34) == 0x54F8D0 && Slot(kSoldierVtable, 34) == 0x5A3600 &&
              Slot(kVehicleVtable, 34) == 0x6347C0,
          "the fixture vtables carry the three slot 34 rules");
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
        const void* classTable = base + kVehicleVtable;  // decided where it is (6347C0): the events' case
        std::memcpy(object, &classTable, 8);
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

    // A target whose own rule already leaves hits to the shooter's machine: the game decides, nothing is sent.
    struct ClassCase {
        const char* what;
        std::uint32_t table;
        bool player;
        bool attackerRemote;
        std::uint32_t targetFlags;
        std::uint32_t expect;
    };
    const ClassCase classes[] = {
        {"our shot at another machine's enemy: dealt on our copy, as the game does", kGameObjectVtable, false, false, 1,
         kDamageMessage},
        {"someone else's shot at our enemy: the game's rule (its machine dealt it)", kGameObjectVtable, false, true, 2,
         kDamageMessage},
        {"our shot at another machine's soldier NPC: the game's", kSoldierVtable, false, false, 1, kDamageMessage},
        {"our shot at another machine's player: sent, dropped here", kSoldierVtable, true, false, 1, kDroppedMessage},
        {"someone else's shot at our player: dropped (its machine sends)", kSoldierVtable, true, true, 2,
         kDroppedMessage},
    };
    fakeSendResult = true;
    const int sendsBefore = fakeSends;
    for (const auto& c : classes) {
        alignas(16) std::uint8_t target[0x2000]{};
        const void* classTable = base + c.table;
        std::memcpy(target, &classTable, 8);
        const void* netTablePtr = netTable;
        std::memcpy(target + 0x120, &netTablePtr, 8);
        std::memcpy(target + 0x128, &c.targetFlags, 4);
        if (c.player) target[0x1ED0] = 1;  // its eos::User
        alignas(16) std::uint8_t gdi[kGdiBytes]{};
        gdi[kGdiOverride] = 1;
        gdi[kGdiOverrideRemote] = c.attackerRemote ? 1 : 0;
        const float damage = 50.0f;
        std::memcpy(gdi + kGdiDamage, &damage, 4);
        std::uint32_t damageMessage = kDamageMessage;
        void* gdiPayload = gdi;
        CpuContext call{};
        call.rcx = reinterpret_cast<std::uint64_t>(target);
        call.rdx = reinterpret_cast<std::uint64_t>(&damageMessage);
        call.r8 = reinterpret_cast<std::uint64_t>(&gdiPayload);
        HitPreFilterHandler(&call);
        Check(damageMessage == c.expect, c.what);
    }
    Check(fakeSends == sendsBefore + 1, "only the shot at the player went through the object's send");
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
    const void* classTable = base + kVehicleVtable;  // decided where it is: the forward's case
    std::memcpy(target, &classTable, sizeof(classTable));
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
    pointer(target, base + 0x17D8B50);  // the target is a vehicle too: decided where it is
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

// A forwarded hit on a weak point (docs/net-re/damage.md section 6.7): the sender's sub-hits as body places, through the
// game's stream code, rebuilt in the owner's GDI with the owner's own Havok ids, then the game's own UfoCarrier508
// filter (slot 16, 4F7190: the sub-hit nearest the hit point, its body mapped by 6EA430, == the core index +0xD8C while
// the hatch flag +0xD85 is up) asked on the owner's copy. Version 2's event carried no sub-hit: the filter said no.
void TestWeakPointFilter() {
    constexpr std::uint32_t kCarrierAccept = 0x4F7190, kBodyIndex = 0x6EA430, kReserve = 0x124190, kRelease = 0x11E9D0;
    Check(Bytes(kCarrierAccept, "40554883ec5080b9850d0000"), "4F7190 is UfoCarrier508's slot 16 (hatch flag +0xD85)");
    Check(Bytes(0x4F72B1, "488d8d600b0000"), "4F7190 maps the body through the ragdoll at +0xB60");
    Check(Bytes(kBodyIndex, "4c8b4178"), "6EA430 reads the bodies at +0x78");
    Check(Slot(0x17C9DC0, 16) == kCarrierAccept, "UfoCarrier508's vtable slot 16 is 4F7190");
    struct Carrier {
        alignas(16) std::uint8_t object[0x1700]{};
        FakeRagdoll ragdoll;
        Carrier(std::uint32_t firstId, const void* vtable) : ragdoll(firstId) {
            std::memcpy(object, &vtable, 8);
            std::memcpy(object + 0xB60 + kRagdollBodies, ragdoll.component + kRagdollBodies, 0x18);
        }
    };
    static const std::uint64_t senderTable[4]{}, ownerTable[4]{};
    static Carrier sender(900, senderTable), owner(4000, ownerTable);  // one ship, two physics worlds: other Havok ids
    Check(Fn<int(__fastcall*)(const void*, std::uint32_t)>(kBodyIndex)(sender.object + 0xB60, 902) == 2 &&
              Fn<int(__fastcall*)(const void*, std::uint32_t)>(kBodyIndex)(owner.object + 0xB60, 4002) == 2,
          "the game's own 6EA430 reads the stand-in ragdolls");

    // The sender's GDI: the bullet's two sub-hits, the core body nearest the hit point, another one not of this ship.
    alignas(16) std::uint8_t sent[kGdiBytes]{};
    alignas(16) std::uint8_t hits[2 * kHitRecordBytes]{};
    const float closeHit[4] = {10.5f, 2.5f, -7.0f, 1.0f}, awayHit[4] = {40.0f, 2.5f, -7.0f, 1.0f};
    std::memcpy(hits, closeHit, 16);
    const std::uint32_t coreId = 902, otherId = 77777;
    std::memcpy(hits + kHitRecordBody, &coreId, 4);
    std::memcpy(hits + kHitRecordBytes, awayHit, 16);
    std::memcpy(hits + kHitRecordBytes + kHitRecordBody, &otherId, 4);
    const void* hitsPtr = hits;
    std::memcpy(sent + kGdiHits, &hitsPtr, 8);
    const std::size_t two = 2;
    std::memcpy(sent + kGdiHitCount, &two, 8);
    DamageEvent e = SampleEvent();
    HitBodiesFromGdi(sender.object, sent, e, 5000);
    Check(e.bodyCount == 1 && e.bodies[0].component == 0xB60 && e.bodies[0].index == 2 && e.bodies[0].point[0] == 10.5f,
          "the sender finds the ship's ragdoll (+0xB60) by the body the shot touched, and leaves the other body out");

    // Through the game's stream code.
    Stream written;
    Fn<void*(__fastcall*)(void*, int)>(kStreamConstruct)(written.bytes, 0x40);
    Check(WriteHitEventMessage(written.bytes, e), "the event with its sub-hit is written into a game stream");
    Stream reader;
    ToReader(written, reader);
    DamageEvent got;
    Check(PeekHitEventMessage(reader.bytes, got) == HitMessage::Event && got.bodyCount == 1, "and read back");

    // The owner's GDI as Deal builds it, the sub-hit rebuilt, the game's filter asked.
    const auto accept = Fn<bool(__fastcall*)(void*, const void*)>(kCarrierAccept);
    auto dealt = [&](const DamageEvent& event, bool* filter) {
        alignas(16) std::uint8_t gdi[kGdiBytes]{};
        const float one = 1.0f;
        std::memcpy(gdi + kGdiExtra68, &one, 4);
        Fn<bool(__fastcall*)(void*, std::size_t)>(kReserve)(gdi + kGdiParts, 4);
        EventToGdi(event, gdi);
        HitBodiesToGdi(owner.object, gdi, event);
        std::size_t count = 0;
        std::memcpy(&count, gdi + kGdiHitCount, 8);
        std::uint32_t id = 0;
        const std::uint8_t* entries = nullptr;
        std::memcpy(&entries, gdi + kGdiHits, 8);
        if (count) std::memcpy(&id, entries + kHitRecordBody, 4);
        *filter = accept(owner.object, gdi);
        Fn<void(__fastcall*)(void*)>(kRelease)(gdi + kGdiParts);
        return count ? id : 0u;
    };
    owner.object[0xD85] = 1;
    std::int32_t core = 2;
    std::memcpy(owner.object + 0xD8C, &core, 4);
    bool taken = false;
    Check(dealt(got, &taken) == 4002, "the owner rebuilds the sub-hit with its own copy's id of that body");
    Check(taken, "the game's UfoCarrier508 filter takes the forwarded hit on its core");
    core = 1;
    std::memcpy(owner.object + 0xD8C, &core, 4);
    dealt(got, &taken);
    Check(!taken, "a hit on another body is still refused by the filter");
    core = 2;
    std::memcpy(owner.object + 0xD8C, &core, 4);
    DamageEvent old = got;
    old.bodyCount = 0;  // what version 2 carried
    dealt(old, &taken);
    Check(!taken, "without the sub-hit (version 2) the filter refuses the hit: the gap this closes");
    DamageEvent forged = got;
    forged.bodies[0].component = 0x1F00;  // a place holding no ragdoll: left out
    forged.bodies[1] = HitBody{0xB61, 0, {0, 0, 0}};  // misaligned: left out
    forged.bodyCount = 2;
    Check(dealt(forged, &taken) == 0 && !taken, "a sub-hit at a place with no such body is left out");
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
    TestRagdollBodies();
    TestHitBodySearch();
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
        if (!native_test::ResolveCrtImports(game)) {
            std::printf("FAIL: the C runtime imports of EDF.dll cannot be filled in (error %lu)\n", GetLastError());
            return 1;
        }
        base = reinterpret_cast<const unsigned char*>(game);
        TestGameCode();
        TestPreFilter();
        TestForwardReferenceOwnership();
        TestVehicleSeatAuthority();
        TestWeakPointFilter();
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
