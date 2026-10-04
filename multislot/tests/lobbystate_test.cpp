// The lobby the game is in (lobbystate.h), against a stand-in EOS SDK (fake_eos.cpp): what a room update
// publishes is read from the lobby, the label follows it, and the game's lobby calls never leave this machine
// behind as a member of a lobby it no longer plays in.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../src/hostmode.h"
#include "../src/lobbystate.h"
#include "../src/log.h"
#include "../src/packetfit.h"

using namespace multislot;

namespace {

int failures = 0;
// A room size the host picked at run time (F2): capacity follows the lobby, not the build.
constexpr int kRoomSize = 12;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

HMODULE fake = nullptr;
template <typename T>
T Fake(const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(fake, name)));
}

// --- the game's side of the import table ---
std::vector<std::string> redirected;
std::map<std::string, void*> replacements;
bool FakeRedirect(HMODULE, const char* dll, const char* function, void* replacement, void** original) {
    redirected.push_back(std::string(dll) + "!" + function);
    replacements[function] = replacement;
    *original = reinterpret_cast<void*>(GetProcAddress(fake, function));
    return *original != nullptr;
}

struct LobbyIdCallbackInfo {
    std::int32_t ResultCode;
    void* ClientData;
    const char* LobbyId;
};
using LobbyIdCallback = void (*)(const LobbyIdCallbackInfo*);
using LobbyCallFn = void (*)(void*, const void*, void*, LobbyIdCallback);
using TickFn = void (*)(void*);
struct CreateOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    std::uint32_t MaxLobbyMembers;
};
struct JoinOptions {  // EOS_Lobby_JoinLobbyOptions, version 4, as EDF.dll fills it
    std::int32_t ApiVersion;
    void* LobbyDetailsHandle;
    const void* LocalUserId;
    std::int32_t bPresenceEnabled;
    const void* LocalRTCOptions;
    std::int32_t bCrossplayOptOut;
};
struct LeaveOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const char* LobbyId;
};
struct UpdateModificationOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const char* LobbyId;
};
struct UpdateOptions {
    std::int32_t ApiVersion;
    void* LobbyModificationHandle;
};

int gameMarker = 0;  // the game's ClientData
std::vector<std::int32_t> gameResults;
bool gameClientDataOk = true;
void GameCallback(const LobbyIdCallbackInfo* info) {
    gameResults.push_back(info->ResultCode);
    gameClientDataOk = gameClientDataOk && info->ClientData == &gameMarker;
}

int lobbyHandle = 0;
void* Lobby() { return &lobbyHandle; }
void Call(const char* name, const void* options) {
    reinterpret_cast<LobbyCallFn>(replacements[name])(Lobby(), options, &gameMarker, &GameCallback);
}
void Tick() { reinterpret_cast<TickFn>(replacements["EOS_Platform_Tick"])(nullptr); }
void NextBeat() {
    Sleep(1050);
    Tick();
}

const void* User(const char* id) { return Fake<const void* (*)(const char*)>("FakeEos_User")(id); }
int Count(const char* what) { return Fake<int (*)()>(what)(); }
bool Member() { return Fake<int (*)(const char*)>("FakeEos_IsMember")("self") != 0; }
void EnterLobby(const char* id, const char* owner) { Fake<void (*)(const char*, const char*)>("FakeEos_EnterLobby")(id, owner); }
void SetMaxMembers(std::uint32_t count) { Fake<void (*)(std::uint32_t)>("FakeEos_SetMaxMembers")(count); }
void SetSearchType(std::int64_t value) {
    Fake<void (*)(const char*, std::int64_t)>("FakeEos_SetLobbyAttribute")("SEARCH_TYPE", value);
}

// The room object the room update runs on (r13), as hostmode_test builds it.
struct FakeRoom {
    alignas(8) unsigned char bytes[0x90]{};
    FakeRoom(void* lobby, const void* user, const char* id) {
        const std::uint64_t size = std::strlen(id), capacity = 15;
        std::memcpy(bytes + 0x20, &lobby, 8);
        std::memcpy(bytes + 0x60, &user, 8);
        std::memcpy(bytes + 0x68, id, size);
        std::memcpy(bytes + 0x78, &size, 8);
        std::memcpy(bytes + 0x80, &capacity, 8);
    }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes)); }
};
// What the room update (749C91, 749CBF) publishes for a room of kind 0x93.
std::pair<std::uint64_t, std::uint64_t> Published(const FakeRoom& room) {
    CpuContext context{};
    context.r13 = room.Address();
    context.rdx = 0xDEAD;
    context.rbx = 0xDEAD;
    HostModeHookHandler(0x749C91)(&context);
    HostModeHookHandler(0x749CBF)(&context);
    return {context.rdx, context.rbx};
}
const std::uint64_t kMirrored = 2 * kSearchTypeCenter - 0x93;

