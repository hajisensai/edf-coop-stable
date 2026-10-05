// What each machine of a game-code test does once EDF.dll and EDF6Coop.dll are in (machine.cpp). Every EOS call
// goes through EDF.dll's import table, as the game makes it, so EDF6Coop's wrappers see it as they do in the game.
#include <cstring>
#include <functional>
#include <vector>

#include "machine.h"
#include "net_shared.h"

namespace gamenet {
namespace {

constexpr const char* kSplitSyncKey = "EDF6MS_SPLITSYNC";  // syncmarker.h
void* const kPlatform = reinterpret_cast<void*>(0x1000);     // what the fake's EOS_Platform_Create hands out
void* const kLobbyInterface = reinterpret_cast<void*>(0x3000);

template <typename T>
T Import(const Machine& machine, const char* name) {
    return reinterpret_cast<T>(GameImport(machine, name));
}
template <typename T>
T FakeExport(const char* name) {
    return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll"), name));
}

// One frame of the game's EOS: EOS_Platform_Tick through EDF.dll's import, where EDF6Coop watches the room.
void Tick(const Machine& machine) { Import<void (*)(void*)>(machine, "EOS_Platform_Tick")(kPlatform); }

// Ticks every ~16 ms until `done` or `timeoutMs`.
bool TickUntil(const Machine& machine, unsigned timeoutMs, const std::function<bool()>& done) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        Tick(machine);
        if (done()) return true;
        if (GetTickCount64() > deadline) return false;
        Sleep(16);
    }
}

const void* Self(const Machine& machine) { return FakeExport<const void* (*)(const char*)>("FakeNet_User")(machine.user.c_str()); }

struct LobbyCallbackInfo {
    std::int32_t ResultCode;
    void* ClientData;
    const char* LobbyId;
};
struct Entered {
    bool done = false;
    std::int32_t result = -1;
    std::string lobby;
};
void OnEntered(const LobbyCallbackInfo* info) {
    auto* entered = static_cast<Entered*>(info->ClientData);
    entered->done = true;
    entered->result = info->ResultCode;
    entered->lobby = info->LobbyId ? info->LobbyId : "";
}

// The leading fields of EOS_Lobby_CreateLobbyOptions / JoinLobbyOptions; the rest stays zero.
struct CreateOptions {
    std::int32_t ApiVersion = 8;
    const void* LocalUserId = nullptr;
    std::uint32_t MaxLobbyMembers = 0;
    std::uint8_t rest[256]{};
};
struct JoinOptions {
    std::int32_t ApiVersion = 5;
    void* LobbyDetailsHandle = nullptr;
    const void* LocalUserId = nullptr;
    std::uint8_t rest[256]{};
};
using LobbyCall = void (*)(void*, const void*, void*, void (*)(const LobbyCallbackInfo*));

// The room's members as EOS lists them, and whether each publishes the split start message marker.
struct Seen {
    std::vector<std::string> members;
    std::vector<bool> marked;
};
Seen LookAtRoom(const Machine& machine, const std::string& lobby) {
    Seen seen;
    struct CopyOptions {
        std::int32_t ApiVersion;
        const char* LobbyId;
        const void* LocalUserId;
    } copy{1, lobby.c_str(), Self(machine)};
    void* details = nullptr;
    if (FakeExport<std::int32_t (*)(void*, const void*, void**)>("EOS_Lobby_CopyLobbyDetailsHandle")(kLobbyInterface, &copy,
                                                                                                    &details))
        return seen;
    const std::uint32_t count = FakeExport<std::uint32_t (*)(void*, const void*)>("EOS_LobbyDetails_GetMemberCount")(details, nullptr);
    for (std::uint32_t i = 0; i < count; ++i) {
        struct ByIndex {
            std::int32_t ApiVersion;
            std::uint32_t MemberIndex;
        } byIndex{1, i};
        const void* member = FakeExport<const void* (*)(void*, const void*)>("EOS_LobbyDetails_GetMemberByIndex")(details, &byIndex);
        char text[64]{};
        std::int32_t length = sizeof(text);
        FakeExport<std::int32_t (*)(const void*, char*, std::int32_t*)>("EOS_ProductUserId_ToString")(member, text, &length);
        struct ByKey {
            std::int32_t ApiVersion;
            const void* TargetUserId;
            const char* AttrKey;
        } byKey{1, member, kSplitSyncKey};
        void* attribute = nullptr;
        const bool marked = FakeExport<std::int32_t (*)(void*, const void*, void**)>(
                                "EOS_LobbyDetails_CopyMemberAttributeByKey")(details, &byKey, &attribute) == 0;
        if (attribute) FakeExport<void (*)(void*)>("EOS_Lobby_Attribute_Release")(attribute);
        seen.members.push_back(text);
        seen.marked.push_back(marked);
    }
    FakeExport<void (*)(void*)>("EOS_LobbyDetails_Release")(details);
    return seen;
}

