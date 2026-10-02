#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "rooms.h"

#include <atomic>

#include "crashlog.h"
#include "log.h"
#include "patches.h"

namespace multislot {
namespace {

constexpr std::uint32_t kMemberCountWrapper = 0x12AEB30;
constexpr std::uint32_t kEosMaxLobbyMembers = 64;

struct CopyInfoOptions {
    std::int32_t ApiVersion;
};
// EOS_LobbyDetails_Info, leading fields only. EDF.dll reads LobbyId at +8 and expects ApiVersion 3.
struct LobbyDetailsInfo {
    std::int32_t ApiVersion;
    const char* LobbyId;
    void* LobbyOwnerUserId;
    std::int32_t PermissionLevel;
    std::uint32_t AvailableSlots;
    std::uint32_t MaxMembers;
};

using MemberCountFn = std::uint32_t(*)(void*);

MemberCountFn memberCount = nullptr;
std::atomic<LobbyInfoCopyFn> copyInfo{nullptr};
std::atomic<LobbyInfoReleaseFn> releaseInfo{nullptr};
std::atomic<int> reports{0};

void* HandleOf(void* holder) {
    return Probing([&]() -> void* {
        __try {
            return holder ? **static_cast<void***>(holder) : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    });
}

// The lobby's capacity from EOS, or the vanilla 4 when EOS cannot tell.
std::uint32_t Capacity(void* holder, std::uint32_t members) {
    std::uint32_t capacity = 0;
    void* handle = HandleOf(holder);
    LobbyDetailsInfo* info = nullptr;
    const CopyInfoOptions options{1};
    const LobbyInfoCopyFn copy = copyInfo.load();
    const LobbyInfoReleaseFn release = releaseInfo.load();
    if (handle && copy && release && copy(handle, &options, reinterpret_cast<void**>(&info)) == 0 && info) {
        capacity = CapacityFromInfo(members, info->AvailableSlots, info->MaxMembers);
        if (DetailLog() && reports.fetch_add(1) < 40)
            Log("ROOM members=%u available=%u max=%u -> capacity %u%s", members, info->AvailableSlots, info->MaxMembers,
                capacity ? capacity : kVanillaPlayers, capacity ? "" : " (inconsistent, vanilla fallback)");
        release(info);
    }
    return capacity ? capacity : static_cast<std::uint32_t>(kVanillaPlayers);
}

}  // namespace

std::uint32_t CapacityFromInfo(std::uint32_t members, std::uint32_t availableSlots, std::uint32_t maxMembers) {
    // AvailableSlots is MaxMembers minus the members in the same snapshot; anything else means the
    // struct is not laid out as expected, so trust nothing from it.
    if (maxMembers < 1 || maxMembers > kEosMaxLobbyMembers || availableSlots > maxMembers) return 0;
    if (members + availableSlots != maxMembers) return 0;
    return maxMembers;
}

void InitRooms(const unsigned char* gameBase) {
    memberCount = reinterpret_cast<MemberCountFn>(gameBase + kMemberCountWrapper);
    const HMODULE sdk = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
    copyInfo = sdk ? reinterpret_cast<LobbyInfoCopyFn>(GetProcAddress(sdk, "EOS_LobbyDetails_CopyInfo")) : nullptr;
    releaseInfo = sdk ? reinterpret_cast<LobbyInfoReleaseFn>(GetProcAddress(sdk, "EOS_LobbyDetails_Info_Release")) : nullptr;
    if (!copyInfo.load() || !releaseInfo.load()) Log("ROOM: EOS_LobbyDetails_CopyInfo unavailable; room list keeps the vanilla 4-player view");
}

void RouteLobbyInfo(LobbyInfoCopyFn copy, LobbyInfoReleaseFn release) {
    releaseInfo = release;  // first: a copy made through the new function is always released through its pair
    copyInfo = copy;
}

std::uint64_t RoomCountAndCapacity(void* holder) {
    const std::uint32_t members = memberCount(holder);
    return (static_cast<std::uint64_t>(Capacity(holder, members)) << 32) | members;
}

std::uint32_t RoomFullCount(void* holder) {
    const std::uint32_t members = memberCount(holder);
    return members >= Capacity(holder, members) ? 4u : 0u;
}

}  // namespace multislot
