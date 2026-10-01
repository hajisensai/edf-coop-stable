// Who reads a split mission start message (syncmarker.h), against a stand-in EOS SDK (fake_eos.cpp): the game's
// lobby calls go through the plugin's redirects, completions run on the EOS tick, and the lobby keeps member
// attributes the way EOS does.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/log.h"
#include "../src/packetfit.h"
#include "../src/syncmarker.h"

using namespace multislot;

namespace {

int failures = 0;

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
struct LobbyOptions {  // CreateLobby: ApiVersion, LocalUserId; JoinLobby: ApiVersion, details, LocalUserId
    std::int32_t ApiVersion;
    const void* first;
    const void* second;
};
using LobbyCallFn = void (*)(void*, const LobbyOptions*, void*, LobbyIdCallback);
using LeaveFn = void (*)(void*, const void*, void*, void*);
using TickFn = void (*)(void*);

int gameMarker = 0;  // the game's ClientData
std::vector<std::int32_t> gameResults;
std::string gameLobby;
bool gameClientDataOk = true;
void GameLobbyCallback(const LobbyIdCallbackInfo* info) {
    gameResults.push_back(info->ResultCode);
    gameLobby = info->LobbyId ? info->LobbyId : "";
    gameClientDataOk = gameClientDataOk && info->ClientData == &gameMarker;
}

void Tick() { reinterpret_cast<TickFn>(replacements["EOS_Platform_Tick"])(nullptr); }
void NextObservation() {
    Sleep(1050);
    Tick();
}

const void* User(const char* id) { return Fake<const void* (*)(const char*)>("FakeEos_User")(id); }
std::int64_t Published(const char* key) { return Fake<std::int64_t (*)(const char*, const char*)>("FakeEos_Attribute")("self", key); }
int Updates() { return Fake<int (*)()>("FakeEos_Updates")(); }
void AddMember(const char* id, bool marked) {
    Fake<void (*)(const char*)>("FakeEos_AddMember")(id);
    if (marked) Fake<void (*)(const char*, const char*, std::int64_t)>("FakeEos_SetAttribute")(id, kSplitSyncKey, 1);
}

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

std::size_t Count(const std::string& text, const char* needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

void TestRoomWithoutEos() {
    SplitSyncRoom room;
    Check(!room.InLobby() && !room.Observe({{"a", true}}) && !room.Marked("a"), "outside a lobby nothing is learnt");
    room.Entered("L1");
    Check(room.Observe({{"a", true}, {"b", false}}), "the first members are newcomers");
    Check(!room.Observe({{"a", false}, {"b", false}}), "the same members again are not");
    Check(room.Marked("a") && !room.Marked("b"), "a marker once seen stays while we are in the room");
    Check(!room.Observe({{"b", true}}) && room.Marked("b"), "a marker seen later counts");
    Check(room.Observe({{"a", false}, {"b", false}, {"c", false}}) && room.Unmarked() == std::vector<std::string>({"c"}),
          "a newcomer is reported and listed as unmarked");
    room.Entered("L2");
    Check(!room.Marked("a") && room.Unmarked().empty() && room.LobbyId() == "L2", "another lobby starts from nothing");
    room.Left();
    Check(!room.InLobby() && !room.Observe({{"a", true}}) && !room.Marked("a"), "after leaving nothing is learnt");
}

void TestLobby(const wchar_t* fakePath) {
    Check(!InstallSyncMarker(nullptr, &FakeRedirect) && redirected.empty(),
          "without the EOS SDK loaded nothing is redirected");
    Check(!PeerReadsSplitSync(nullptr), "a null peer reads nothing");
    fake = LoadLibraryW(fakePath);
    Check(fake != nullptr, "the stand-in EOS SDK loads");
    if (!fake) return;
    Fake<void (*)(const char*)>("FakeEos_Reset")("self");
    Check(InstallSyncMarker(nullptr, &FakeRedirect), "the marker installs");
    const std::string sdk = "EOSSDK-Win64-Shipping.dll!";
    Check(redirected == std::vector<std::string>({sdk + "EOS_Lobby_LeaveLobby", sdk + "EOS_Lobby_DestroyLobby",
                                                 sdk + "EOS_Platform_Tick", sdk + "EOS_Lobby_CreateLobby",
                                                 sdk + "EOS_Lobby_JoinLobby"}),
          "leave, destroy, tick, create and join are redirected, in that order");

    // Hosting: the game creates a room; its completion (a not-final run, then the final one) reaches the game.
    const int lobbyHandle = 0;
    const LobbyOptions create{1, User("self"), nullptr};
    reinterpret_cast<LobbyCallFn>(replacements["EOS_Lobby_CreateLobby"])(
        const_cast<int*>(&lobbyHandle), &create, &gameMarker, &GameLobbyCallback);
    Tick();
    Check(gameResults.size() == 2 && gameResults[1] == 0 && gameLobby == "lobby-created" && gameClientDataOk,
          "the game gets both runs of its completion with its own ClientData");
    Check(SplitSync().LobbyId() == "lobby-created", "the room entered is known");
    Check(Updates() == 1 && Published(kSplitSyncKey) == -1, "our marker is sent on the tick that enters");
    Tick();
    Check(Published(kSplitSyncKey) == kSplitSyncFormat && Published(kSplitSyncSeqKey) == 1, "and is in the lobby");
    Check(PeerReadsSplitSync(User("self")) == false, "we are not observed yet");

    // Two members join: one with the split, one without. The next observation sees them and sends our marker again.
    AddMember("newer", true);
    AddMember("older", false);
    Tick();
    Check(Updates() == 1, "members are observed once a second, not every tick");
    NextObservation();
    Check(Updates() == 2, "a newcomer gets our marker sent again");
    Tick();
    Check(Published(kSplitSyncSeqKey) == 2, "with a new sequence number, so EOS sends it to everyone");
    Check(PeerReadsSplitSync(User("newer")) && PeerReadsSplitSync(User("self")), "members with the marker read it");
    Check(!PeerReadsSplitSync(User("older")) && !PeerReadsSplitSync(User("older")), "a member without it does not");
    Check(!PeerReadsSplitSync(User("stranger")), "nor does someone not in the room");

    // Our copy of the lobby loses an attribute: the marker stays.
    Fake<void (*)(const char*)>("FakeEos_ClearAttributes")("newer");
    NextObservation();
    Check(PeerReadsSplitSync(User("newer")), "a marker once seen stays while we are in the room");

    // A publish that fails is sent again with the next observation, not on every tick.
    Fake<void (*)(std::int32_t)>("FakeEos_SetUpdateResult")(10);
    AddMember("third", true);
    NextObservation();
    Check(Updates() == 3, "the third member gets our marker sent");
    Tick();  // its completion: failed
    Tick();
    Check(Updates() == 3, "a failed publish is not repeated on every tick");
    Fake<void (*)(std::int32_t)>("FakeEos_SetUpdateResult")(0);
    NextObservation();
    Check(Updates() == 4, "it is sent again a second later");
    Tick();
    Check(Published(kSplitSyncSeqKey) == 4, "and is in the lobby then");

    // No local copy of the lobby: nothing is learnt and nothing is forgotten.
    Fake<void (*)(int)>("FakeEos_SetCopyFails")(1);
    NextObservation();
    Check(PeerReadsSplitSync(User("third")) && Updates() == 4, "without a lobby copy what we know stays");
    Fake<void (*)(int)>("FakeEos_SetCopyFails")(0);

    // Leaving: everything about the room is forgotten, records and held packets included.
    StubInfo stub{9, 4, 0};
    const std::uint8_t record[4] = {1, 2, 3, 4};
    stub.hash = RecordHash(record, sizeof(record));
    StoreRecord(stub, record);
    reinterpret_cast<LeaveFn>(replacements["EOS_Lobby_LeaveLobby"])(nullptr, nullptr, nullptr, nullptr);
    Check(Fake<int (*)()>("FakeEos_Leaves")() == 1, "the game's leave goes through");
    Check(!SplitSync().InLobby() && !PeerReadsSplitSync(User("newer")), "after leaving nobody is known");
    Check(!FindRecord(stub, nullptr), "and the records of that room are gone");
    const int updates = Updates();
    NextObservation();
    Check(Updates() == updates, "outside a room nothing is published");

    // Joining someone else's room: nothing carries over from the last one.
    const LobbyOptions join{1, nullptr, User("self")};
    reinterpret_cast<LobbyCallFn>(replacements["EOS_Lobby_JoinLobby"])(
        const_cast<int*>(&lobbyHandle), &join, &gameMarker, &GameLobbyCallback);
    Tick();
    Check(gameLobby == "lobby-joined" && SplitSync().LobbyId() == "lobby-joined" && Updates() == updates + 1,
          "joining publishes our marker in the new room");
    Check(PeerReadsSplitSync(User("third")) && !PeerReadsSplitSync(User("newer")),
          "members are read on entering (newer lost its marker in our copy, and this is a new room)");
    Fake<void (*)(const char*)>("FakeEos_RemoveMember")("older");
    reinterpret_cast<LeaveFn>(replacements["EOS_Lobby_DestroyLobby"])(nullptr, nullptr, nullptr, nullptr);
    Check(!SplitSync().InLobby(), "closing a room leaves it too");
}

void TestLog(const std::wstring& path) {
    const std::string log = ReadLog(path);
    Check(Count(log, "EOS older has not published that it reads a split start message") == 1,
          "a member without the marker is logged once per room");
    Check(Count(log, "publishing our split marker failed (EOS result 10)") == 1, "a failed publish is logged once");
    Check(Count(log, "MISSION sync: published that this machine reads a split start message") >= 3,
          "every publish that got in is logged");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: SyncMarkerTests <log> <fake EOSSDK-Win64-Shipping.dll>\n");
        return 2;
    }
    std::wstring logPath, fakePath;
    for (const char* c = argv[1]; *c; ++c) logPath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    for (const char* c = argv[2]; *c; ++c) fakePath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    DeleteFileW(logPath.c_str());
    LogOpen(logPath.c_str());
    TestRoomWithoutEos();
    TestLobby(fakePath.c_str());
    TestLog(logPath);
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
