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
    return a.seq == b.seq && a.attackerRef == b.attackerRef && a.targetRef == b.targetRef && a.kind == b.kind &&
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

void TestDecision() {
    HitInput in;
    in.active = true;
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
    off.active = false;
    Check(DecideHit(off) == HitVerdict::Vanilla, "off: the game's rule");
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

void TestSerialization() {
    const DamageEvent e = SampleEvent();
    std::uint8_t bytes[kDamageEventBytes + 8]{};
    Check(kDamageEventBytes == 86, "the event is 86 bytes");
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
    SetNetFeatureForTest(NetFeature::HitAuthority, true);
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
    SetNetFeatureForTest(NetFeature::HitAuthority, false);
    ToReader(written, hooked);
    HitObjectReceiveHandler(&ctx);
    Check(HitAuthorityCounters().inactive == after.inactive + 1, "with the feature off an event is not dealt");
}

// The pre-filter's decisions on fake objects (the forward itself needs a game session: gamenet/real machine).
void TestPreFilter() {
    SetNetFeatureForTest(NetFeature::HitAuthority, true);
    struct Case {
        const char* what;
        std::uint32_t targetFlags;
        bool attackerRemote;
        float damage;
        std::uint16_t flags;
        bool active;
        std::uint32_t expect;
    };
    const Case cases[] = {
        {"someone else's shot at our player: dropped", 2, true, 50.0f, 0, true, kDroppedMessage},
        {"someone else's shot at a third machine's object: dropped", 1, true, 50.0f, 0, true, kDroppedMessage},
        {"our shot at our object: the game deals it", 2, false, 50.0f, 0, true, kDamageMessage},
        {"the game's kill message: untouched", 1, true, 50.0f, kGdiFromNetwork, true, kDamageMessage},
        {"a heal: untouched", 2, true, -50.0f, 0, true, kDamageMessage},
        {"a target without network identity: untouched", 0, true, 50.0f, 0, true, kDamageMessage},
        {"feature off: untouched", 2, true, 50.0f, 0, false, kDamageMessage},
    };
    for (const auto& c : cases) {
        SetNetFeatureForTest(NetFeature::HitAuthority, c.active);
        alignas(16) std::uint8_t object[0x800]{};
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
    SetNetFeatureForTest(NetFeature::HitAuthority, true);
    HitPreFilterHandler(&ctx);
    Check(message == 0x10000007, "a message that is not damage passes");
    SetNetFeatureForTest(NetFeature::HitAuthority, false);
}

}  // namespace

int main(int argc, char** argv) {
    TestOwner();
    TestDecision();
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
    }
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
