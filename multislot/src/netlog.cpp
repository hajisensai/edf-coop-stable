#include "netlog.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <cstddef>
#include <intrin.h>
#include <string>

#include "crashlog.h"
#include "log.h"
#include "identity.h"
#include "joinlog.h"
#include "packetfit.h"

namespace multislot {
namespace {

// EOS SDK C types, only the fields read here. Layouts match what EDF.dll builds (see NOTES.md).
using EOS_EResult = std::int32_t;
struct CreateLobbyOptions {
    std::int32_t ApiVersion;
    void* LocalUserId;
    std::uint32_t MaxLobbyMembers;
    std::int32_t PermissionLevel;
    std::int32_t bPresenceEnabled;
    std::int32_t bAllowInvites;
    const char* BucketId;
    std::int32_t bDisableHostMigration;
    std::int32_t bEnableRTCRoom;
};
struct SetMaxMembersOptions {
    std::int32_t ApiVersion;
    std::uint32_t MaxMembers;
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
    std::int32_t ValueType;  // 0 bool, 1 int64, 2 double, 3 string
};
struct AddAttributeOptions {
    std::int32_t ApiVersion;
    const AttributeData* Attribute;
    std::int32_t Visibility;
};
struct SetParameterOptions {
    std::int32_t ApiVersion;
    const AttributeData* Parameter;
    std::int32_t ComparisonOp;
};
struct LogMessage {
    const char* Category;
    const char* Message;
    std::int32_t Level;
};
using LogCallback = void(*)(const LogMessage*);

using CreateLobbyFn = void(*)(void*, const CreateLobbyOptions*, void*, void*);
using SetMaxMembersFn = EOS_EResult(*)(void*, const SetMaxMembersOptions*);
using AddAttributeFn = EOS_EResult(*)(void*, const AddAttributeOptions*);
using SetParameterFn = EOS_EResult(*)(void*, const SetParameterOptions*);
using CountFn = std::uint32_t(*)(void*, const void*);
using SetLogCallbackFn = EOS_EResult(*)(LogCallback);
using SetLogLevelFn = EOS_EResult(*)(std::int32_t, std::int32_t);
using AcceptConnectionFn = EOS_EResult(*)(void*, const void*);
struct SocketId {
    std::int32_t ApiVersion;
    char Name[33];
};
struct CloseConnectionOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const void* RemoteUserId;
    const SocketId* Socket;
};
struct SendPacketOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const void* RemoteUserId;
    const SocketId* Socket;
    std::uint8_t Channel;
    std::uint32_t DataLengthBytes;
    const void* Data;
    std::int32_t AllowDelayedDelivery;
    std::int32_t Reliability;
    std::int32_t DisableAutoAccept;
};
static_assert(offsetof(SendPacketOptions, Data) == 0x28);
static_assert(offsetof(SendPacketOptions, AllowDelayedDelivery) == 0x30);
static_assert(offsetof(SendPacketOptions, Reliability) == 0x34);
using SendPacketFn = EOS_EResult(*)(void*, const SendPacketOptions*);
using ReceivePacketFn = EOS_EResult(*)(void*, const void*, void**, SocketId*, std::uint8_t*, void*, std::uint32_t*);
using CloseConnectionFn = EOS_EResult(*)(void*, const CloseConnectionOptions*);
SendPacketFn originalSendPacket = nullptr;
ReceivePacketFn originalReceivePacket = nullptr;
CloseConnectionFn originalCloseConnection = nullptr;
std::uintptr_t gameAddress = 0;
std::atomic<unsigned> handshakeLines{0};
bool packetDiagnostics = true;
bool handshakeRecovery = false;
using FinalHelloFn = void(*)(void*, const void*, const char*);
FinalHelloFn originalFinalHello = nullptr;

// Only lives on the game's final-hello call stack. No packet, token, user or room is retained.
struct HelloAttempt {
    void* manager;
    const void* peer;
    bool retry = false;
    unsigned sends = 0;
    EOS_EResult result = -1;
};
thread_local HelloAttempt* activeHello = nullptr;
struct HelloScope {
    HelloAttempt* previous;
    explicit HelloScope(HelloAttempt* attempt) : previous(activeHello) { activeHello = attempt; }
    ~HelloScope() { activeHello = previous; }
};

