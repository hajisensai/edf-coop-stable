// Exercise the actual import wrappers against in-process fakes: no EOS service or game launch.
#include "../src/netlog.cpp"
#include <cstdio>
#include <thread>
#include <stdexcept>
#include <vector>

using namespace multislot;
namespace {
int failures = 0, sendCalls = 0, receiveCalls = 0, closeCalls = 0, leaveCalls = 0;
const void* expectedOptions = nullptr;
void* expectedHandle = reinterpret_cast<void*>(0x1234);
void Check(bool ok, const char* label) {
    if (!ok) { std::printf("FAIL: %s\n", label); ++failures; }
}
EOS_EResult FakeSend(void* handle, const SendPacketOptions* options) {
    ++sendCalls;
    Check(handle == expectedHandle && options == expectedOptions, "send forwards original arguments");
    return 7;
}
EOS_EResult FakeReceive(void* handle, const void* options, void** peer, SocketId* socket,
                       std::uint8_t* channel, void* data, std::uint32_t* size) {
    ++receiveCalls;
    Check(handle == expectedHandle && options == expectedOptions, "receive forwards original arguments");
    *peer = nullptr;
    *socket = SocketId{1, "test-socket"};
    *channel = 3;
    const std::uint32_t payload[] = {0, 0x12345678};
    std::memcpy(data, payload, sizeof(payload));
    *size = sizeof(payload);
    return 0;
}
EOS_EResult FakeClose(void* handle, const CloseConnectionOptions* options) {
    ++closeCalls;
    Check(handle == expectedHandle && options == expectedOptions, "close forwards original arguments");
    return 9;
}
void Completion(const JoinLobbyCallbackInfo*) {}
void FakeLeave(void* handle, const void* options, void* clientData, JoinLobbyCallback completion) {
    ++leaveCalls;
    Check(handle == expectedHandle && options == expectedOptions && clientData == expectedHandle && completion == &Completion,
          "leave forwards options, client data and callback unchanged");
}

alignas(8) unsigned char manager[0x150]{}, users[8][0x50]{};
alignas(8) unsigned char session[0x38]{}, room[0x170]{};
std::uintptr_t slots[16]{}, slotVector[3]{};
std::uint32_t helloPayload[2]{0, 0x12345678};
int helloCalls = 0, recoverySends = 0, firstResult = 1, secondResult = 0;
bool accepted = false, invalidateAfterFirst = false;
std::uintptr_t helloCaller = 0x12C90F2;
std::uint8_t helloChannel = 0;
std::int32_t helloApi = 3, helloReliability = 1;
SendPacketOptions recorded[2]{};
const SendPacketOptions* gameOptions = nullptr;
const char token[] = "original-token";
template <typename T> void Put(unsigned char* memory, std::size_t offset, T value) {
    std::memcpy(memory + offset, &value, sizeof(value));
}
const void* Peer(int index) { return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x8000 + index)); }
void ResetRecovery(int count = 8) {
    std::memset(manager, 0, sizeof(manager));
    std::memset(users, 0, sizeof(users));
    std::memset(slots, 0, sizeof(slots));
    slotVector[0] = reinterpret_cast<std::uintptr_t>(slots);
    slotVector[1] = slotVector[2] = slotVector[0] + sizeof(slots);
    for (int i = 0; i < count; ++i) {
        slots[i * 2] = reinterpret_cast<std::uintptr_t>(users[i]);
        Put(users[i], 0x10, std::uint32_t(i ? 3 : 1));
        Put(users[i], 0x18, Peer(i));
        Put(users[i], 0x40, std::int32_t(i));
    }
    Put(manager, 0x18, expectedHandle);
    const SocketId socket{1, "test-session"};
    Put(manager, 0x20, socket);
    Put(manager, 0x138, Peer(0));
    Put(manager, 0x140, static_cast<const void*>(slotVector));
    Put(reinterpret_cast<unsigned char*>(gameAddress), 0x20B2AC0, static_cast<const void*>(session));
    Put(session, 0x28, static_cast<const void*>(room));
    Put(room, 0, gameAddress + 0x17ECA00);  // Actual RoomImpl, verified against EDF constructors in tests.cpp.
    Put(room, 0x160, static_cast<const void*>(slotVector));
    handshakeRecovery = true;
    packetDiagnostics = false;
    helloCalls = recoverySends = 0;
    firstResult = 1;
    secondResult = 0;
    accepted = invalidateAfterFirst = false;
    helloPayload[0] = 0;
    helloCaller = 0x12C90F2;
    helloChannel = 0;
    helloApi = 3;
    helloReliability = 1;
}
EOS_EResult FakeRecoverySend(void* handle, const SendPacketOptions* options) {
    Check(handle == expectedHandle, "recovery preserves the EOS handle");
    Check(recoverySends < 2, "recovery never loops");
    if (recoverySends >= 2) return 1;
    recorded[recoverySends] = *options;
    if (!recoverySends) {
        Check(options == gameOptions && options->AllowDelayedDelivery == 0, "first send is unchanged");
    } else {
        Check(accepted, "vanilla AcceptConnection ran before retry");
        Check(options != gameOptions && gameOptions->AllowDelayedDelivery == 0,
              "retry copies options without mutating game memory");
        Check(options->AllowDelayedDelivery == 1 && options->DisableAutoAccept == 1 && options->Reliability == 1,
              "only delayed delivery changes");
        Check(options->Data == helloPayload && options->DataLengthBytes == sizeof(helloPayload) &&
              options->RemoteUserId == Peer(1) && options->LocalUserId == Peer(0) &&
              options->Socket == reinterpret_cast<const SocketId*>(manager + 0x20) && options->Channel == 0,
              "retry preserves payload, peer, local user, socket and channel");
    }
    ++recoverySends;
    return recoverySends == 1 ? firstResult : secondResult;
}
void FakeFinalHello(void* context, const void* peer, const char* suppliedToken) {
    ++helloCalls;
    Check(context == manager && suppliedToken == token, "sender preserves manager and authentication token");
    SendPacketOptions options{};
    options.ApiVersion = helloApi;
    options.LocalUserId = Peer(0);
    options.RemoteUserId = peer;
    options.Socket = reinterpret_cast<const SocketId*>(manager + 0x20);
    options.Channel = helloChannel;
    options.DataLengthBytes = sizeof(helloPayload);
    options.Data = helloPayload;
    options.Reliability = helloReliability;
    options.DisableAutoAccept = 1;
    gameOptions = &options;
    const auto result = DispatchSendPacket(expectedHandle, &options, helloCaller);
    Check(result == (helloCalls == 1 ? firstResult : secondResult), "original EOS result is never fabricated");
    if (result == 1) accepted = true;  // Mirrors the original EDF sender's branch after SendPacket returns.
    if (invalidateAfterFirst) Put(users[1], 0x10, std::uint32_t(2));
}
void RunRecovery(int expectedCalls, const char* label) {
    originalSendPacket = &FakeRecoverySend;
    originalFinalHello = &FakeFinalHello;
    FinalHelloHook(manager, Peer(1), token);
    Check(helloCalls == expectedCalls && recoverySends == expectedCalls && !activeHello, label);
}
void ThrowingHello(void*, const void*, const char*) { throw std::runtime_error("test unwinding"); }
// Apply (plugin.cpp) points the final hello call at FinalHelloHook before InstallNetLog runs, so the hook must
// already know the game's sender then: a hello in that window used to call a null pointer.
void HookBeforeInstall(unsigned char* mappedGame) {
    originalFinalHello = nullptr;
    handshakeRecovery = false;
    InitFinalHello(mappedGame);
    Check(reinterpret_cast<const unsigned char*>(originalFinalHello) == mappedGame + 0x12C8F50,
          "the game's final hello sender is known before the call is redirected");
    // The sender stands in as `inc dword [rcx]; ret`: rcx is the manager argument.
    unsigned char* sender = mappedGame + 0x12C8F50;
    const unsigned char code[] = {0xFF, 0x01, 0xC3};
    std::memcpy(sender, code, sizeof(code));
    DWORD previous = 0;
    Check(VirtualProtect(sender, sizeof(code), PAGE_EXECUTE_READWRITE, &previous) != 0, "fake sender executable");
    FlushInstructionCache(GetCurrentProcess(), sender, sizeof(code));
    alignas(4) std::uint32_t calls = 0;
    FinalHelloHook(&calls, Peer(1), token);
    Check(calls == 1, "a hello before InstallNetLog reaches the game's sender unchanged");
    VirtualProtect(sender, sizeof(code), previous, &previous);
}
void RecoveryTests() {
    auto* mappedGame = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x22CE000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(mappedGame != nullptr, "fake image allocated");
    if (!mappedGame) return;
    HookBeforeInstall(mappedGame);
    gameAddress = reinterpret_cast<std::uintptr_t>(mappedGame);
    ResetRecovery(); RunRecovery(2, "NoConnection: one queued retry");
    ResetRecovery(); secondResult = 1; RunRecovery(2, "retry also fails: no third attempt");
    ResetRecovery(); firstResult = 0; RunRecovery(1, "successful 8-player handshake stays unchanged");
    ResetRecovery(); firstResult = 7; RunRecovery(1, "other EOS errors are not retried");
    ResetRecovery(4); RunRecovery(1, "four-player room stays unchanged");
    ResetRecovery(1); RunRecovery(1, "single player stays unchanged");
    ResetRecovery(5); RunRecovery(2, "five occupied slots qualify");
    ResetRecovery(5); slots[14] = slots[8]; slots[8] = 0; RunRecovery(2, "sparse slots count occupancy");
    ResetRecovery(4); slots[14] = slots[6]; slots[6] = 0; RunRecovery(1, "high slot index alone does not qualify");
    ResetRecovery(); handshakeRecovery = false; RunRecovery(1, "feature disabled preserves original behavior");
    ResetRecovery(); Put(users[1], 0x10, std::uint32_t(2)); RunRecovery(1, "unvalidated peer is never promoted or retried");
    ResetRecovery(); Put(users[0], 0x10, std::uint32_t(0)); RunRecovery(1, "local user must be validated");
    ResetRecovery(); slots[2] = 0; RunRecovery(1, "absent peer is not retried");
    ResetRecovery(); invalidateAfterFirst = true; RunRecovery(1, "peer validation is rechecked after original send");
    ResetRecovery(); slotVector[1] = slotVector[0] + 17; RunRecovery(1, "malformed vector fails closed");
    ResetRecovery(); helloPayload[0] = 1; RunRecovery(1, "non-hello payload is untouched");
    ResetRecovery(); helloChannel = 1; RunRecovery(1, "other channels are untouched");
    ResetRecovery(); helloCaller = 0x123456; RunRecovery(1, "other send callers are untouched");
    ResetRecovery(); helloApi = 2; RunRecovery(1, "unexpected API version is untouched");
    ResetRecovery(); helloReliability = 0; RunRecovery(1, "unexpected reliability is untouched");
    ResetRecovery(); Put(session, 0x28, static_cast<const void*>(nullptr)); RunRecovery(1, "room teardown is untouched");
    ResetRecovery(); Put(room, 0x160, static_cast<const void*>(nullptr)); RunRecovery(1, "old manager after room replacement is untouched");
    ResetRecovery(); Put(room, 0, std::uintptr_t(0)); RunRecovery(1, "unexpected room implementation is untouched");
    ResetRecovery(); Put(room, 0, gameAddress + 0x17EC968); RunRecovery(1, "base room during teardown is not treated as active");
    ResetRecovery();
    Check(!EligibleFinalHello(nullptr, Peer(1)) && !EligibleFinalHello(manager, nullptr) &&
          !EligibleFinalHello(manager, Peer(0)), "null and self peers rejected");
    {
        HelloAttempt outer{manager, Peer(1)};
        HelloScope scope(&outer);
        bool isolated = false;
        std::thread thread([&] { isolated = activeHello == nullptr; });
        thread.join();
        Check(isolated, "another thread cannot inherit recovery scope");
        originalFinalHello = &ThrowingHello;
        try { FinalHelloHook(manager, Peer(1), token); } catch (const std::runtime_error&) {}
        Check(activeHello == &outer, "nested exception restores outer thread scope");
    }
    Check(!activeHello, "recovery retains no scope after returning");
    VirtualFree(mappedGame, 0, MEM_RELEASE);
    gameAddress = 0;
}