std::string ReadLog(const std::wstring& path) {
    LogFlush();
    std::string text;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
        std::fclose(file);
    }
    return text;
}
bool Logged(const std::wstring& path, const char* needle) { return ReadLog(path).find(needle) != std::string::npos; }

void TestKinds() {
    Check(KindOf({}) == LobbyKind::Unknown, "a lobby nothing is known of has no kind");
    Check(KindOf({4, false, 0}) == LobbyKind::Normal, "four members, no SEARCH_TYPE yet: normal");
    Check(KindOf({static_cast<std::uint32_t>(kRoomSize), false, 0}) == LobbyKind::MultiSlot,
          "more than four members: MultiSlot");
    Check(KindOf({4, true, static_cast<std::int64_t>(kMirrored)}) == LobbyKind::MultiSlot,
          "this build's SEARCH_TYPE decides over a MaxMembers EOS refused to raise");
    Check(KindOf({static_cast<std::uint32_t>(kRoomSize), true, 0x93}) == LobbyKind::Normal,
          "a vanilla SEARCH_TYPE is a normal room");
    Check(CapacityToKeep({10, false, 0}) == (kMaxPlayers >= 10 ? 10 : kMaxPlayers) &&
              CapacityToKeep({4, true, static_cast<std::int64_t>(kMirrored)}) == kMaxPlayers,
          "a MultiSlot lobby keeps its own size, at most this build's");
}

