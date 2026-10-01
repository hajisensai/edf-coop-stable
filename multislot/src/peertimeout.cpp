#include "peertimeout.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "crashlog.h"
#include "identity.h"
#include "log.h"
#include "patches.h"  // kMaxPlayers

namespace multislot {
namespace {

unsigned char* game = nullptr;
constexpr std::uint32_t kAnyLinkTimedOut = 0x12BE750;  // RoomImpl: [room+0xB0] ? 12C86B0(manager) : false

// Written by Users::Add (the EOS callback thread), read by the room update: both behind the lock.
SRWLOCK joinLock = SRWLOCK_INIT;
struct Join {
    std::uintptr_t user = 0;
    std::uint64_t tick = 0;
};
// The local user is kept on its own: a long evening of people coming and going must not push it out.
Join localJoin{};
// Everyone else, newest last. A timed-out link is always to someone who joined recently, and a room holds at
// most kMaxPlayers users, so 64 joins back (four rooms' worth for the larger builds) is far more than a decision
// ever needs.
constexpr std::size_t kJoinRing = 4 * kMaxPlayers > 64 ? 4 * kMaxPlayers : 64;
Join joins[kJoinRing]{};
std::size_t joinCount = 0;

bool JoinTick(std::uintptr_t user, bool local, std::uint64_t& tick) {
    AcquireSRWLockShared(&joinLock);
    bool found = false;
    if (local) {
        found = user && localJoin.user == user;
        tick = localJoin.tick;
    } else {
        for (std::size_t i = 0; i < kJoinRing && i < joinCount && !found; ++i) {
            const auto& join = joins[(joinCount - 1 - i) % kJoinRing];
            if (join.user != user) continue;
            found = true;
            tick = join.tick;
        }
    }
    ReleaseSRWLockShared(&joinLock);
    return found;
}

// A room of kMaxPlayers has at most kMaxPlayers - 1 links; the walk stops well past that on a broken list.
constexpr std::size_t kMaxLinks = 2 * kMaxPlayers > 32 ? 2 * kMaxPlayers : 32;
struct TimedOutLink {
    std::uintptr_t user = 0;  // 0: the user is gone already (the weak_ptr has expired)
    std::uintptr_t productId = 0;
    float deadline = 0;
};

// Mirrors 12C86B0: every node of the manager's link list, Link+0xA8. Only reads, and only inside __try.
bool ReadTimedOutLinks(const void* room, std::uintptr_t& local, TimedOutLink* out, std::size_t& count) {
    count = 0;
    local = 0;
    if (!room) return false;
    return Probing([&]() -> bool {
        __try {
            const auto base = reinterpret_cast<std::uintptr_t>(room);
            const auto users = *reinterpret_cast<const std::uintptr_t*>(base + kRoomUsersOffset);
            if (!users) return false;
            local = *reinterpret_cast<const std::uintptr_t*>(users + kUsersLocalOffset);
            const auto manager = *reinterpret_cast<const std::uintptr_t*>(base + kRoomLinksOwnerOffset);
            if (!manager) return false;
            const auto sentinel = *reinterpret_cast<const std::uintptr_t*>(manager + kLinkListOffset);
            if (!sentinel) return false;
            auto node = *reinterpret_cast<const std::uintptr_t*>(sentinel);
            for (std::size_t walked = 0; node != sentinel; ++walked) {
                if (!node || walked >= kMaxLinks) return false;
                const auto link = *reinterpret_cast<const std::uintptr_t*>(node + kLinkNodeValueOffset);
                if (link && *reinterpret_cast<const std::uint8_t*>(link + kLinkTimedOutOffset)) {
                    auto& entry = out[count++];
                    entry = {};
                    entry.deadline = *reinterpret_cast<const float*>(link + kLinkDeadlineOffset);
                    // weak_ptr<eos::User>: object, then the control block whose use count (+8) says it is alive.
                    const auto user = *reinterpret_cast<const std::uintptr_t*>(link + kLinkUserOffset);
                    const auto control = *reinterpret_cast<const std::uintptr_t*>(link + kLinkUserOffset + 8);
                    if (user && control && *reinterpret_cast<const std::int32_t*>(control + 8) > 0) {
                        entry.user = user;
                        entry.productId = *reinterpret_cast<const std::uintptr_t*>(user + kUserProductIdOffset);
                    }
                }
                node = *reinterpret_cast<const std::uintptr_t*>(node);
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
}

// The game's own deadline (12D5E0B) when the link holds nothing sensible.
constexpr float kGameDeadlineMs = 20000.0f;

// Rate limit of the lines below: the game asks every frame for as long as the link stays timed out.
constexpr std::uint64_t kRepeatLogMs = 30000;
std::uintptr_t lastPeer = 0;
PeerTimeoutVerdict lastVerdict = PeerTimeoutVerdict::Leave;
bool logged = false;
std::uint64_t lastLog = 0;
unsigned long long quietChecks = 0;

}  // namespace

void InitPeerTimeout(unsigned char* gameBase) { game = gameBase; }

void NotePeerJoined(const void* user, bool local, std::uint64_t tick) {
    if (!user) return;
    const Join join{reinterpret_cast<std::uintptr_t>(user), tick};
    AcquireSRWLockExclusive(&joinLock);
    if (local)
        localJoin = join;
    else
        joins[joinCount++ % kJoinRing] = join;
    ReleaseSRWLockExclusive(&joinLock);
}

void ForgetPeerJoins() {
    AcquireSRWLockExclusive(&joinLock);
    localJoin = {};
    for (auto& join : joins) join = {};
    joinCount = 0;
    ReleaseSRWLockExclusive(&joinLock);
}

void PeerJoinedHandler(CpuContext* context) {
    std::uintptr_t user = 0;
    std::uint8_t remote = 0xFF;
    const bool read = Probing([&]() -> bool {
        __try {
            user = *reinterpret_cast<const std::uintptr_t*>(static_cast<std::uintptr_t>(context->rax));
            remote = *reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(context->rbx) + 8);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
    // An unreadable add records nothing, and a decision about that user then falls back to the game's.
    if (read && user && remote <= 1) NotePeerJoined(reinterpret_cast<const void*>(user), remote == 0, GetTickCount64());
}

PeerTimeoutDecision DecidePeerTimeout(const void* room) {
    PeerTimeoutDecision decision{};
    TimedOutLink links[kMaxLinks]{};
    std::size_t count = 0;
    std::uintptr_t local = 0;
    if (!ReadTimedOutLinks(room, local, links, count)) {
        decision.reason = "the room's links could not be read";
        return decision;
    }
    std::uint64_t localTick = 0;
    if (!JoinTick(local, true, localTick)) {
        decision.reason = "when this machine joined the room is not known";
        return decision;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& link = links[i];
        if (!link.user) continue;  // already removed from the room; its link goes with it
        ++decision.timedOut;
        decision.peer = link.user;
        decision.productId = link.productId;
        decision.deadlineMs = link.deadline > 0.0f && link.deadline <= 600000.0f ? link.deadline : kGameDeadlineMs;
        decision.joinKnown = false;
        decision.joinedAfterMs = 0;
        std::uint64_t peerTick = 0;
        if (!JoinTick(link.user, false, peerTick)) {
            decision.reason = "when they joined the room is not known";
            return decision;
        }
        decision.joinKnown = true;
        decision.joinedAfterMs = static_cast<std::int64_t>(peerTick - localTick);
        // This machine's own links were made when it joined, and its own deadline ran out before this peer
        // came: had any of them failed, it would have left then. Anything closer is this machine's join too.
        if (peerTick < localTick || static_cast<float>(peerTick - localTick) < decision.deadlineMs) {
            decision.reason = "this machine joined the room with or after them, so its own join is what failed";
            return decision;
        }
    }
    decision.verdict = PeerTimeoutVerdict::Stay;
    decision.reason = decision.timedOut ? "they joined after this machine was an established member"
                                        : "everyone whose link timed out has left the room already";
    return decision;
}

void ForgetPeerTimeoutLog() {
    lastPeer = 0;
    lastVerdict = PeerTimeoutVerdict::Leave;
    logged = false;
    lastLog = 0;
    quietChecks = 0;
}

bool KeepRoomAfterPeerTimeout(const void* room, std::uint64_t now) {
    const PeerTimeoutDecision decision = DecidePeerTimeout(room);
    const bool stay = decision.verdict == PeerTimeoutVerdict::Stay;
    if (logged && decision.peer == lastPeer && decision.verdict == lastVerdict && now - lastLog < kRepeatLogMs) {
        ++quietChecks;
        return stay;
    }
    char id[40]{};
    ProductUserIdText(reinterpret_cast<const void*>(decision.productId), id, sizeof(id));
    char peer[160]{};
    if (decision.peer && decision.joinKnown)
        _snprintf_s(peer, _TRUNCATE, " (EOS %s, deadline %.0f ms, joined %.1f s after this machine)", id[0] ? id : "?",
                    static_cast<double>(decision.deadlineMs), static_cast<double>(decision.joinedAfterMs) / 1000.0);
    else if (decision.peer)
        _snprintf_s(peer, _TRUNCATE, " (EOS %s, deadline %.0f ms)", id[0] ? id : "?",
                    static_cast<double>(decision.deadlineMs));
    char repeats[64]{};
    if (logged && quietChecks)
        _snprintf_s(repeats, _TRUNCATE, "; %llu more checks since the last line", quietChecks);
    if (stay)
        Log("HANDSHAKE TIMEOUT: this machine STAYS in the room, which the game would have left "
            "(KeepRoomOnPeerTimeout=1): %s%s. %zu timed-out link(s) left unconnected until they finish or the lobby "
            "removes them%s",
            decision.reason, peer, decision.timedOut, repeats);
    else
        Log("HANDSHAKE TIMEOUT: leaving the room as the game does: %s%s%s", decision.reason, peer, repeats);
    logged = true;
    lastPeer = decision.peer;
    lastVerdict = decision.verdict;
    lastLog = now;
    quietChecks = 0;
    return stay;
}

bool __fastcall PeerTimeoutLeaveCheck(void* room) {
    using AnyLinkTimedOutFn = bool(__fastcall*)(void*);
    if (!reinterpret_cast<AnyLinkTimedOutFn>(game + kAnyLinkTimedOut)(room)) return false;
    return !KeepRoomAfterPeerTimeout(room, GetTickCount64());
}

}  // namespace multislot
