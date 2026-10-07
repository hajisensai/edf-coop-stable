#include "netplayer_game.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "code.h"
#include "identity.h"
#include "log.h"
#include "netfeature.h"
#include "netaoi.h"
#include "netplayer.h"
#include "netplayer_members.h"
#include "netplayer_tracks.h"
#include "patches.h"

namespace multislot {
namespace {

// SoldierBase fields (docs/net-re/player.md, each tied to an instruction checked in kChecks below).
constexpr std::size_t kNetFlags = 0x128;        // byte: bit 0 another machine's copy, bit 1 this machine's
constexpr std::size_t kPosition = 0x90;         // vec4, what the record's position bit sends (EDF+77AD03)
constexpr std::size_t kProxy = 0x680;           // the Havok character proxy holder (EDF+5961AA)
constexpr std::size_t kAddedVelocity = 0x840;   // vec4: one-step velocity added by the proxy step (EDF+596335)
constexpr std::size_t kSyncFrames = 0x1820;     // u32: sync frames written so far (EDF+5781B0, this-0x120)
constexpr std::size_t kSyncControl = 0x1830;    // HumanCharacterSyncControl (EDF+59621F)
constexpr std::size_t kSyncEnabled = kSyncControl + 0x00;  // byte, set by EDF+77AD40
constexpr std::size_t kSyncSettled = kSyncControl + 0x20;  // byte: nothing to correct (EDF+77ADA1)
constexpr std::size_t kSyncNewData = kSyncControl + 0x21;  // byte: a position arrived (EDF+77AB34)
constexpr std::size_t kIsPlayerSlot = 33;       // vtable: controlled by a player (EDF+550890)
constexpr float kStepRate = 59.999996f;          // the game's own factor from displacement to added velocity (EDF+17B0A10)

constexpr std::uint32_t kProxyMatrix = 0x11AF6F0;  // (holder, float out[16]): translation at +0x30
constexpr std::uint32_t kProxyWarp = 0x11B9850;    // (holder, const float pos[4]): the game's own warp (EDF+596318)

struct Check {
    const char* name;
    std::uint32_t rva;
    std::vector<std::uint8_t> bytes;
};
const std::vector<Check>& Checks() {
    static const std::vector<Check> checks = {
        {"sync frame counter", 0x5781B0, {0xFF, 0x81, 0x00, 0x17, 0x00, 0x00, 0xB0, 0x01}},
        {"NetworkUpdate sync control", 0x59621F, {0x48, 0x8D, 0x8F, 0x30, 0x18, 0x00, 0x00}},
        {"NetworkUpdate proxy", 0x5961AA, {0x48, 0x8D, 0x9F, 0x80, 0x06, 0x00, 0x00}},
        {"NetworkUpdate added velocity", 0x596335, {0x0F, 0x10, 0x8F, 0x40, 0x08, 0x00, 0x00}},
        {"NetworkUpdate warp", 0x596311, {0x48, 0x8D, 0x55, 0x10, 0x48, 0x8B, 0xCB, 0xE8, 0x33, 0x35, 0xC2, 0x00}},
        {"sync control enable", 0x77AD40, {0x38, 0x11, 0x74, 0x20, 0x88, 0x11, 0x84, 0xD2}},
        {"sync control settled", 0x77ADA1, {0x80, 0x79, 0x20, 0x00}},
        {"sync control read", 0x77AB25, {0x80, 0x7B, 0x22, 0x00, 0x75, 0x0F, 0x0F, 0x28, 0x44, 0x24, 0x20, 0x0F, 0x11, 0x43, 0x10}},
        {"record position", 0x77AD03, {0x49, 0x8D, 0x90, 0x90, 0x00, 0x00, 0x00}},
        {"is player", 0x550890, {0x48, 0x83, 0xB9, 0x40, 0x03, 0x00, 0x00, 0x00, 0x75, 0x0D}},
        {"proxy matrix", kProxyMatrix, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x11}},
        {"proxy warp", kProxyWarp, {0xE9, 0x0B, 0x62, 0xFF, 0xFF}},
    };
    return checks;
}

// [class][kind]: 0 SoldierBase, AssultSoldier, Engineer, HeavyArmor (one set of functions), 1 PaleWing.
enum Kind { kMask, kWrite, kRead, kUpdate, kKinds };
constexpr std::uint32_t kOriginal[2][kKinds] = {
    {0x59FB10, 0x59FDB0, 0x592180, 0x596130},
    {0x582DF0, 0x582E40, 0x57FCC0, 0x5803C0},
};
struct SlotSite {
    std::uint32_t rva;
    int family;
    Kind kind;
};
const std::vector<SlotSite>& Sites() {
    // vtables: AssultSoldier 17CDF28, Engineer 17CF100, HeavyArmor 17CF5B8, SoldierBase 17D24D8, PaleWing 17D0FF8;
    // slot 92 at +0x2E0, 52 at +0x1A0, 53 at +0x1A8, 55 at +0x1B8.
    static const std::vector<SlotSite> sites = {
        {0x17CE208, 0, kMask},   {0x17CF3E0, 0, kMask},   {0x17CF898, 0, kMask},   {0x17D27B8, 0, kMask},
        {0x17D12D8, 1, kMask},   {0x17CE0C8, 0, kWrite},  {0x17CF2A0, 0, kWrite},  {0x17CF758, 0, kWrite},
        {0x17D2678, 0, kWrite},  {0x17D1198, 1, kWrite},  {0x17CE0D0, 0, kRead},   {0x17CF2A8, 0, kRead},
        {0x17CF760, 0, kRead},   {0x17D2680, 0, kRead},   {0x17D11A0, 1, kRead},   {0x17CE0E0, 0, kUpdate},
        {0x17CF2B8, 0, kUpdate}, {0x17CF770, 0, kUpdate}, {0x17D2690, 0, kUpdate}, {0x17D11B0, 1, kUpdate},
    };
    return sites;
}

using MaskFn = std::uint16_t (*)(void*);
using RecordFn = void (*)(void*, void*, std::uint16_t);
using UpdateFn = void (*)(void*);
using MatrixFn = void* (*)(void*, float*);
using WarpFn = void (*)(void*, const float*);

unsigned char* game = nullptr;
PlayerSyncSettings settings;
NetPlayerParams params;

template <typename F>
F GameFn(std::uint32_t rva) {
    return reinterpret_cast<F>(game + rva);
}

template <typename T>
T Get(const void* object, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
    return value;
}
template <typename T>
void Put(void* object, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(object) + offset, &value, sizeof(value));
}