bool EligibleFinalHello(void* manager, const void* peer) {
    if (!manager || !peer) return false;
    return Probing([&]() -> bool {
        __try {
            const auto* bytes = static_cast<const unsigned char*>(manager);
            const auto local = *reinterpret_cast<const void* const*>(bytes + 0x138);
            if (!local || local == peer) return false;
            const auto users = *reinterpret_cast<const void* const*>(bytes + 0x140);
            // Do not queue a reply from an old manager after leaving/replacing the active room.
            std::uintptr_t roomUsers = 0;
            const char* failure = nullptr;
            if (!ReadCurrentRoomUsers(gameAddress, roomUsers, failure) ||
                reinterpret_cast<const void*>(roomUsers) != users) return false;
            UserSlotsSnapshot snapshot{};
            if (!ReadUserSlots(users, snapshot) || snapshot.occupied <= 4) return false;
            bool localReady = false, peerReady = false;
            for (const auto& slot : snapshot.slots) {
                const auto id = reinterpret_cast<const void*>(slot.productId);
                if (slot.object && id == local && (slot.flags & 1)) localReady = true;
                // The peer must already have passed the game's validation on this machine.
                if (slot.object && id == peer && (slot.flags & 3) == 3) peerReady = true;
            }
            return localReady && peerReady;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
}

bool MatchFinalHello(void* handle, const SendPacketOptions* options, std::uintptr_t caller) {
    if (!activeHello || caller != 0x12C90F2 || !options) return false;
    return Probing([&]() -> bool {
        __try {
            const auto* manager = static_cast<const unsigned char*>(activeHello->manager);
            std::uint32_t type = ~0u;
            if (!options->Data || options->DataLengthBytes < sizeof(type)) return false;
            std::memcpy(&type, options->Data, sizeof(type));
            return type == 0 && options->ApiVersion == 3 && options->Channel == 0 &&
                options->Reliability == 1 && options->AllowDelayedDelivery == 0 && options->DisableAutoAccept == 1 &&
                options->RemoteUserId == activeHello->peer &&
                options->LocalUserId == *reinterpret_cast<const void* const*>(manager + 0x138) &&
                options->Socket == reinterpret_cast<const SocketId*>(manager + 0x20) &&
                handle == *reinterpret_cast<void* const*>(manager + 0x18);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
}

bool HandshakeBudget() {
    const auto line = handshakeLines.fetch_add(1);
    if (line == 6000) Log("HANDSHAKE packet/close diagnostic budget exhausted; further packet lines suppressed");
    return line < 6000;
}

std::uintptr_t GameRva(const void* address) {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return value >= gameAddress && value - gameAddress < 0x22CE000 ? value - gameAddress : 0;
}

bool IsHello(const void* data, std::uint32_t length) {
    return Probing([&]() -> bool {
        __try {
            std::uint32_t type = ~0u;
            if (!data || length < sizeof(type)) return false;
            std::memcpy(&type, data, sizeof(type));
            return type == 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
}

// The final hello (12C90F2) goes out again and again until the peer answers: one line per peer and handshake
// instead of one per packet (1.5.15 logged each, hundreds a minute). A run ends when no hello went to that peer
// for kHelloRunGapMs, when its connection is closed or when the lobby is left; its line says which. A send that
// fails is logged on its own as well, as it happens.
constexpr std::size_t kHelloRuns = 16;
constexpr ULONGLONG kHelloRunGapMs = 3000;
struct HelloRun {
    const void* remote = nullptr;  // EOS_ProductUserId; null: a free entry
    const void* local = nullptr;
    ULONGLONG first = 0, last = 0;
    unsigned sends = 0, failed = 0, answers = 0;
    std::uint32_t bytes = 0;
    std::uint8_t channel = 0;
    std::int32_t delayed = 0, reliability = 0;
    EOS_EResult result = 0;
};
// Sends, receives and closes come from the game's thread, but nothing promises it: guarded.
SRWLOCK helloLock = SRWLOCK_INIT;
HelloRun helloRuns[kHelloRuns];
std::atomic<ULONGLONG> helloSweepAt{~0ULL};  // the earliest a run can end by its gap

void LogHelloRun(const HelloRun& run, const char* end) {
    if (!HandshakeBudget()) return;
    char local[40]{}, remote[40]{};
    ProductUserIdText(run.local, local, sizeof(local));
    ProductUserIdText(run.remote, remote, sizeof(remote));
    Log("HANDSHAKE SEND: %s -> %s %u hello(s) over %llu ms (bytes=%u channel=%u delayed=%d reliability=%d), %u failed, "
        "last result=%d, %u hello(s) received from it; %s",
        local, remote, run.sends, static_cast<unsigned long long>(run.last - run.first), run.bytes, run.channel,
        run.delayed, run.reliability, run.failed, run.result, run.answers, end);
}

// Ends the runs `match` picks, copied to `out` to be logged once the lock is released. Caller holds helloLock.
template <typename Match>
std::size_t TakeRunsLocked(Match match, HelloRun* out) {
    std::size_t taken = 0;
    ULONGLONG next = ~0ULL;
    for (auto& run : helloRuns) {
        if (!run.remote) continue;
        if (match(run)) {
            out[taken++] = run;
            run = HelloRun{};
        } else if (run.last + kHelloRunGapMs < next) {
            next = run.last + kHelloRunGapMs;
        }
    }
    helloSweepAt.store(next);
    return taken;
}

// remote null: every run.
void EndHelloRuns(const void* remote, const char* why) {
    HelloRun ended[kHelloRuns];
    AcquireSRWLockExclusive(&helloLock);
    const std::size_t count = TakeRunsLocked([&](const HelloRun& run) { return !remote || run.remote == remote; }, ended);
    ReleaseSRWLockExclusive(&helloLock);
    for (std::size_t i = 0; i < count; ++i) LogHelloRun(ended[i], why);
}

void SweepHelloRuns() {
    const ULONGLONG now = GetTickCount64();
    if (now < helloSweepAt.load()) return;
    HelloRun ended[kHelloRuns];
    AcquireSRWLockExclusive(&helloLock);
    const std::size_t count = TakeRunsLocked([&](const HelloRun& run) { return now - run.last > kHelloRunGapMs; }, ended);
    ReleaseSRWLockExclusive(&helloLock);
    for (std::size_t i = 0; i < count; ++i) LogHelloRun(ended[i], "no further hello for 3 s");
}

void NoteHelloSend(const SendPacketOptions& options, EOS_EResult result) {
    SweepHelloRuns();
    const ULONGLONG now = GetTickCount64();
    HelloRun evicted{};
    AcquireSRWLockExclusive(&helloLock);
    HelloRun* run = nullptr;
    HelloRun* unused = nullptr;
    HelloRun* oldest = &helloRuns[0];
    for (auto& entry : helloRuns) {
        if (entry.remote && entry.remote == options.RemoteUserId) run = &entry;
        if (!entry.remote && !unused) unused = &entry;
        if (entry.last < oldest->last) oldest = &entry;
    }
    if (!run) run = unused;
    if (!run) {  // more peers in a handshake at once than entries: the longest quiet one is logged now
        evicted = *oldest;
        *oldest = HelloRun{};
        run = oldest;
    }
    if (!run->remote) {
        run->remote = options.RemoteUserId;
        run->local = options.LocalUserId;
        run->first = now;
        run->bytes = options.DataLengthBytes;
        run->channel = options.Channel;
        run->delayed = options.AllowDelayedDelivery;
        run->reliability = options.Reliability;
    }
    run->last = now;
    ++run->sends;
    if (result != 0) ++run->failed;
    run->result = result;
    if (now + kHelloRunGapMs < helloSweepAt.load()) helloSweepAt.store(now + kHelloRunGapMs);
    ReleaseSRWLockExclusive(&helloLock);
    if (evicted.remote) LogHelloRun(evicted, "more peers in a handshake than this log follows at once");
}

void NoteHelloAnswer(const void* remote) {
    AcquireSRWLockExclusive(&helloLock);
    for (auto& run : helloRuns)
        if (run.remote && run.remote == remote) ++run.answers;
    ReleaseSRWLockExclusive(&helloLock);
}

// Keep the import wrapper itself small so its return address is always the real game caller.
EOS_EResult DispatchSendPacket(void* handle, const SendPacketOptions* options, std::uintptr_t caller) {
    const bool finalHello = MatchFinalHello(handle, options, caller);
    SendPacketOptions queued{};
    const auto* effective = options;
    if (finalHello && activeHello->retry && activeHello->sends == 0) {
        queued = *options;
        queued.AllowDelayedDelivery = 1;
        effective = &queued;
    }
    const auto result = originalSendPacket(handle, effective);
    if (finalHello) {
        ++activeHello->sends;
        activeHello->result = result;
    }
    return Probing([&]() -> EOS_EResult {
        __try {
            if (packetDiagnostics && caller == 0x12C90F2 && effective) {
                NoteHelloSend(*effective, result);
                if (result != 0 && HandshakeBudget()) {
                    char local[40]{}, remote[40]{};
                    ProductUserIdText(effective->LocalUserId, local, sizeof(local));
                    ProductUserIdText(effective->RemoteUserId, remote, sizeof(remote));
                    Log("HANDSHAKE SEND FAILED: %s -> %s bytes=%u channel=%u delayed=%d reliability=%d result=%d "
                        "thread=%lu", local, remote, effective->DataLengthBytes, effective->Channel,
                        effective->AllowDelayedDelivery, effective->Reliability, result, GetCurrentThreadId());
                }
            }
            // EOS refuses these (the mission start message did at eight players); name the sender once per kind.
            if (packetDiagnostics && effective && effective->DataLengthBytes > kEosMaxPacket)
                LogOversizePacket(caller, effective->Channel, effective->Reliability, effective->Data,
                                  effective->DataLengthBytes, result);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return result;
    });
}

EOS_EResult HookSendPacket(void* handle, const SendPacketOptions* options) {
    return DispatchSendPacket(handle, options, GameRva(_ReturnAddress()));
}

EOS_EResult HookReceivePacket(void* handle, const void* options, void** peer, SocketId* socket,
                             std::uint8_t* channel, void* data, std::uint32_t* size) {
    const auto caller = GameRva(_ReturnAddress());
    const auto result = originalReceivePacket(handle, options, peer, socket, channel, data, size);
    // The game polls every frame: the clock that ends a quiet hello run (its summary line) when nothing is sent.
    if (packetDiagnostics) SweepHelloRuns();
    return Probing([&]() -> EOS_EResult {
        __try {
            if (packetDiagnostics && result == 0 && size && IsHello(data, *size)) NoteHelloAnswer(peer ? *peer : nullptr);
            if (result == 0 && size && IsHello(data, *size) && HandshakeBudget()) {
                char remote[40]{};
                ProductUserIdText(peer ? *peer : nullptr, remote, sizeof(remote));
                Log("HANDSHAKE RECEIVE type=0: EOS %s bytes=%u channel=%u socket=%.33s caller=EDF+%llX thread=%lu",
                    remote, *size, channel ? *channel : 255, socket ? socket->Name : "?",
                    static_cast<unsigned long long>(caller), GetCurrentThreadId());
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return result;
    });
}

EOS_EResult HookCloseConnection(void* handle, const CloseConnectionOptions* options) {
    const auto caller = GameRva(_ReturnAddress());
    // Taken here, so the frames are this wrapper's callers and not the probe's.
    void* stack[6]{};
    const auto count = CaptureStackBackTrace(0, 6, stack, nullptr);
    const char* reason = caller == 0x12C7C2C ? "initial-retry" : caller == 0x12C95A2 ? "Users-disconnect-notify" : "other";
    if (packetDiagnostics && options) {
        char end[48]{};
        _snprintf_s(end, _TRUNCATE, "connection closed (%s)", reason);
        EndHelloRuns(options->RemoteUserId, end);
    }
    Probing([&] {
        __try {
            if (options && HandshakeBudget()) {
                char local[40]{}, remote[40]{};
                ProductUserIdText(options->LocalUserId, local, sizeof(local));
                ProductUserIdText(options->RemoteUserId, remote, sizeof(remote));
                std::uintptr_t frames[6]{};
                for (USHORT i = 0; i < count; ++i) frames[i] = GameRva(stack[i]);
                Log("HANDSHAKE CLOSE: %s -> %s reason=%s caller=EDF+%llX thread=%lu stackEDF=%llX,%llX,%llX,%llX,%llX,%llX",
                    local, remote, reason,
                    static_cast<unsigned long long>(caller), GetCurrentThreadId(),
                    static_cast<unsigned long long>(frames[0]), static_cast<unsigned long long>(frames[1]),
                    static_cast<unsigned long long>(frames[2]), static_cast<unsigned long long>(frames[3]),
                    static_cast<unsigned long long>(frames[4]), static_cast<unsigned long long>(frames[5]));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
    return originalCloseConnection(handle, options);
}
struct SetLobbyIdOptions {
    std::int32_t ApiVersion;
    const char* LobbyId;
};
using SetLobbyIdFn = EOS_EResult(*)(void*, const SetLobbyIdOptions*);
struct FindCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
};
using FindCallback = void(*)(const FindCallbackInfo*);
using FindFn = void(*)(void*, const void*, void*, FindCallback);
struct JoinLobbyCallbackInfo {
    EOS_EResult ResultCode;
    void* ClientData;
    const char* LobbyId;
};
using JoinLobbyCallback = void(*)(const JoinLobbyCallbackInfo*);
using JoinLobbyFn = void(*)(void*, const void*, void*, JoinLobbyCallback);
using LeaveLobbyFn = void(*)(void*, const void*, void*, JoinLobbyCallback);
LeaveLobbyFn originalLeaveLobby = nullptr;

void HookLeaveLobby(void* handle, const void* options, void* clientData, JoinLobbyCallback completion) {
    const auto caller = GameRva(_ReturnAddress());
    void* stack[6]{};
    const auto count = CaptureStackBackTrace(0, 6, stack, nullptr);
    std::uintptr_t frames[6]{};
    for (USHORT i = 0; i < count; ++i) frames[i] = GameRva(stack[i]);
    Log("HANDSHAKE LEAVE LOBBY requested: caller=EDF+%llX thread=%lu stackEDF=%llX,%llX,%llX,%llX,%llX,%llX",
        static_cast<unsigned long long>(caller), GetCurrentThreadId(),
        static_cast<unsigned long long>(frames[0]), static_cast<unsigned long long>(frames[1]),
        static_cast<unsigned long long>(frames[2]), static_cast<unsigned long long>(frames[3]),
        static_cast<unsigned long long>(frames[4]), static_cast<unsigned long long>(frames[5]));
    if (packetDiagnostics) EndHelloRuns(nullptr, "the lobby is left");
    originalLeaveLobby(handle, options, clientData, completion);
}

CreateLobbyFn originalCreateLobby = nullptr;
JoinLobbyFn originalJoinLobby = nullptr;
SetMaxMembersFn originalSetMaxMembers = nullptr;
AddAttributeFn originalAddAttribute = nullptr;
SetParameterFn originalSetParameter = nullptr;
CountFn originalSearchResultCount = nullptr;
CountFn originalMemberCount = nullptr;
SetLogCallbackFn originalSetLogCallback = nullptr;
AcceptConnectionFn originalAcceptConnection = nullptr;
SetLobbyIdFn originalSetLobbyId = nullptr;
FindFn originalFind = nullptr;
LogCallback gameLogCallback = nullptr;

std::atomic<std::uint32_t> lastMemberCount{0xFFFFFFFF};

// EOS strings come from the game or the SDK; keep log lines bounded and printable.
const char* Printable(const char* text, char* buffer, std::size_t size) {
    return Probing([&]() -> const char* {
        __try {
            if (!text) return "(null)";
            std::size_t i = 0;
            for (; i + 1 < size && text[i]; ++i) buffer[i] = (text[i] >= 0x20 && text[i] < 0x7F) ? text[i] : '?';
            buffer[i] = 0;
            return buffer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return "(unreadable)";
        }
    });
}

// "KEY = value  op/vis=N", as the log has always shown an attribute.
void FormatAttribute(const AttributeData* data, std::int32_t extra, char* out, std::size_t size) {
    char key[64], text[96];
    if (!data) {
        _snprintf_s(out, size, _TRUNCATE, "(null attribute) op/vis=%d", extra);
        return;
    }
    const char* name = Printable(data->Key, key, sizeof(key));
    switch (data->ValueType) {
    case 0: _snprintf_s(out, size, _TRUNCATE, "%s = bool %d  op/vis=%d", name, data->Value.AsBool, extra); break;
    case 1:
        _snprintf_s(out, size, _TRUNCATE, "%s = %lld (0x%llX)  op/vis=%d", name, data->Value.AsInt64, data->Value.AsInt64, extra);
        break;
    case 2: _snprintf_s(out, size, _TRUNCATE, "%s = %f  op/vis=%d", name, data->Value.AsDouble, extra); break;
    case 3:
        _snprintf_s(out, size, _TRUNCATE, "%s = \"%s\"  op/vis=%d", name, Printable(data->Value.AsUtf8, text, sizeof(text)), extra);
        break;
    default: _snprintf_s(out, size, _TRUNCATE, "%s = type %d  op/vis=%d", name, data->ValueType, extra); break;
    }
}

void LogAttribute(const char* what, const AttributeData* data, std::int32_t extra) {
    char text[192];
    FormatAttribute(data, extra, text, sizeof(text));
    Log("EOS %s %s", what, text);
}

// A room search sets its parameters one call at a time (several per search, a search every few seconds while the
// room list is open): they go into one line with the search they belong to. One EOS refuses is logged at once.
constexpr std::size_t kSearches = 4;
struct SearchParameters {
    void* search = nullptr;  // EOS_HLobbySearch
    std::string text;
    unsigned count = 0;
};
SRWLOCK searchLock = SRWLOCK_INIT;
SearchParameters searches[kSearches];
std::size_t nextSearch = 0;

void NoteSearchParameter(void* search, const char* parameter) {
    SearchParameters dropped;
    AcquireSRWLockExclusive(&searchLock);
    SearchParameters* entry = nullptr;
    for (auto& candidate : searches)
        if (candidate.count && candidate.search == search) entry = &candidate;
    if (!entry) {
        entry = &searches[nextSearch];
        nextSearch = (nextSearch + 1) % kSearches;
        dropped = std::move(*entry);
        *entry = SearchParameters{};
        entry->search = search;
    }
    entry->text += entry->count++ ? "; " : "";
    entry->text += parameter;
    ReleaseSRWLockExclusive(&searchLock);
    // A search released without Find: its parameters still reach the log.
    if (dropped.count) Log("EOS LobbySearch (never run): %u parameter(s): %s", dropped.count, dropped.text.c_str());
}

SearchParameters TakeSearchParameters(void* search) {
    SearchParameters taken;
    AcquireSRWLockExclusive(&searchLock);
    for (auto& entry : searches)
        if (entry.count && entry.search == search) {
            taken = std::move(entry);
            entry = SearchParameters{};
        }
    ReleaseSRWLockExclusive(&searchLock);
    return taken;
}

void HookCreateLobby(void* handle, const CreateLobbyOptions* options, void* clientData, void* completion) {
    char bucket[64];
    if (options)
        Log("EOS CreateLobby api=%d MaxLobbyMembers=%u permission=%d bucket=\"%s\" hostMigrationDisabled=%d rtc=%d",
            options->ApiVersion, options->MaxLobbyMembers, options->PermissionLevel,
            Printable(options->BucketId, bucket, sizeof(bucket)), options->bDisableHostMigration, options->bEnableRTCRoom);
    originalCreateLobby(handle, options, clientData, completion);
}

// Completion delegates are wrapped to log their result. EOS runs one again while its result is not final
// (EOS_EResult_IsOperationComplete false, as for EOS_OperationWillRetry), so the wrapper lives until the final run.
using IsCompleteFn = std::int32_t (*)(EOS_EResult);
IsCompleteFn isOperationComplete = nullptr;  // not resolved: every result is final
bool FinalResult(EOS_EResult result) { return !isOperationComplete || isOperationComplete(result); }

struct JoinContext {
    void* clientData;
    JoinLobbyCallback callback;
};

void OnJoinLobby(const JoinLobbyCallbackInfo* info) {
    const auto* context = static_cast<JoinContext*>(info->ClientData);
    JoinLobbyCallbackInfo forwarded = *info;
    forwarded.ClientData = context->clientData;
    Log("EOS JoinLobby result %d", info->ResultCode);
    const JoinLobbyCallback callback = context->callback;
    if (FinalResult(info->ResultCode)) delete context;
    if (callback) callback(&forwarded);
}

void HookJoinLobby(void* handle, const void* options, void* clientData, JoinLobbyCallback completion) {
    Log("EOS JoinLobby requested");
    originalJoinLobby(handle, options, new JoinContext{clientData, completion}, &OnJoinLobby);
}

EOS_EResult HookSetLobbyId(void* handle, const SetLobbyIdOptions* options) {
    const EOS_EResult result = originalSetLobbyId(handle, options);
    char id[64];
    Log("EOS LobbySearch SetLobbyId %s -> result %d", options ? Printable(options->LobbyId, id, sizeof(id)) : "(null)", result);
    return result;
}

struct FindContext {
    void* clientData;
    FindCallback callback;
};

void OnFind(const FindCallbackInfo* info) {
    const auto* context = static_cast<FindContext*>(info->ClientData);
    FindCallbackInfo forwarded = *info;
    forwarded.ClientData = context->clientData;
    Log("EOS LobbySearch Find result %d", info->ResultCode);
    const FindCallback callback = context->callback;
    if (FinalResult(info->ResultCode)) delete context;
    if (callback) callback(&forwarded);
}

void HookFind(void* handle, const void* options, void* clientData, FindCallback completion) {
    const SearchParameters parameters = TakeSearchParameters(handle);
    Log("EOS LobbySearch Find: %u parameter(s)%s%s", parameters.count, parameters.count ? ": " : "",
        parameters.text.c_str());
    originalFind(handle, options, new FindContext{clientData, completion}, &OnFind);
}

EOS_EResult HookSetMaxMembers(void* handle, const SetMaxMembersOptions* options) {
    const EOS_EResult result = originalSetMaxMembers(handle, options);
    Log("EOS LobbyModification SetMaxMembers %u -> result %d", options ? options->MaxMembers : 0, result);
    return result;
}

EOS_EResult HookAddAttribute(void* handle, const AddAttributeOptions* options) {
    const EOS_EResult result = originalAddAttribute(handle, options);
    LogAttribute("LobbyModification AddAttribute", options ? options->Attribute : nullptr, options ? options->Visibility : -1);
    return result;
}

EOS_EResult HookSetParameter(void* handle, const SetParameterOptions* options) {
    const EOS_EResult result = originalSetParameter(handle, options);
    char text[192];
    FormatAttribute(options ? options->Parameter : nullptr, options ? options->ComparisonOp : -1, text, sizeof(text));
    if (result != 0)
        Log("EOS LobbySearch SetParameter %s -> result %d (refused)", text, result);
    else
        NoteSearchParameter(handle, text);
    return result;
}

std::uint32_t HookSearchResultCount(void* handle, const void* options) {
    const std::uint32_t count = originalSearchResultCount(handle, options);
    Log("EOS LobbySearch results %u", count);
    return count;
}

std::uint32_t HookMemberCount(void* handle, const void* options) {
    const std::uint32_t count = originalMemberCount(handle, options);
    // Polled every frame by the room UI: log changes only.
    if (lastMemberCount.exchange(count) != count) Log("EOS LobbyDetails member count %u", count);
    return count;
}

EOS_EResult HookAcceptConnection(void* handle, const void* options) {
    const EOS_EResult result = originalAcceptConnection(handle, options);
    Log("EOS P2P AcceptConnection -> result %d", result);
    return result;
}

// EOS_ELogCategory values (eos_logging_categories.h) of the categories Wanted() keeps at Info: Presence, P2P,
// Connect, Lobby, RTC, RTCAdmin (its name contains "RTC") and CustomInvites. EOS_ELogLevel Info is 400.
constexpr std::int32_t kInfoCategories[] = {3, 7, 13, 18, 29, 30, 31};
constexpr std::int32_t kEosLogInfo = 400;

bool Wanted(const char* category) {
    return std::strstr(category, "Lobby") || std::strstr(category, "P2P") || std::strstr(category, "RTC") ||
           std::strstr(category, "Connect") || std::strstr(category, "Presence") || std::strstr(category, "CustomInvites");
}

// LogEOSP2P at Info is mostly per-packet and per-peer bookkeeping (one evening of eight players: "Added new peer"
// 415 times, "Accepted" 369, ...). Kept are the connection's changes of state; Warnings and Errors always.
constexpr const char* kP2PInfoKept[] = {"Connection established", "Connection closed", "Connection interrupted",
                                        "NAT Type", "Received connection invitation request for unknown socket"};
// The game asks EOS_UI_GetFriendsVisible with an invalid parameter over and over; after the first, the Warning
// says nothing new.
std::atomic<bool> friendsVisibleLogged{false};

// Whether an SDK message is logged; `note` gets a remark for its line.
bool KeepEosLog(const LogMessage& message, const char*& note) {
    note = "";
    if (message.Level <= 300) {  // Fatal, Error and Warning
        if (!std::strstr(message.Message, "GetFriendsVisible")) return true;
        if (friendsVisibleLogged.exchange(true)) return false;
        note = "  (further ones are not logged)";
        return true;
    }
    if (!Wanted(message.Category)) return false;
    if (!std::strstr(message.Category, "P2P")) return true;
    for (const char* kept : kP2PInfoKept)
        if (std::strstr(message.Message, kept)) return true;
    return false;
}

void OnEosLog(const LogMessage* message) {
    Probing([&] {
        __try {
            // No per-session line limit: the log file drops its oldest lines past 2 MB (log.h), so the lines
            // leading up to a late crash are kept.
            const char* note = "";
            if (message && message->Category && message->Message && KeepEosLog(*message, note))
                Log("EOSSDK %d %.40s: %.600s%s", message->Level, message->Category, message->Message, note);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (gameLogCallback) gameLogCallback(message);
    });
}

EOS_EResult HookSetLogCallback(LogCallback callback) {
    gameLogCallback = callback;
    const EOS_EResult result = originalSetLogCallback(&OnEosLog);
    // The game never raises the SDK log level (every category defaults to Warning); Info shows lobby membership
    // and P2P connections. Only the categories OnEosLog keeps at Info are raised: raising all of them (1.5.12 and
    // before) had the SDK build Info messages for every other category too, on the game's thread, only for
    // OnEosLog to drop them.
    const HMODULE sdk = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
    const auto setLevel = sdk ? reinterpret_cast<SetLogLevelFn>(GetProcAddress(sdk, "EOS_Logging_SetLogLevel")) : nullptr;
    int raised = 0;
    EOS_EResult level = -1;
    for (const std::int32_t category : kInfoCategories) {
        level = setLevel ? setLevel(category, kEosLogInfo) : -1;
        if (level == 0) ++raised;
    }
    Log("EOS logging routed to plugin log (SetCallback result %d, SetLogLevel(Info) for %d of %zu categories, last result %d)",
        result, raised, std::size(kInfoCategories), level);
    return result;
}

using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
TerminateProcessFn originalTerminate = nullptr;

BOOL WINAPI TerminateProcessHook(HANDLE process, UINT code) {
    // Only this process ending counts as the game shutting down.
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        LogShutdown("the game exited");
    return originalTerminate ? originalTerminate(process, code) : FALSE;
}

bool RedirectImport(HMODULE module, const char* dll, const char* function, void* replacement, void** original) {
    const auto base = reinterpret_cast<unsigned char*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    for (auto entry = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); entry->Name; ++entry) {
        if (_stricmp(reinterpret_cast<const char*>(base + entry->Name), dll) != 0) continue;
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (entry->OriginalFirstThunk ? entry->OriginalFirstThunk : entry->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) continue;
            DWORD previous = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &previous)) return false;
            *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            DWORD ignored = 0;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), previous, &ignored);
            return true;
        }
    }
    return false;
}

}  // namespace

bool RedirectGameImport(HMODULE game, const char* dll, const char* function, void* replacement, void** original) {
    return RedirectImport(game, dll, function, replacement, original);
}

bool InstallExitMarker(HMODULE game) {
    if (originalTerminate) return true;  // already wrapped
    void* previous = nullptr;
    const bool ok = RedirectImport(game, "KERNEL32.dll", "TerminateProcess",
                                   reinterpret_cast<void*>(&TerminateProcessHook), &previous) ||
                    RedirectImport(game, "kernel32.dll", "TerminateProcess",
                                   reinterpret_cast<void*>(&TerminateProcessHook), &previous);
    if (!ok) return false;
    originalTerminate = reinterpret_cast<TerminateProcessFn>(previous);
    // The game must always be able to exit. If the slot held something unusable - null, or our own hook
    // because something redirected it already - fall back to the real one rather than return FALSE from
    // the wrapper and leave the process unable to end.
    if (!originalTerminate || originalTerminate == &TerminateProcessHook) {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        originalTerminate = kernel ? reinterpret_cast<TerminateProcessFn>(
                                         reinterpret_cast<void*>(GetProcAddress(kernel, "TerminateProcess")))
                                   : nullptr;
    }
    if (!originalTerminate) {  // could not find a way through: put the slot back and stay out of it
        void* restore = nullptr;
        RedirectImport(game, "KERNEL32.dll", "TerminateProcess", reinterpret_cast<void*>(previous), &restore);
        return false;
    }
    return true;
}

void FinalHelloHook(void* manager, const void* peer, const char* token) {
    HelloAttempt attempt{manager, peer};
    const bool eligible = handshakeRecovery && !activeHello && EligibleFinalHello(manager, peer);
    HelloScope scope(eligible ? &attempt : nullptr);
    originalFinalHello(manager, peer, token);
    // The unmodified sender has now run its own AcceptConnection after EOS_NoConnection (1).
    // Retry once only after that; do not fabricate success or bypass validation/teardown/timeouts.
    if (eligible && attempt.sends == 1 && attempt.result == 1 && EligibleFinalHello(manager, peer)) {
        attempt.retry = true;
        attempt.sends = 0;
        attempt.result = -1;
        originalFinalHello(manager, peer, token);
        char remote[40]{};
        ProductUserIdText(peer, remote, sizeof(remote));
        Log("HANDSHAKE RECOVERY: final hello EOS %s retry after NoConnection, delayed=1 sends=%u result=%d (queued is not confirmed)",
            remote, attempt.sends, attempt.result);
    }
}

void InitFinalHello(const unsigned char* gameBase) {
    originalFinalHello = reinterpret_cast<FinalHelloFn>(const_cast<unsigned char*>(gameBase) + 0x12C8F50);
}

int InstallNetLog(HMODULE game, bool diagnostics, bool recovery) {
    gameAddress = reinterpret_cast<std::uintptr_t>(game);
    packetDiagnostics = diagnostics;
    handshakeRecovery = false;
    constexpr const char* sdk = "EOSSDK-Win64-Shipping.dll";
    if (const HMODULE eos = GetModuleHandleA(sdk))
        isOperationComplete = reinterpret_cast<IsCompleteFn>(
            reinterpret_cast<void*>(GetProcAddress(eos, "EOS_EResult_IsOperationComplete")));
    struct Entry {
        const char* name;
        void* replacement;
        void** original;
    };
    const Entry entries[] = {
        {"EOS_Lobby_CreateLobby", reinterpret_cast<void*>(&HookCreateLobby), reinterpret_cast<void**>(&originalCreateLobby)},
        {"EOS_Lobby_JoinLobby", reinterpret_cast<void*>(&HookJoinLobby), reinterpret_cast<void**>(&originalJoinLobby)},
        {"EOS_Lobby_LeaveLobby", reinterpret_cast<void*>(&HookLeaveLobby), reinterpret_cast<void**>(&originalLeaveLobby)},
        {"EOS_LobbyModification_SetMaxMembers", reinterpret_cast<void*>(&HookSetMaxMembers), reinterpret_cast<void**>(&originalSetMaxMembers)},
        {"EOS_LobbyModification_AddAttribute", reinterpret_cast<void*>(&HookAddAttribute), reinterpret_cast<void**>(&originalAddAttribute)},
        {"EOS_LobbySearch_SetParameter", reinterpret_cast<void*>(&HookSetParameter), reinterpret_cast<void**>(&originalSetParameter)},
        {"EOS_LobbySearch_GetSearchResultCount", reinterpret_cast<void*>(&HookSearchResultCount), reinterpret_cast<void**>(&originalSearchResultCount)},
        {"EOS_LobbyDetails_GetMemberCount", reinterpret_cast<void*>(&HookMemberCount), reinterpret_cast<void**>(&originalMemberCount)},
        {"EOS_P2P_AcceptConnection", reinterpret_cast<void*>(&HookAcceptConnection), reinterpret_cast<void**>(&originalAcceptConnection)},
        {"EOS_P2P_CloseConnection", reinterpret_cast<void*>(&HookCloseConnection), reinterpret_cast<void**>(&originalCloseConnection)},
        {"EOS_P2P_SendPacket", reinterpret_cast<void*>(&HookSendPacket), reinterpret_cast<void**>(&originalSendPacket)},
        {"EOS_P2P_ReceivePacket", reinterpret_cast<void*>(&HookReceivePacket), reinterpret_cast<void**>(&originalReceivePacket)},
        {"EOS_Logging_SetCallback", reinterpret_cast<void*>(&HookSetLogCallback), reinterpret_cast<void**>(&originalSetLogCallback)},
        {"EOS_LobbySearch_SetLobbyId", reinterpret_cast<void*>(&HookSetLobbyId), reinterpret_cast<void**>(&originalSetLobbyId)},
        {"EOS_LobbySearch_Find", reinterpret_cast<void*>(&HookFind), reinterpret_cast<void**>(&originalFind)},
    };
    int redirected = 0;
    for (const auto& entry : entries) {
        const bool send = std::strcmp(entry.name, "EOS_P2P_SendPacket") == 0;
        if (!diagnostics && !(recovery && send)) continue;
        if (RedirectImport(game, sdk, entry.name, entry.replacement, entry.original)) {
            ++redirected;
            if (send) handshakeRecovery = recovery;
        } else {
            Log("NETLOG: import %s not found; not logged", entry.name);
        }
    }
    Log("HandshakeRecovery=%d: %s", handshakeRecovery ? 1 : 0,
        handshakeRecovery ? "experimental final-hello NoConnection retry for 5+ occupied users" : "off");
    return redirected;
}

}  // namespace multislot
