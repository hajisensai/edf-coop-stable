#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <array>

#include "midhook.h"
struct EOS_ProductUserIdDetails;

namespace multislot {

// A member who has been in a room for a long time must not leave it because someone new could not connect.
//
// What the game does (EDF.dll, 2025-01 build; research in the commit that added this file):
//   Link::OnInitial (12D5AA0), the P2P handshake state of the link to one user, gives a link to anyone but the
//   room's host 20000 ms (12D5E0B; a link to the host never times out, 12D5DFF). Past that it sets Link+0xA8
//   (12D5C90) and stops retrying. Every frame the online room update (788460) then asks, unless this machine
//   hosts the room (12BE580: the local user's host flag), whether ANY link timed out (788ABF -> 12BE750 ->
//   12C86B0 -> 12D5A40, Link+0xA8), and if so leaves the room: 787090 -> 728740 releases the RoomImpl
//   (742130 -> 741AB0 -> 12BFB30 -> 12BE860), which calls EOS_Lobby_LeaveLobby (12BE96C).
//   It never asks whose join failed. That is right for the one who just joined (their join failed), and wrong
//   for everybody else: on 2026-09-30 a newcomer whose P2P link never came up made all four non-host members
//   of a room they had played in for 15 minutes leave it, 20 s after he joined; only the host stayed.
//
// With KeepRoomOnPeerTimeout=1 (the default) the call at 788ABF comes here instead. When every timed-out
// link leads to someone who joined this machine's room at least that link's deadline after this machine did,
// this machine had finished its own join before they came, so the failure is theirs: the game is told no link
// timed out, and the peer is left unconnected - exactly what the host already does. Their own game still
// times out towards the members and leaves, the lobby reports them LEFT and the game's normal disconnect path
// removes the user and its link. Anything else (this machine joined with or after them, or a join time is
// not known) gets the game's answer, so a newcomer whose join failed still leaves as before.
//
// Join times come from Users::Add (12B7F50), right after it made the user object (12B80E0).

void InitPeerTimeout(unsigned char* gameBase);
struct NativeUser { std::array<unsigned char,0x18> prefix{}; EOS_ProductUserIdDetails* productId=nullptr; };
struct NativeUserControl { std::array<unsigned char,8> prefix{}; volatile LONG strong=0; volatile LONG weak=0; };
struct NativeWeakUser { NativeUser* object; NativeUserControl* control; };
struct NativeFailedUsers { NativeWeakUser* begin; NativeWeakUser* end; NativeWeakUser* capacity; };
static_assert(offsetof(NativeUser,productId)==0x18 && offsetof(NativeUserControl,strong)==8 && sizeof(NativeWeakUser)==16);
using LiveFailedPeerQuery = bool(*)(const char* room,EOS_ProductUserIdDetails* productId) noexcept; // NOSONAR: atomic, non-owning native bridge callback
using SharedUserRelease = void(__fastcall*)(NativeWeakUser*); // NOSONAR: exact native shared_ptr destructor ABI
inline std::atomic<LiveFailedPeerQuery>& LiveFailedPeerQueryStore() noexcept {
    static std::atomic<LiveFailedPeerQuery> query{nullptr};
    return query;
}
inline void SetLiveFailedPeerQuery(LiveFailedPeerQuery query) noexcept { LiveFailedPeerQueryStore().store(query); }
std::size_t FilterNativeFailedPeers(const unsigned char* room,NativeFailedUsers* vector,LiveFailedPeerQuery query,SharedUserRelease release); // NOSONAR: native bridge callbacks do not allocate or own captures
void NativeFailedPeersHandler(CpuContext* context);

// Mid-function hook at EDF+12B80E0: rax = the new shared_ptr<eos::User>, rbx = {ProductUserId, bool remote}.
void PeerJoinedHandler(CpuContext* context);
// Replaces the call to EDF+12BE750 (RoomImpl: any link timed out) at EDF+788ABF. rcx = the RoomImpl.
bool __fastcall PeerTimeoutLeaveCheck(void* room);

// The pieces, for tests. `tick` is GetTickCount64 milliseconds.
void NotePeerJoined(const void* user, bool local, std::uint64_t tick);
void ForgetPeerJoins();

enum class PeerTimeoutVerdict {
    Leave,  // the game's own answer: some link timed out and this machine leaves the room
    Stay,   // this machine is an established member; the timed-out peers joined after it had
};
struct PeerTimeoutDecision {
    PeerTimeoutVerdict verdict = PeerTimeoutVerdict::Leave;
    std::size_t timedOut = 0;          // timed-out links whose user still exists
    std::uintptr_t peer = 0;           // the eos::User the verdict is about (the deciding one, or the last)
    std::uintptr_t productId = 0;      // its EOS_ProductUserId
    bool joinKnown = false;            // both join times were recorded
    std::int64_t joinedAfterMs = 0;    // how long after this machine the peer joined (negative: before it)
    float deadlineMs = 0;              // that link's handshake deadline
    const char* reason = "";
};
// Reads the room's links and users (read-only, faults caught) and decides; no logging.
PeerTimeoutDecision DecidePeerTimeout(const void* room);
// What PeerTimeoutLeaveCheck does once the game has said a link timed out: decide, log (rate-limited: the
// game asks every frame) and return true when this machine stays in the room.
bool KeepRoomAfterPeerTimeout(const void* room, std::uint64_t now);
void ForgetPeerTimeoutLog();

// Layout read here, tied to the image by PatchTablesMatchEDF.
inline constexpr std::uint32_t kRoomUsersOffset = 0x160;       // RoomImpl -> eos::Users (12BE597)
inline constexpr std::uint32_t kUsersLocalOffset = 0x18;       // eos::Users -> the local user (12BE5AF)
inline constexpr std::uint32_t kRoomLinksOwnerOffset = 0xB0;   // RoomImpl -> P2P link manager (12BE750)
inline constexpr std::uint32_t kLinkListOffset = 0xD0;         // manager -> sentinel of the link list (12C8757)
inline constexpr std::uint32_t kLinkNodeValueOffset = 0x30;    // list node -> Link* (12C8780)
inline constexpr std::uint32_t kLinkUserOffset = 0x60;         // Link -> weak_ptr<eos::User> (12D5BEB..12D5C43)
inline constexpr std::uint32_t kLinkDeadlineOffset = 0xA0;     // Link -> handshake deadline, float ms (12D5C82)
inline constexpr std::uint32_t kLinkTimedOutOffset = 0xA8;     // Link -> timed out (12D5C90, 12D5A40)
inline constexpr std::uint32_t kUserProductIdOffset = 0x18;    // eos::User -> EOS_ProductUserId

}  // namespace multislot