// --- log volume: the lines must still carry every change, just not one per packet or parameter ---
std::wstring logPath;
std::string ReadLog() {
    LogFlush();
    std::string text;
    FILE* file = nullptr;
    if (_wfopen_s(&file, logPath.c_str(), L"rb") == 0 && file) {
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

EOS_EResult helloResult = 0;
EOS_EResult FakeHelloSend(void*, const SendPacketOptions*) { return helloResult; }
EOS_EResult FakeAnyClose(void*, const CloseConnectionOptions*) { return 0; }
const void* hellofrom = nullptr;
EOS_EResult FakeHelloReceive(void*, const void*, void** peer, SocketId* socket, std::uint8_t* channel, void* data,
                             std::uint32_t* size) {
    if (!hellofrom) return 13;  // EOS_NotFound: nothing waiting, as on most frames
    *peer = const_cast<void*>(hellofrom);
    *socket = SocketId{1, "test-socket"};
    *channel = 0;
    const std::uint32_t payload[] = {0, 1};
    std::memcpy(data, payload, sizeof(payload));
    *size = sizeof(payload);
    return 0;
}

void SendHello(const void* to) {
    SendPacketOptions options{};
    options.ApiVersion = 3;
    options.LocalUserId = Peer(0);
    options.RemoteUserId = to;
    options.DataLengthBytes = sizeof(helloPayload);
    options.Data = helloPayload;
    options.Reliability = 1;
    DispatchSendPacket(expectedHandle, &options, 0x12C90F2);
}

void ReceiveOnce(const void* from) {
    hellofrom = from;
    void* peer = nullptr;
    SocketId socket{};
    std::uint8_t channel = 0;
    std::uint32_t buffer[2]{}, size = sizeof(buffer);
    HookReceivePacket(expectedHandle, nullptr, &peer, &socket, &channel, buffer, &size);
    hellofrom = nullptr;
}

void HelloSummaryTests() {
    packetDiagnostics = true;
    handshakeRecovery = false;
    originalSendPacket = &FakeHelloSend;
    originalReceivePacket = &FakeHelloReceive;
    originalCloseConnection = &FakeAnyClose;
    const std::string before = ReadLog();
    for (int i = 0; i < 50; ++i) SendHello(Peer(1));
    helloResult = 1;  // EOS_NoConnection
    SendHello(Peer(1));
    helloResult = 0;
    ReceiveOnce(Peer(1));
    CloseConnectionOptions close{1, Peer(0), Peer(1), nullptr};
    HookCloseConnection(expectedHandle, &close);
    std::string log = ReadLog().substr(before.size());
    Check(Count(log, "HANDSHAKE SEND: ") == 1 && Count(log, " 51 hello(s) over ") == 1 && Count(log, ", 1 failed,") == 1 &&
              Count(log, "1 hello(s) received from it; connection closed (other)") == 1,
          "51 hellos to one peer are one line, ended by the close");
    Check(Count(log, "HANDSHAKE SEND FAILED: ") == 1 && Count(log, "result=1 ") == 1, "the failed send has a line of its own");
    Check(log.find("HANDSHAKE SEND: ") < log.find("HANDSHAKE CLOSE: "), "the summary comes before the close it ends with");

    // A run nobody closes ends after 3 s without hellos, on the next poll.
    SendHello(Peer(2));
    SendHello(Peer(3));
    ReceiveOnce(nullptr);
    log = ReadLog().substr(before.size());
    Check(Count(log, "HANDSHAKE SEND: ") == 1, "a run still sending is not cut");
    Sleep(3100);
    ReceiveOnce(nullptr);
    log = ReadLog().substr(before.size());
    Check(Count(log, "no further hello for 3 s") == 1, "quiet runs end on the next poll after 3 s");
    // Leaving the lobby ends every run.
    SendHello(Peer(4));
    originalLeaveLobby = &FakeLeave;
    expectedOptions = nullptr;
    HookLeaveLobby(expectedHandle, nullptr, expectedHandle, &Completion);
    log = ReadLog().substr(before.size());
    Check(Count(log, "the lobby is left") == 1, "leaving the lobby ends the runs");
    // Without the SDK both quiet peers print as "", so the log folds the second line into a repeat count, which it
    // writes once a different line follows.
    Check(Count(log, "(repeated 1 more times: HANDSHAKE SEND:") == 1, "both quiet runs were ended");
    packetDiagnostics = false;
}

EOS_EResult parameterResult = 0;
EOS_EResult FakeSetParameter(void*, const SetParameterOptions*) { return parameterResult; }
int finds = 0;
void FakeFind(void*, const void*, void*, FindCallback) { ++finds; }
void SearchTests() {
    originalSetParameter = &FakeSetParameter;
    originalFind = &FakeFind;
    const std::string before = ReadLog();
    AttributeData type{1, "SEARCH_TYPE", {}, 1};
    type.Value.AsInt64 = 0x93;
    AttributeData mission{1, "MISSION", {}, 1};
    mission.Value.AsInt64 = 7;
    const SetParameterOptions first{1, &type, 5}, second{1, &mission, 0};
    int search = 0, other = 0;
    HookSetParameter(&search, &first);
    HookSetParameter(&other, &second);
    parameterResult = 10;
    HookSetParameter(&search, &second);
    parameterResult = 0;
    HookSetParameter(&search, &second);
    HookFind(&search, nullptr, nullptr, nullptr);
    HookFind(&search, nullptr, nullptr, nullptr);
    const std::string log = ReadLog().substr(before.size());
    Check(Count(log, "EOS LobbySearch Find: 2 parameter(s): SEARCH_TYPE = 147 (0x93)  op/vis=5; MISSION = 7 (0x7)  op/vis=0") == 1,
          "one search's parameters are one line, in order");
    Check(Count(log, "EOS LobbySearch SetParameter MISSION = 7 (0x7)  op/vis=0 -> result 10 (refused)") == 1,
          "a refused parameter is logged at once");
    Check(Count(log, "EOS LobbySearch Find: 0 parameter(s)") == 1 && finds == 2, "a search run again has none left");
    Check(Count(log, "MISSION") == 2, "another search's parameters stay with it");
}

void EosLogFilterTests() {
    const char* note = nullptr;
    auto keep = [&](const char* category, const char* message, std::int32_t level) {
        const LogMessage entry{category, message, level};
        return KeepEosLog(entry, note);
    };
    Check(!keep("LogEOSP2P", "Added new peer 0002fcfd", 400) && !keep("LogEOSP2P", "Accepted connection", 400),
          "P2P bookkeeping at Info is dropped");
    Check(keep("LogEOSP2P", "Connection established with peer", 400) && keep("LogEOSP2P", "Connection closed: Timeout", 400) &&
              keep("LogEOSP2P", "NAT Type: Moderate", 400),
          "a P2P connection's changes of state are kept");
    Check(keep("LogEOSP2P", "Added new peer 0002fcfd", 300) && keep("LogEOSP2P", "Added new peer", 200),
          "every Warning and Error is kept");
    Check(keep("LogEOSLobby", "Member joined", 400) && keep("LogEOSLobby", "anything at all", 500), "every LogEOSLobby line is kept");
    Check(!keep("LogEOSAuth", "token refreshed", 400), "unwanted categories stay out");
    Check(keep("LogEOSUI", "EOS_UI_GetFriendsVisible: Invalid parameter", 200) && note && note[0],
          "the first friends-visible warning is kept and says the rest are not");
    Check(!keep("LogEOSUI", "EOS_UI_GetFriendsVisible: Invalid parameter", 200), "the repeats are dropped");
}

// A completion EOS runs again until its result is final: the wrapper must still be there for the final run.
std::int32_t TestIsComplete(EOS_EResult result) { return result != 0x99; }
std::vector<EOS_EResult> joinResults;
void GameJoined(const JoinLobbyCallbackInfo* info) {
    joinResults.push_back(info->ResultCode);
    Check(info->ClientData == expectedHandle, "the game gets its own ClientData on every run");
}
void FakeJoinTwice(void*, const void*, void* clientData, JoinLobbyCallback callback) {
    JoinLobbyCallbackInfo info{0x99, clientData, "lobby"};
    callback(&info);
    info.ResultCode = 0;
    callback(&info);
}
void CompletionTests() {
    isOperationComplete = &TestIsComplete;
    originalJoinLobby = &FakeJoinTwice;
    HookJoinLobby(nullptr, nullptr, expectedHandle, &GameJoined);
    Check(joinResults == std::vector<EOS_EResult>({0x99, 0}), "a not-final run and the final one both reach the game");
    isOperationComplete = nullptr;
}
}
int main(int argc, char** argv) {
    if (argc > 1)
        for (const char* c = argv[1]; *c; ++c) logPath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    if (!logPath.empty()) {
        DeleteFileW(logPath.c_str());
        LogOpen(logPath.c_str());
    }
    originalSendPacket = &FakeSend;
    originalReceivePacket = &FakeReceive;
    originalCloseConnection = &FakeClose;
    originalLeaveLobby = &FakeLeave;
    SendPacketOptions send{};
    expectedOptions = &send;
    Check(HookSendPacket(expectedHandle, &send) == 7 && sendCalls == 1, "send called once and result preserved");
    void* peer = reinterpret_cast<void*>(0x1234);
    SocketId socket{};
    std::uint8_t channel = 0;
    std::uint32_t payload[2]{1, 2}, size = 0;
    Check(HookReceivePacket(expectedHandle, expectedOptions, &peer, &socket, &channel, payload, &size) == 0 &&
          receiveCalls == 1, "receive called once and result preserved");
    Check(!peer && channel == 3 && size == 8 && payload[0] == 0 && payload[1] == 0x12345678 &&
          std::strcmp(socket.Name, "test-socket") == 0, "receive output is byte-for-byte preserved");
    CloseConnectionOptions close{};
    expectedOptions = &close;
    Check(HookCloseConnection(expectedHandle, &close) == 9 && closeCalls == 1, "close called once and result preserved");
    HookLeaveLobby(expectedHandle, expectedOptions, expectedHandle, &Completion);
    Check(leaveCalls == 1, "leave called once");
    Check(IsHello(payload, 8) && !IsHello(payload, 3) && !IsHello(nullptr, 8), "hello probe checks minimum length");
    RecoveryTests();
    EosLogFilterTests();
    CompletionTests();
    if (!logPath.empty()) {
        HelloSummaryTests();
        SearchTests();
    }
    std::printf("network wrapper forwarding: %d failures\n", failures);
    return failures ? 1 : 0;
}