double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Remote(const void* soldier) { return (Get<std::uint8_t>(soldier, kNetFlags) & 1) != 0; }
bool Local(const void* soldier) { return (Get<std::uint8_t>(soldier, kNetFlags) & 2) != 0; }
bool IsPlayer(void* soldier) {
    const auto vtable = Get<void**>(soldier, 0);
    return reinterpret_cast<bool (*)(void*)>(vtable[kIsPlayerSlot])(soldier);
}
Vec3 RecordPosition(const void* soldier) {
    return {Get<float>(soldier, kPosition), Get<float>(soldier, kPosition + 4), Get<float>(soldier, kPosition + 8)};
}

SRWLOCK lock = SRWLOCK_INIT;
PlayerTracks<SendTrack> sent;
PlayerTracks<RemoteTrack> remotes;
struct Counters {
    std::uint64_t sent = 0, received = 0, refused = 0, driven = 0, warps = 0;
    double loggedMs = 0.0;
} counters;

void LogCounters(double now) {
    if (!DetailLog() || now - counters.loggedMs < 30000.0) return;
    if (counters.loggedMs > 0.0 && (counters.sent || counters.received))
        Log("PLAYER sync: %llu blocks sent, %llu received (%llu refused), %llu copy frames driven, %llu warps",
            counters.sent, counters.received, counters.refused, counters.driven, counters.warps);
    counters.loggedMs = now;
}

bool Sending(void* soldier) {
    if (!NetFeatureActive(NetFeature::PlayerSync) || !Local(soldier) || !IsPlayer(soldier)) return false;
    const auto frames = Get<std::uint32_t>(soldier, kSyncFrames);
    return frames % static_cast<std::uint32_t>(settings.sendIntervalFrames) == 0;
}

