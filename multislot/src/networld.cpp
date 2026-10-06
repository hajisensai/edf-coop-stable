#include "networld.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstring>

#include "code.h"
#include "crashlog.h"
#include "log.h"
#include "mission.h"
#include "netfeature.h"
#include "worldrng.h"

namespace multislot {
namespace {

constexpr std::uint16_t kRemoteOwned = 1;  // NetworkObject flag bit0 (782880)
constexpr std::uint32_t kSearchableStates = ~std::uint32_t{2};  // 54A3BA: `test [obj+0x39C], 0xFFFFFFFD`
constexpr int kLogLimit = 40;

std::atomic<std::uint64_t> kept{0}, chosen{0};
std::atomic<int> logLines{0};

// --- The game's message stream and object messages (as docs/net-re/damage.md section 6.3 uses them) ---
unsigned char* game = nullptr;
constexpr std::uint32_t kStreamConstruct = 0x79A460;  // (stream, 0x40), as 54CD28 builds one
constexpr std::uint32_t kStreamDestroy = 0x760180;
constexpr std::uint32_t kWriteSmall = 0x12B5790;      // a small int (-14..15) in one byte
constexpr std::uint32_t kWriteBlock = 0x12B5200;      // a byte block (0xA0 | len >> 8, len, bytes)
constexpr std::uint32_t kReadBlock = 0x12B4900;       // a block into another stream
constexpr std::size_t kStreamBytes = 0x5F8, kStreamData = 0x10, kStreamSize = 0x5F0;
constexpr std::size_t kSendToCopies = 0x80;           // NetworkObject +0x80 = 773DA0 (broadcast as event 7)
constexpr std::uint16_t kLocallyOwned = 2;

template <typename F>
F Fn(std::uint32_t rva) {
    return reinterpret_cast<F>(game + rva);
}

class GameStream {
public:
    GameStream() { Fn<void*(__fastcall*)(void*, int)>(kStreamConstruct)(bytes_, 0x40); }
    ~GameStream() { Fn<void(__fastcall*)(void*)>(kStreamDestroy)(bytes_); }
    GameStream(const GameStream&) = delete;
    GameStream& operator=(const GameStream&) = delete;
    void* get() { return bytes_; }
    const std::uint8_t* data() const { return bytes_ + kStreamData; }
    std::size_t size() const {
        std::size_t value = 0;
        std::memcpy(&value, bytes_ + kStreamSize, sizeof(value));
        return value;
    }

private:
    alignas(16) std::uint8_t bytes_[0x600]{};
};
static_assert(sizeof(GameStream) >= kStreamBytes, "the game's stream fits");

std::atomic<std::uint32_t> rngIntervalMs{kDefaultRngSyncMs};
SRWLOCK rngLock = SRWLOCK_INIT;
RngSchedule rngSchedule;
RngReceiver rngReceiver;
RngCounters rngStats;
std::uint32_t rngSeq = 0;
std::uint64_t rngSummaryAt = 0;

void RngSummary(std::uint64_t now) {  // under rngLock
    if (!rngSummaryAt) rngSummaryAt = now;
    if (now - rngSummaryAt < 60000) return;
    rngSummaryAt = now;
    if (DetailLog() && (rngStats.sent || rngStats.applied || rngStats.malformed))
        Log("WORLD rng: %llu sent (%llu failed), %llu taken, %llu stale, %llu malformed, %llu not remote, %llu while off",
            static_cast<unsigned long long>(rngStats.sent), static_cast<unsigned long long>(rngStats.sendFailed),
            static_cast<unsigned long long>(rngStats.applied), static_cast<unsigned long long>(rngStats.stale),
            static_cast<unsigned long long>(rngStats.malformed), static_cast<unsigned long long>(rngStats.notRemote),
            static_cast<unsigned long long>(rngStats.inactive));
}

template <typename T>
T Field(std::uint64_t object, std::uint32_t offset) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(object)) + offset, sizeof(value));
    return value;
}