void TestLobby(const wchar_t* fakePath, const std::wstring& log) {
    Check(!InstallLobbyState(nullptr, &FakeRedirect) && redirected.empty(), "without the EOS SDK nothing is redirected");
    fake = LoadLibraryW(fakePath);
    Check(fake != nullptr, "the stand-in EOS SDK loads");
    if (!fake) return;
    Fake<void (*)(const char*)>("FakeEos_Reset")("self");
    Check(InstallLobbyState(nullptr, &FakeRedirect), "lobby state installs");
    const std::string sdk = "EOSSDK-Win64-Shipping.dll!";
    Check(redirected == std::vector<std::string>({sdk + "EOS_Lobby_LeaveLobby", sdk + "EOS_Lobby_DestroyLobby",
                                                 sdk + "EOS_Lobby_UpdateLobby", sdk + "EOS_Platform_Tick",
                                                 sdk + "EOS_Lobby_CreateLobby", sdk + "EOS_Lobby_JoinLobby",
                                                 sdk + "EOS_LobbySearch_Find", sdk + "EOS_LobbySearch_GetSearchResultCount",
                                                 sdk + "EOS_LobbySearch_CopySearchResultByIndex"}),
          "leave, destroy, update, tick, create, join and the room list are redirected, in that order");
    InitHostMode(nullptr, nullptr, false, VK_F2, 0, L"F2");

    // Hosting a MultiSlot room: the creation decides until the lobby is read, then the lobby does.
    const CreateOptions create{1, User("self"), static_cast<std::uint32_t>(kRoomSize)};
    Call("EOS_Lobby_CreateLobby", &create);
    Tick();
    Check(gameResults.size() == 2 && gameResults[1] == 0 && gameClientDataOk, "the game gets both runs of its completion");
    Check(CreatedCurrentLobby() && CurrentLobbyKind() == LobbyKind::MultiSlot, "the lobby created is known as MultiSlot");
    NextBeat();
    Check(Logged(log, "LOBBY lobby-created owner: EOS self (this machine)"), "the owner is logged");
    const FakeRoom hosted(Lobby(), User("self"), "lobby-created");
    Check(Published(hosted) == std::make_pair(static_cast<std::uint64_t>(kRoomSize), kMirrored),
          "the creator's update keeps the MultiSlot room with the setting OFF since");
    Check(Logged(log, "ROOM UPDATE of lobby lobby-created keeps it a MultiSlot room"), "and says why");

    // 2026-10-01: a member that EOS made the owner updated a 12-player room as a normal one. Joined, setting OFF.
    gameResults.clear();
    EnterLobby("lobby-12p", "host");
    SetMaxMembers(static_cast<std::uint32_t>(kRoomSize));
    SetSearchType(static_cast<std::int64_t>(kMirrored));
    NoteLobbyEntered(Lobby(), User("self"), "lobby-12p", 0);
    Check(!CreatedCurrentLobby() && CurrentLobbyKind() == LobbyKind::Unknown, "a joined lobby is unknown until read");
    NextBeat();
    Check(CurrentLobbyKind() == LobbyKind::MultiSlot, "the beat reads its kind");
    Check(Logged(log, "LOBBY lobby-12p owner: EOS host"), "and its owner");
    const FakeRoom joined(Lobby(), User("self"), "lobby-12p");
    Fake<void (*)(const char*)>("FakeEos_SetOwner")("self");
    NextBeat();
    Check(Logged(log, "LOBBY lobby-12p owner changed: EOS host -> EOS self (this machine is the owner now)"),
          "the owner change is logged");
    Check(Published(joined) == std::make_pair(static_cast<std::uint64_t>(kRoomSize), kMirrored),
          "a member that became the owner keeps the room MultiSlot, whatever its own setting");
    // A normal room stays normal for a member set to ON.
    InitHostMode(nullptr, nullptr, true, VK_F2, 0, L"F2");
    SetMaxMembers(4);
    SetSearchType(0x93);
    Check(Published(joined) == std::make_pair(std::uint64_t{4}, std::uint64_t{0x93}),
          "a normal room stays a normal room with the setting ON");
    // EOS has no copy and this machine did not create it: the game's values, as without the plugin.
    Fake<void (*)(int)>("FakeEos_SetCopyFails")(1);
    Check(Published(joined) == std::make_pair(std::uint64_t{4}, std::uint64_t{0x93}), "an unreadable joined room is left as the game has it");
    NextBeat();
    Check(Logged(log, "LOBBY lobby-12p: EOS has no copy of this lobby for this machine any more"), "a lost copy is logged");
    // ... but one this machine created keeps what it was created as.
    NoteLobbyEntered(Lobby(), User("self"), "lobby-12p", static_cast<std::uint32_t>(kRoomSize));
    InitHostMode(nullptr, nullptr, false, VK_F2, 0, L"F2");
    Check(Published(joined) == std::make_pair(static_cast<std::uint64_t>(kRoomSize), kMirrored),
          "an unreadable lobby this machine created keeps its creation");
    Check(Published(FakeRoom(Lobby(), User("self"), "lobby-x")) == std::make_pair(std::uint64_t{4}, std::uint64_t{0x93}),
          "another lobby id is not taken for the one created here");
    Fake<void (*)(int)>("FakeEos_SetCopyFails")(0);

    // The game closes a room it believes it hosts alone, but EOS gave the lobby to someone else: leave instead.
    EnterLobby("lobby-theirs", "host");
    NoteLobbyEntered(Lobby(), User("self"), "lobby-theirs", 0);
    gameResults.clear();
    int leaves = Count("FakeEos_Leaves"), destroys = Count("FakeEos_Destroys");
    const LeaveOptions theirs{1, User("self"), "lobby-theirs"};
    Call("EOS_Lobby_DestroyLobby", &theirs);
    Check(Count("FakeEos_Leaves") == leaves + 1 && Count("FakeEos_Destroys") == destroys && !Member(),
          "a destroy of a lobby someone else owns becomes a leave");
    Tick();
    Check(gameResults == std::vector<std::int32_t>({0}) && gameClientDataOk, "the game gets its completion");
    Check(CurrentLobbyKind() == LobbyKind::Unknown && !CreatedCurrentLobby(), "the lobby is no longer current");
    Check(Logged(log, "LOBBY DestroyLobby lobby-theirs requested by the game, but EOS host owns it") &&
              Logged(log, "LOBBY LeaveLobby lobby-theirs in place of the game's DestroyLobby: result 0"),
          "and the log says so, with the result");

    // A destroy that fails leaves the lobby too.
    EnterLobby("lobby-mine", "self");
    Fake<void (*)(std::int32_t)>("FakeEos_SetDestroyResult")(22);
    gameResults.clear();
    leaves = Count("FakeEos_Leaves");
    const LeaveOptions mine{1, User("self"), "lobby-mine"};
    Call("EOS_Lobby_DestroyLobby", &mine);
    Check(Member() && Count("FakeEos_Leaves") == leaves, "an owner's destroy goes to EOS");
    Tick();
    Check(gameResults == std::vector<std::int32_t>({22}), "the game gets the failure");
    Check(!Member() && Count("FakeEos_Leaves") == leaves + 1, "and this machine leaves the lobby it could not close");
    Tick();
    Check(Logged(log, "LOBBY DestroyLobby lobby-mine: result 22") &&
              Logged(log, "LOBBY LeaveLobby lobby-mine (after a failed DestroyLobby): result 0"),
          "both results are logged");
    Fake<void (*)(std::int32_t)>("FakeEos_SetDestroyResult")(0);

    // Joining a lobby EOS still lists this machine in: leave it, then join it.
    EnterLobby("lobby-ghost", "host");
    gameResults.clear();
    leaves = Count("FakeEos_Leaves");
    int joins = Count("FakeEos_Joins");
    void* ghost = Fake<void* (*)(const char*)>("FakeEos_Details")("lobby-ghost");
    const JoinOptions join{4, ghost, User("self"), 1, nullptr, 0};
    Call("EOS_Lobby_JoinLobby", &join);
    Check(Count("FakeEos_Leaves") == leaves + 1 && Count("FakeEos_Joins") == joins, "the stale membership is left first");
    Tick();  // the leave completes; the join goes out
    Check(Count("FakeEos_Joins") == joins + 1 && gameResults.empty(), "then the game's join goes out");
    Tick();
    Check(gameResults == std::vector<std::int32_t>({0}) && gameClientDataOk && Member(), "and gets in");
    Check(Logged(log, "LOBBY JoinLobby lobby-ghost: EOS still lists this machine as a member of it") &&
              Logged(log, "LOBBY LeaveLobby lobby-ghost before joining it again: result 0") &&
              Logged(log, "LOBBY lobby-ghost joined"),
          "every step is logged");

    // A join EOS refuses as "already a member" (9002) leaves, so the next join gets in; no join of our own.
    gameResults.clear();
    leaves = Count("FakeEos_Leaves");
    joins = Count("FakeEos_Joins");
    const JoinOptions older{3, ghost, User("self"), 1, nullptr, 0};  // a layout this module does not copy
    Call("EOS_Lobby_JoinLobby", &older);
    Tick();
    Check(gameResults == std::vector<std::int32_t>({9002}) && Count("FakeEos_Joins") == joins + 1,
          "the game gets the refusal, and nothing joins again by itself");
    Check(Count("FakeEos_Leaves") == leaves + 1 && !Member(), "the stale membership is left");
    Tick();
    Check(Logged(log, "LOBBY JoinLobby lobby-ghost: EOS says this machine is still a member of it (9002") &&
              Logged(log, "LOBBY LeaveLobby lobby-ghost (a stale membership that refused a join): result 0"),
          "the refusal and the leave are logged");
    Fake<void (*)(void*)>("EOS_LobbyDetails_Release")(ghost);

    // Updates and leaves report their results.
    EnterLobby("lobby-last", "self");
    NoteLobbyEntered(Lobby(), User("self"), "lobby-last", static_cast<std::uint32_t>(kRoomSize));
    using ModificationFn = std::int32_t (*)(void*, const UpdateModificationOptions*, void**);
    void* modification = nullptr;
    const UpdateModificationOptions modify{1, User("self"), "lobby-last"};
    Fake<ModificationFn>("EOS_Lobby_UpdateLobbyModification")(Lobby(), &modify, &modification);
    Fake<void (*)(std::int32_t)>("FakeEos_SetUpdateResult")(22);
    gameResults.clear();
    const UpdateOptions update{1, modification};
    Call("EOS_Lobby_UpdateLobby", &update);
    Fake<void (*)(void*)>("EOS_LobbyModification_Release")(modification);
    Tick();
    Check(gameResults == std::vector<std::int32_t>({22}), "the game gets the update's result");
    Check(Logged(log, "LOBBY UpdateLobby lobby-last: result 22"), "an update's result is logged");
    gameResults.clear();
    const LeaveOptions last{1, User("self"), "lobby-last"};
    Call("EOS_Lobby_LeaveLobby", &last);
    Tick();
    Check(gameResults == std::vector<std::int32_t>({0}) && !Member() && CurrentLobbyKind() == LobbyKind::Unknown,
          "the game's leave goes through");
    Check(Logged(log, "LOBBY LeaveLobby lobby-last requested by the game") && Logged(log, "LOBBY LeaveLobby lobby-last: result 0"),
          "a leave and its result are logged");
    reinterpret_cast<LobbyCallFn>(replacements["EOS_Lobby_LeaveLobby"])(Lobby(), nullptr, nullptr, nullptr);
    Tick();
    Check(Logged(log, "LOBBY LeaveLobby ? requested by the game"), "a leave without options is passed on");
}