template <int F>
std::uint16_t MaskHook(void* soldier) {
    std::uint16_t mask = GameFn<MaskFn>(kOriginal[F][kMask])(soldier);
    if (Sending(soldier)) mask = static_cast<std::uint16_t>(mask | kPlayerBlockBit);
    return mask;
}

template <int F>
void WriteHook(void* soldier, void* writer, std::uint16_t mask) {
    GameFn<RecordFn>(kOriginal[F][kWrite])(soldier, writer, mask);
    if (!(mask & kPlayerBlockBit)) return;
    const double now = NowMs();
    std::uint8_t block[kPlayerBlockBytes];
    AcquireSRWLockExclusive(&lock);
    SendTrack* track = sent.Find(soldier, now, true);
    const PlayerSample sample = MakeSample(*track, RecordPosition(soldier), now);
    const bool written = EncodePlayerBlock(sample, block, sizeof(block)) && WriteBinElement(writer, block, sizeof(block));
    counters.sent += written ? 1 : 0;
    LogCounters(now);
    ReleaseSRWLockExclusive(&lock);
}

template <int F>
void ReadHook(void* soldier, void* reader, std::uint16_t mask) {
    GameFn<RecordFn>(kOriginal[F][kRead])(soldier, reader, mask);
    if (!(mask & kPlayerBlockBit)) return;
    // The block is the last element of the record: read it whatever is done with it, and a malformed one is
    // left where it is, inside this record's own data.
    std::uint8_t block[64];
    std::size_t size = 0;
    PlayerSample sample;
    const bool valid = ReadBinElement(reader, block, sizeof(block), size) && DecodePlayerBlock(block, size, sample);
    const double now = NowMs();
    AcquireSRWLockExclusive(&lock);
    if (!valid) {
        ++counters.refused;
    } else if (Remote(soldier)) {
        RemoteTrack* track = remotes.Find(soldier, now, true);
        counters.received += AcceptSample(*track, sample, now, params) ? 1 : 0;
    }
    ReleaseSRWLockExclusive(&lock);
}

Vec3 ProxyPosition(void* soldier, float& w) {
    alignas(16) float matrix[16]{};
    GameFn<MatrixFn>(kProxyMatrix)(static_cast<std::uint8_t*>(soldier) + kProxy, matrix);
    w = matrix[15];
    return {matrix[12], matrix[13], matrix[14]};
}

// Interest management's positions (netplayer_members.h, netaoi.h): every player object's member and place.
MemberPlaces& Places() {
    static MemberPlaces places(
        [](const void* productId) {
            if (!productId) return std::string();
            char text[64]{};
            ProductUserIdText(productId, text, sizeof(text));
            return std::string(text);
        },
        [](const std::string& member, const PlayerPlace& place) {
            MemberPlace m;
            m.position = {place.position.x, place.position.y, place.position.z};
            m.facing = {place.facing.x, place.facing.y, place.facing.z};
            m.engaged = place.engaged;
            NoteMember(member, m);
        },
        [](const std::string& member) { ForgetMember(member); });
    return places;
}
double expiredMs = 0.0;

// Called with the lock held.
void FeedPlace(void* soldier, double now) {
    if (now - expiredMs > 500.0) {
        expiredMs = now;
        Places().Expire(now);
    }
    if (!IsPlayer(soldier)) return;
    const void* user = PlayerUser(soldier);
    if (!user) return;
    Vec3 position = RecordPosition(soldier), velocity;
    if (Remote(soldier)) {
        // The estimate where this player's machine sends the block: where it is now, not where the copy got to.
        const RemoteTrack* track = remotes.Find(soldier, now, false);
        if (track && TrackFresh(*track, now, params)) {
            position = EstimatePosition(*track, now, params);
            velocity = track->last.velocity;
        }
    }
    Places().Observe(user, position, velocity, now);
}