// The owner of `object` (a live GameObjectBase on its choice frame) sends its random state when it is due.
void MaybeSendRng(std::uint64_t object, std::uint16_t netFlags) {
    if (!game) return;
    RngSendView view;
    view.active = NetFeatureActive(NetFeature::WorldAuthority);
    view.online = true;
    view.netFlags = netFlags;
    if (!view.active) return;
    view.team = Field<std::int32_t>(object, kObjectTeam);
    const auto vtable = Field<std::uint64_t>(object, kObjectNetwork);
    const auto imageBase = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(game));
    view.networkVtableRva =
        vtable > imageBase && vtable - imageBase < kImageSize ? static_cast<std::uint32_t>(vtable - imageBase) : 0;
    if (!ShouldSyncRng(view)) return;
    const std::uint64_t now = GetTickCount64();
    RngSync sync;
    AcquireSRWLockExclusive(&rngLock);
    const bool due = rngSchedule.Due(object, now, rngIntervalMs.load());
    if (due) sync.seq = ++rngSeq;
    ReleaseSRWLockExclusive(&rngLock);
    if (!due) return;
    sync.state = Field<std::uint64_t>(object, kObjectRandom);
    sync.state2 = Field<std::uint64_t>(object, kObjectRandom2);
    std::uint8_t bytes[kRngSyncBytes];
    const std::size_t size = WriteRngSync(sync, bytes, sizeof(bytes));
    bool sent = false;
    {
        GameStream stream;
        if (size && Fn<bool(__fastcall*)(void*, std::int8_t)>(kWriteSmall)(stream.get(), kRngSyncTag) &&
            Fn<bool(__fastcall*)(void*, const void*, std::size_t)>(kWriteBlock)(stream.get(), bytes, size)) {
            void* net = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(object)) + kObjectNetwork;
            const auto send =
                *reinterpret_cast<bool(__fastcall* const*)(void*, void*)>(*static_cast<std::uint8_t**>(net) + kSendToCopies);
            sent = send(net, stream.get());
        }
    }
    AcquireSRWLockExclusive(&rngLock);
    ++(sent ? rngStats.sent : rngStats.sendFailed);
    RngSummary(now);
    ReleaseSRWLockExclusive(&rngLock);
}

// 54D79E (see networld.h): r8d = type - 1, rbx the NetworkObject, rdi the stream positioned after the type.
void RngReceiveHandler(CpuContext* context) {
    if (!RngMessageType(context->r8) || !game) return;
    const std::uint64_t now = GetTickCount64();
    if (!NetFeatureActive(NetFeature::WorldAuthority)) {
        // The game returns without reading on; the block is simply left in the message.
        AcquireSRWLockExclusive(&rngLock);
        ++rngStats.inactive;
        ReleaseSRWLockExclusive(&rngLock);
        return;
    }
    RngSync sync;
    bool parsed = false;
    {
        GameStream block;
        parsed = Fn<bool(__fastcall*)(void*, void*)>(kReadBlock)(reinterpret_cast<void*>(context->rdi), block.get()) &&
                 ReadRngSync(block.data(), block.size(), sync);
    }
    const std::uint64_t net = context->rbx;
    const std::uint64_t object = net - kObjectNetwork;
    AcquireSRWLockExclusive(&rngLock);
    if (!parsed) {
        ++rngStats.malformed;
    } else if (!(Field<std::uint16_t>(net, 8) & kRemoteOwned)) {
        ++rngStats.notRemote;  // a machine that thinks it owns the object too: keep our own state
    } else if (!rngReceiver.Accept(object, sync.seq, now)) {
        ++rngStats.stale;
    } else {
        auto* o = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(object));
        std::memcpy(o + kObjectRandom, &sync.state, sizeof(sync.state));
        std::memcpy(o + kObjectRandom2, &sync.state2, sizeof(sync.state2));
        ++rngStats.applied;
    }
    RngSummary(now);
    ReleaseSRWLockExclusive(&rngLock);
}