bool AllMarked(const Seen& seen, std::size_t members) {
    if (seen.members.size() != members) return false;
    for (bool marked : seen.marked)
        if (!marked) return false;
    return true;
}

int expectedMembers() {
    char text[16]{};
    GetEnvironmentVariableA("EDF6NET_MEMBERS", text, sizeof(text));
    return text[0] ? std::atoi(text) : 2;
}

// Host: creates the room the way the game does and waits until everyone is in and marked.
int HostRoom(Machine& machine, std::string& lobby) {
    Entered entered;
    CreateOptions options;
    options.LocalUserId = Self(machine);
    options.MaxLobbyMembers = 8;
    Import<LobbyCall>(machine, "EOS_Lobby_CreateLobby")(kLobbyInterface, &options, &entered, &OnEntered);
    if (!TickUntil(machine, 5000, [&] { return entered.done; }) || entered.result != 0) {
        Result("room", "create failed (%d)", entered.result);
        return 1;
    }
    lobby = entered.lobby;
    Result("lobby", "%s", lobby.c_str());
    const std::size_t members = static_cast<std::size_t>(expectedMembers());
    const bool everyone = TickUntil(machine, 20000, [&] { return AllMarked(LookAtRoom(machine, lobby), members); });
    const Seen seen = LookAtRoom(machine, lobby);
    for (std::size_t i = 0; i < seen.members.size(); ++i)
        Result("member", "%s %s", seen.members[i].c_str(), seen.marked[i] ? "marked" : "unmarked");
    return everyone ? 0 : 1;
}

// Guest: waits for the room, joins it the way the game does, and waits until everyone is marked.
int GuestRoom(Machine& machine, std::string& lobby) {
    void* details = nullptr;
    const auto roomDetails = FakeExport<void* (*)()>("FakeNet_RoomDetails");
    if (!TickUntil(machine, 10000, [&] { return (details = roomDetails()) != nullptr; })) {
        Result("room", "no room to join");
        return 1;
    }
    Entered entered;
    JoinOptions options;
    options.LobbyDetailsHandle = details;
    options.LocalUserId = Self(machine);
    Import<LobbyCall>(machine, "EOS_Lobby_JoinLobby")(kLobbyInterface, &options, &entered, &OnEntered);
    if (!TickUntil(machine, 5000, [&] { return entered.done; }) || entered.result != 0) {
        Result("room", "join failed (%d)", entered.result);
        return 1;
    }
    FakeExport<void (*)(void*)>("EOS_LobbyDetails_Release")(details);
    lobby = entered.lobby;
    Result("lobby", "%s", lobby.c_str());
    const std::size_t members = static_cast<std::size_t>(expectedMembers());
    const bool everyone = TickUntil(machine, 20000, [&] { return AllMarked(LookAtRoom(machine, lobby), members); });
    const Seen seen = LookAtRoom(machine, lobby);
    for (std::size_t i = 0; i < seen.members.size(); ++i)
        Result("member", "%s %s", seen.members[i].c_str(), seen.marked[i] ? "marked" : "unmarked");
    return everyone ? 0 : 1;
}

}  // namespace

int RunRole(Machine& machine, const std::string& role) {
    std::string lobby;
    if (role == "host-room") return HostRoom(machine, lobby);
    if (role == "guest-room") return GuestRoom(machine, lobby);
    Result("role", "unknown role %s", role.c_str());
    return 2;
}

}  // namespace gamenet
