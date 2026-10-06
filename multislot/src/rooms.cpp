#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "rooms.h"

#include <atomic>
#include <cstring>

#include "crashlog.h"
#include "log.h"
#include "patches.h"

namespace multislot {
namespace {

constexpr std::uint32_t kMemberCountWrapper = 0x12AEB30;
constexpr std::uint32_t kEosMaxLobbyMembers = static_cast<std::uint32_t>(kEosLobbyMembers);

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

// EOS_LobbyDetails_GetAttributeCount / CopyAttributeByIndex / EOS_Lobby_Attribute_Release and their structs.
struct AttributeCountOptions {
    std::int32_t ApiVersion;
};
struct CopyAttributeByIndexOptions {
    std::int32_t ApiVersion;
    std::uint32_t AttrIndex;
};
struct AttributeData {
    std::int32_t ApiVersion;
    const char* Key;
    union {
        std::int64_t AsInt64;
        double AsDouble;
        std::int32_t AsBool;
        const char* AsUtf8;
    } Value;
    std::int32_t ValueType;  // 1 int64
};
struct LobbyAttribute {
    std::int32_t ApiVersion;
    AttributeData* Data;
    std::int32_t Visibility;
};
using AttributeCountFn = std::uint32_t (*)(void* details, const AttributeCountOptions*);
using CopyAttributeFn = std::int32_t (*)(void* details, const CopyAttributeByIndexOptions*, LobbyAttribute** out);
using ReleaseAttributeFn = void (*)(LobbyAttribute* attribute);
const unsigned char* gameImage = nullptr;

// What the game's own import of `function` points at now: EOS, or whatever wrapped it (the direct-link part answers
// for room list entries it made itself, EOS knows nothing of those).
void* GameImport(const char* function) {
    if (!gameImage) return nullptr;
    return Probing([&]() -> void* {
        __try {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameImage);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(gameImage + dos->e_lfanew);
            const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!directory.VirtualAddress) return nullptr;
            for (auto entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(gameImage + directory.VirtualAddress);
                 entry->Name; ++entry) {
                if (_stricmp(reinterpret_cast<const char*>(gameImage + entry->Name), "EOSSDK-Win64-Shipping.dll") != 0)
                    continue;
                auto names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                    gameImage + (entry->OriginalFirstThunk ? entry->OriginalFirstThunk : entry->FirstThunk));
                auto slots = reinterpret_cast<const IMAGE_THUNK_DATA64*>(gameImage + entry->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++slots) {
                    if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                    const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(gameImage + names->u1.AddressOfData);
                    if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
                        return reinterpret_cast<void*>(slots->u1.Function);
                }
            }
            return nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    });
}

// The room size the lobby publishes (kRoomSizeKey), 0 when it publishes none or it cannot be read.
std::uint32_t PublishedRoomSize(void* handle) {
    const auto count = reinterpret_cast<AttributeCountFn>(GameImport("EOS_LobbyDetails_GetAttributeCount"));
    const auto copy = reinterpret_cast<CopyAttributeFn>(GameImport("EOS_LobbyDetails_CopyAttributeByIndex"));
    const auto release = reinterpret_cast<ReleaseAttributeFn>(GameImport("EOS_Lobby_Attribute_Release"));
    if (!handle || !count || !copy || !release) return 0;
    const AttributeCountOptions countOptions{1};
    const std::uint32_t attributes = count(handle, &countOptions);
    std::uint32_t size = 0;
    for (std::uint32_t i = 0; i < attributes && i < 64 && !size; ++i) {
        const CopyAttributeByIndexOptions options{1, i};
        LobbyAttribute* attribute = nullptr;
        if (copy(handle, &options, &attribute) != 0 || !attribute) continue;
        const AttributeData* data = attribute->Data;
        if (data && data->Key && std::strcmp(data->Key, kRoomSizeKey) == 0 && data->ValueType == 1 &&
            data->Value.AsInt64 > 0 && data->Value.AsInt64 <= kMaxPlayers)
            size = static_cast<std::uint32_t>(data->Value.AsInt64);
        release(attribute);
    }
    return size;
}

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
        const std::uint32_t maxMembers = info->MaxMembers, available = info->AvailableSlots;
        release(info);
        // Only a full-sized EOS lobby can stand for a larger room.
        const std::uint32_t roomSize = maxMembers == kEosMaxLobbyMembers ? PublishedRoomSize(handle) : 0;
        capacity = CapacityFromInfo(members, available, maxMembers, roomSize);
        if (DetailLog() && reports.fetch_add(1) < 40)
            Log("ROOM members=%u available=%u max=%u room size %u -> capacity %u%s", members, available, maxMembers,
                roomSize, capacity ? capacity : kVanillaPlayers, capacity ? "" : " (inconsistent, vanilla fallback)");
    }
    return capacity ? capacity : static_cast<std::uint32_t>(kVanillaPlayers);
}

}  // namespace

std::uint32_t CapacityFromInfo(std::uint32_t members, std::uint32_t availableSlots, std::uint32_t maxMembers,
                               std::uint32_t roomSize) {
    // AvailableSlots is MaxMembers minus the members in the same snapshot; anything else means the
    // struct is not laid out as expected, so trust nothing from it.
    if (maxMembers < 1 || maxMembers > kEosMaxLobbyMembers || availableSlots > maxMembers) return 0;
    if (members + availableSlots != maxMembers) return 0;
    // A room larger than an EOS lobby: its lobby is full-sized, and the size is the room's own.
    if (maxMembers == kEosMaxLobbyMembers && roomSize > maxMembers && roomSize <= static_cast<std::uint32_t>(kMaxPlayers))
        return roomSize;
    return maxMembers;
}

void InitRooms(const unsigned char* gameBase) {
    gameImage = gameBase;
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