// Reads everything the decision needs from `object` (GameObjectBase*) and the object its +0x518 weak pointer names.
// False when any of it could not be read: then the game decides as it always did.
bool ReadView(std::uint64_t object, RetargetView& view) {
    return Probing([&]() -> bool {
        __try {
            const auto* o = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(object));
            std::memcpy(&view.netFlags, o + kObjectNetFlags, sizeof(view.netFlags));
            view.ownerSendsTarget = o[kObjectSendsTarget] != 0 && o[kObjectNoTargetSync] == 0;
            std::memcpy(&view.period, o + kObjectRetargetPeriod, sizeof(view.period));
            std::uint64_t target = 0, control = 0;
            std::memcpy(&target, o + kObjectTarget, sizeof(target));
            std::memcpy(&control, o + kObjectTarget + 8, sizeof(control));
            view.hasTarget = false;
            if (target && control) {
                // std::shared_ptr control block: vtable, uses (+8), weaks (+0xC). Uses 0: the target is gone.
                std::uint32_t uses = 0;
                std::memcpy(&uses, reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(control)) + 8, sizeof(uses));
                if (uses != 0) {
                    const auto* t = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(target));
                    std::memcpy(&view.targetState, t + kObjectState, sizeof(view.targetState));
                    std::memcpy(&view.targetHealth, t + kObjectHealth, sizeof(view.targetHealth));
                    view.hasTarget = true;
                }
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
}

// GameObjectBase::Update at 54BF39, `cmp ecx, [rdi+0x514]` (displaced, runs after this) then `jb 54C22C`: ecx is
// the frame counter before this frame's increment, rdi the object. Setting ecx to 0 makes the jb skip this
// frame's choice (and the owner-side message that follows it, which a remote copy never sends anyway).
void RetargetHandler(CpuContext* context) {
    if (!NetFeatureEnabledLocally(NetFeature::WorldAuthority)) return;
    RetargetView view;
    view.online = OnlineSession();
    if (!view.online || !ReadView(context->rdi, view)) return;
    const std::uint32_t counter = static_cast<std::uint32_t>(context->rcx);
    if (counter < view.period) return;  // not a choice frame for the game either
    if (!(view.netFlags & kRemoteOwned)) {
        if (view.netFlags & kLocallyOwned) MaybeSendRng(context->rdi, view.netFlags);
        return;
    }
    if (KeepOwnersTarget(view)) {
        context->rcx = 0;
        kept.fetch_add(1);
        return;
    }
    chosen.fetch_add(1);
    if (DetailLog() && logLines.fetch_add(1) < kLogLimit)
        Log("WORLD target: remote copy %p chooses its own target (%s)", reinterpret_cast<void*>(context->rdi),
            !view.ownerSendsTarget ? "its owner sends no target"
            : !view.hasTarget      ? "it has none"
                                   : "its target is dead or not selectable");
}

}  // namespace

bool KeepOwnersTarget(const RetargetView& view) {
    return view.online && (view.netFlags & kRemoteOwned) != 0 && view.ownerSendsTarget && view.period > 0 &&
           view.hasTarget && (view.targetState & kSearchableStates) == 0 && view.targetHealth > 0.0f;
}

std::vector<MidSite> WorldHooks() {
    // cmp ecx, dword ptr [rdi+0x514] (6 bytes, no rip-relative operand) is displaced; the jb after it stays.
    return {
        {"GameObjectBase::Update target choice", kRetargetSite, {0x3B, 0x8F, 0x14, 0x05, 0x00, 0x00}, 0, 6},
        // mov [rsp+0x30], rsi (rsp-relative, no rip operand) is displaced; the type dispatch after it stays.
        {"GameObjectBase copies' message receive (enemy random state)", kRngReceiveSite, {0x48, 0x89, 0x74, 0x24, 0x30}, 0, 5},
    };
}

MidHandler WorldHookHandler(std::uint32_t rva) {
    switch (rva) {
        case kRetargetSite: return &RetargetHandler;
        case kRngReceiveSite: return &RngReceiveHandler;
        default: return nullptr;
    }
}

