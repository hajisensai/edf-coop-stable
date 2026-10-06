#include "networld.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstring>

#include "crashlog.h"
#include "log.h"
#include "mission.h"
#include "netfeature.h"

namespace multislot {
namespace {

constexpr std::uint16_t kRemoteOwned = 1;  // NetworkObject flag bit0 (782880)
constexpr std::uint32_t kSearchableStates = ~std::uint32_t{2};  // 54A3BA: `test [obj+0x39C], 0xFFFFFFFD`
constexpr int kLogLimit = 40;

std::atomic<std::uint64_t> kept{0}, chosen{0};
std::atomic<int> logLines{0};

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
    if (!(view.netFlags & kRemoteOwned)) return;
    const std::uint32_t counter = static_cast<std::uint32_t>(context->rcx);
    if (counter < view.period) return;  // not a choice frame for the game either
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
    };
}

MidHandler WorldHookHandler(std::uint32_t rva) {
    switch (rva) {
        case kRetargetSite: return &RetargetHandler;
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

std::uint64_t KeptTargets() { return kept.load(); }
std::uint64_t LocalChoices() { return chosen.load(); }

}  // namespace multislot