template <int F>
void UpdateHook(void* soldier) {
    const UpdateFn original = GameFn<UpdateFn>(kOriginal[F][kUpdate]);
    const double now = NowMs();
    AcquireSRWLockExclusive(&lock);
    FeedPlace(soldier, now);
    ReleaseSRWLockExclusive(&lock);
    if (!Remote(soldier) || !NetFeatureActive(NetFeature::PlayerSync)) return original(soldier);
    AcquireSRWLockExclusive(&lock);
    RemoteTrack* track = remotes.Find(soldier, now, false);
    const bool drive = track && TrackFresh(*track, now, params);
    if (track && !drive) ResetSteps(*track);
    ReleaseSRWLockExclusive(&lock);
    if (!drive) return original(soldier);
    // The game's correction (EDF+77AD70) does nothing for a settled control without new data; our step replaces it.
    Put<std::uint8_t>(soldier, kSyncSettled, 1);
    Put<std::uint8_t>(soldier, kSyncNewData, 0);
    original(soldier);

    AcquireSRWLockExclusive(&lock);
    track = remotes.Find(soldier, now, false);
    if (!track) {
        ReleaseSRWLockExclusive(&lock);
        return;
    }
    // A disabled control (riding, ragdoll, a state the game itself does not correct in) is left alone.
    if (!Get<std::uint8_t>(soldier, kSyncEnabled)) {
        ResetSteps(*track);
        ReleaseSRWLockExclusive(&lock);
        return;
    }
    float w = 1.0f;
    const Vec3 current = ProxyPosition(soldier, w);
    const RemoteStep step = StepRemote(*track, current, now, params);
    ++counters.driven;
    counters.warps += step.warp ? 1 : 0;
    ReleaseSRWLockExclusive(&lock);
    if (step.warp) {
        alignas(16) const float target[4] = {step.target.x, step.target.y, step.target.z, w};
        GameFn<WarpFn>(kProxyWarp)(static_cast<std::uint8_t*>(soldier) + kProxy, target);
        return;
    }
    // Displacement for this step, as the game adds its own correction: velocity = displacement * 60.
    Put<float>(soldier, kAddedVelocity + 0, Get<float>(soldier, kAddedVelocity + 0) + step.add.x * kStepRate);
    Put<float>(soldier, kAddedVelocity + 4, Get<float>(soldier, kAddedVelocity + 4) + step.add.y * kStepRate);
    Put<float>(soldier, kAddedVelocity + 8, Get<float>(soldier, kAddedVelocity + 8) + step.add.z * kStepRate);
}

void* Handler(int family, Kind kind) {
    switch (kind) {
        case kMask: return family ? reinterpret_cast<void*>(&MaskHook<1>) : reinterpret_cast<void*>(&MaskHook<0>);
        case kWrite: return family ? reinterpret_cast<void*>(&WriteHook<1>) : reinterpret_cast<void*>(&WriteHook<0>);
        case kRead: return family ? reinterpret_cast<void*>(&ReadHook<1>) : reinterpret_cast<void*>(&ReadHook<0>);
        default: return family ? reinterpret_cast<void*>(&UpdateHook<1>) : reinterpret_cast<void*>(&UpdateHook<0>);
    }
}

int Clamped(UINT value, int low, int high) { return std::clamp(static_cast<int>(value), low, high); }

}  // namespace

PlayerSyncSettings ReadPlayerSyncSettings(const wchar_t* iniPath) {
    PlayerSyncSettings s;
    s.sendIntervalFrames = Clamped(GetPrivateProfileIntW(L"NetPlayer", L"SendIntervalFrames", 2, iniPath), 1, 6);
    const int flush = static_cast<int>(GetPrivateProfileIntW(L"NetPlayer", L"FlushIntervalMs", 45, iniPath));
    s.flushIntervalMs = flush <= 0 ? 0 : std::clamp(flush, 16, 90);
    s.tauMs = static_cast<float>(Clamped(GetPrivateProfileIntW(L"NetPlayer", L"ConvergeMs", 80, iniPath), 10, 1000));
    s.maxExtrapolateMs =
        static_cast<float>(Clamped(GetPrivateProfileIntW(L"NetPlayer", L"MaxExtrapolateMs", 200, iniPath), 0, 500));
    s.snapDistance = static_cast<float>(Clamped(GetPrivateProfileIntW(L"NetPlayer", L"SnapDistance", 8, iniPath), 1, 100));
    s.feedForward =
        static_cast<float>(Clamped(GetPrivateProfileIntW(L"NetPlayer", L"FeedForwardPercent", 100, iniPath), 0, 100)) / 100.0f;
    return s;
}