std::vector<MidSite> VerifiedWorldHooks(const unsigned char* base) {
    std::vector<MidSite> sites;
    if (!NetFeatureEnabledLocally(NetFeature::WorldAuthority)) {
        Log("Netcode: [Netcode] WorldAuthority=0, remote copies of enemies choose their own targets (the game's way)");
        return sites;
    }
    for (const auto& site : WorldHooks()) {
        const Patch verify{site.name, site.rva, site.original, site.original};
        if (site.rva + site.original.size() > kImageSize || !Matches(base + site.rva, verify)) {
            Log("Netcode: EDF+%X (%s) is not the expected code; enemy target authority stays off, nothing else changes",
                site.rva, site.name);
            continue;
        }
        sites.push_back(site);
    }
    if (!sites.empty())
        Log("Netcode: remote copies of enemies keep the target their owner sends ([Netcode] WorldAuthority=1)");
    return sites;
}

bool RngMessageType(std::uint64_t r8) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(r8)) + 1 == kRngSyncTag;
}

std::uint32_t RngSyncIntervalMs() { return rngIntervalMs.load(); }
void SetRngSyncIntervalMs(std::uint32_t ms) { rngIntervalMs.store(ms); }

RngCounters RngSyncCounters() {
    AcquireSRWLockShared(&rngLock);
    const RngCounters copy = rngStats;
    ReleaseSRWLockShared(&rngLock);
    return copy;
}

std::vector<Patch> InsectPosePatches(int frames) {
    if (frames < 3 || frames >= 90) return {};
    const auto fast = static_cast<std::uint8_t>(frames < 40 ? frames : 40);
    const auto slow = static_cast<std::uint8_t>(frames);
    return {
        {"InsectBase pose interval (+0x2011 clear)", kInsectPoseFast, {0xB8, 0x28, 0x00, 0x00, 0x00}, {0xB8, fast, 0x00, 0x00, 0x00}},
        {"InsectBase pose interval (+0x2011 set)", kInsectPoseSlow, {0xB9, 0x5A, 0x00, 0x00, 0x00}, {0xB9, slow, 0x00, 0x00, 0x00}},
    };
}

void InitWorld(unsigned char* base, const wchar_t* iniPath) {
    game = base;
    const UINT rngMs = GetPrivateProfileIntW(L"Netcode", L"EnemyRngSyncMs", kDefaultRngSyncMs, iniPath);
    rngIntervalMs.store(rngMs < 100 ? 100 : rngMs);
    const int frames = static_cast<int>(GetPrivateProfileIntW(L"Netcode", L"InsectPoseFrames", kDefaultInsectPoseFrames, iniPath));
    const auto patches = InsectPosePatches(frames);
    if (patches.empty()) {
        Log("Netcode: insects send their pose every 40/90 frames when idle (the game's; InsectPoseFrames=%d)", frames);
        return;
    }
    for (const auto& patch : patches) {
        if (patch.rva + patch.original.size() > kImageSize || !Matches(base + patch.rva, patch)) {
            Log("Netcode: EDF+%X (%s) is not the expected code; insect pose intervals stay the game's", patch.rva, patch.name);
            return;
        }
    }
    for (std::size_t i = 0; i < patches.size(); ++i) {
        if (WriteCode(base + patches[i].rva, patches[i].replacement.data(), patches[i].replacement.size())) continue;
        Log("Netcode: could not write EDF+%X (error %lu); insect pose intervals stay the game's", patches[i].rva,
            GetLastError());
        for (std::size_t j = i; j-- > 0;) WriteCode(base + patches[j].rva, patches[j].original.data(), patches[j].original.size());
        return;
    }
    Log("Netcode: insects you own send their pose at least every %d frames (InsectPoseFrames; the game: 40/90)", frames);
}

std::uint64_t KeptTargets() { return kept.load(); }
std::uint64_t LocalChoices() { return chosen.load(); }

}  // namespace multislot