// What the game's import of `name` points at now.
template <typename T>
T Replaced(const char* name) {
    return reinterpret_cast<T>(replacements[name]);
}

// The room list (the user's request, 2026-10-04): normal and MultiSlot rooms are both shown, an earlier MultiSlot
// version's are left out, and the game's indexes name the rooms it is shown. A search handle is any address.
void TestRoomList(const std::wstring& log) {
    if (!fake) return;
    struct ResultOptions {
        std::int32_t ApiVersion;
        std::uint32_t LobbyIndex;
    };
    using FindFn = void (*)(int*, const void*, void*, void*);
    using CountFn = std::uint32_t (*)(int*, const void*);
    using CopyFn = std::int32_t (*)(int*, const ResultOptions*, void**);
    const auto find = Replaced<FindFn>("EOS_LobbySearch_Find");
    const auto count = Replaced<CountFn>("EOS_LobbySearch_GetSearchResultCount");
    const auto copy = Replaced<CopyFn>("EOS_LobbySearch_CopySearchResultByIndex");
    const auto results = [](const auto& types) {
        Fake<void (*)(const std::int64_t*, int)>("FakeEos_SetSearchResults")(types.data(), static_cast<int>(types.size()));
    };
    const auto room = [&copy](int* search, std::uint32_t index) {
        const ResultOptions options{1, index};
        void* details = nullptr;
        if (copy(search, &options, &details) != 0 || !details) return std::string("<none>");
        std::string id = Fake<const char* (*)(void*)>("FakeEos_DetailsId")(details);
        Fake<void (*)(void*)>("EOS_LobbyDetails_Release")(details);
        return id;
    };
    int search = 0;
    int other = 0;
    // 0x8C and 0x7D: earlier MultiSlot versions' rooms; -1: a room that says no SEARCH_TYPE.
    const std::array<std::int64_t, 6> found{0x93, 0x8C, static_cast<std::int64_t>(kMirrored), 0x7D, -1, 0x91};
    results(found);
    find(&search, nullptr, nullptr, nullptr);
    Check(Fake<int (*)()>("FakeEos_Finds")() == 1, "the game's search goes to EOS");
    Check(count(&search, nullptr) == 4, "an earlier MultiSlot version's rooms are left out of the count");
    Check(room(&search, 0) == "room0" && room(&search, 1) == "room2" && room(&search, 2) == "room4" &&
              room(&search, 3) == "room5",
          "the game's indexes name the rooms it is shown, normal and MultiSlot, in EOS's order");
    Check(room(&search, 4) == "<none>", "past the last room shown there is none");
    Check(count(&search, nullptr) == 4, "asked again (EDF6DirectNet asks for every result it copies), the same");
    Check(Logged(log, "LOBBY room list: 2 of 6 room(s) left out"), "the log says how many were left out");
    Check(room(&other, 1) == "room1", "a search the game has not counted is passed through as it is");
    // A new search on the same handle is worked out again, and so are results that come in after the first count.
    results(std::array<std::int64_t, 2>{0x8F, 0x92});
    find(&search, nullptr, nullptr, nullptr);
    Check(count(&search, nullptr) == 1 && room(&search, 0) == "room1", "a search run again is worked out again");
    results(std::array<std::int64_t, 0>{});
    find(&search, nullptr, nullptr, nullptr);
    Check(count(&search, nullptr) == 0, "no results yet");
    results(found);
    Check(count(&search, nullptr) == 4 && room(&search, 3) == "room5", "results that come in later are counted");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: LobbyStateTests <log> <fake EOSSDK-Win64-Shipping.dll>\n");
        return 2;
    }
    std::wstring logPath, fakePath;
    for (const char* c = argv[1]; *c; ++c) logPath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    for (const char* c = argv[2]; *c; ++c) fakePath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    DeleteFileW(logPath.c_str());
    LogOpen(logPath.c_str());
    TestKinds();
    TestLobby(fakePath.c_str(), logPath);
    TestRoomList(logPath);
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