bool InstallPlayerSync(unsigned char* base, const PlayerSyncSettings& wanted) {
    for (const auto& check : Checks()) {
        if (std::memcmp(base + check.rva, check.bytes.data(), check.bytes.size()) != 0) {
            Log("PLAYER sync: OFF - EDF+%X (%s) is not the expected code; the game's own player sync is used",
                check.rva, check.name);
            return false;
        }
    }
    for (const auto& site : Sites()) {
        if (!SlotTargets(base + site.rva, reinterpret_cast<std::uint64_t>(base), kOriginal[site.family][site.kind])) {
            Log("PLAYER sync: OFF - vtable entry EDF+%X does not point at EDF+%X (another mod?); nothing changed",
                site.rva, kOriginal[site.family][site.kind]);
            return false;
        }
    }
    // mov dword ptr [r14+0x58], imm32 (90.0f)
    const std::uint8_t flushOriginal[] = {0x41, 0xC7, 0x46, 0x58, 0x00, 0x00, 0xB4, 0x42};
    // Decided by this machine's own switch (netplayer.h EffectiveFlushIntervalMs): [Netcode] PlayerSync=0 leaves 90 ms.
    const int flushMs = EffectiveFlushIntervalMs(wanted.flushIntervalMs, NetFeatureEnabledLocally(NetFeature::PlayerSync));
    const bool flush = flushMs > 0;
    if (flush && std::memcmp(base + kFlushIntervalSite, flushOriginal, sizeof(flushOriginal)) != 0) {
        Log("PLAYER sync: OFF - the packet flush interval at EDF+%X is not the game's 90 ms (W1 or another mod "
            "changed it?); nothing changed",
            kFlushIntervalSite);
        return false;
    }
    game = base;
    settings = wanted;
    params.tauMs = wanted.tauMs;
    params.maxExtrapolateMs = wanted.maxExtrapolateMs;
    params.snapDistance = wanted.snapDistance;
    params.feedForward = wanted.feedForward;

    struct Write {
        std::uint32_t rva;
        std::vector<std::uint8_t> original, replacement;
    };
    std::vector<Write> writes;
    for (const auto& site : Sites()) {
        const auto address = reinterpret_cast<std::uint64_t>(Handler(site.family, site.kind));
        std::vector<std::uint8_t> bytes(sizeof(address));
        std::memcpy(bytes.data(), &address, sizeof(address));
        writes.push_back({site.rva, {base + site.rva, base + site.rva + 8}, std::move(bytes)});
    }
    if (flush) {
        const float interval = static_cast<float>(flushMs);
        std::vector<std::uint8_t> bytes(flushOriginal, flushOriginal + sizeof(flushOriginal));
        std::memcpy(bytes.data() + 4, &interval, sizeof(interval));
        writes.push_back({kFlushIntervalSite, {flushOriginal, flushOriginal + sizeof(flushOriginal)}, std::move(bytes)});
    }
    for (std::size_t i = 0; i < writes.size(); ++i) {
        if (!WriteCode(base + writes[i].rva, writes[i].replacement.data(), writes[i].replacement.size())) {
            Log("PLAYER sync: OFF - could not write EDF+%X (error %lu); restored", writes[i].rva, GetLastError());
            for (std::size_t j = i; j-- > 0;)
                WriteCode(base + writes[j].rva, writes[j].original.data(), writes[j].original.size());
            return false;
        }
    }
    Log("PLAYER sync: hooks installed (%zu vtable slots%s). Players send position, velocity and time every %d sync "
        "frame(s); remote players with that data are extrapolated (up to %.0f ms) and converge in %.0f ms (warp above "
        "%.0f m, feed-forward %.0f%%); players without it keep the game's own sync. Packet flush every %d ms",
        Sites().size(), flush ? " + flush interval" : "", settings.sendIntervalFrames, params.maxExtrapolateMs,
        params.tauMs, params.snapDistance, params.feedForward * 100.0f, flush ? flushMs : 90);
    return true;
}

}  // namespace multislot
