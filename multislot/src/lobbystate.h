#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>

#include "packetfit.h"

namespace multislot {

// The EOS lobby the game is in, as EOS itself describes it, and the lobby calls that can leave a machine behind
// in a room it no longer plays in.
//
// A room's kind lives in its lobby: MaxMembers and the SEARCH_TYPE attribute its owner published. Whoever updates
// the lobby must keep both, but the room update (749AD0) writes the game's own 4 and vanilla SEARCH_TYPE, and
// the plugin used to replace them with the setting this machine created its own last room with. A member that became the
// lobby's owner (2026-10-01: after the lobby service dropped the real host's connection) then published a
// 12-player room as a normal 4-player one ("SetMaxMembers 4 -> result 22"). The update now reads the lobby.
//
// The game closes a room with DestroyLobby when it believes it hosts it alone, and EOS refuses that from a
// machine that does not own the lobby; the machine then stays a member (EOS_Lobby_LobbyAlreadyExists, 9002, on
// every later join) that never answers a P2P handshake, and every member's link to it times out for good. So a
// destroy of a lobby someone else owns becomes a leave, a destroy that fails is followed by a leave, and a join
// into a lobby EOS still lists this machine in leaves it first.
//
// The room list (the user's request, 2026-10-04): every machine with this plugin lists normal and MultiSlot rooms
// of every size, whatever its F2 setting. One search asks for both families (hostmode.h SearchTypeRange); between
// them lie the SEARCH_TYPE values of earlier MultiSlot versions, whose rooms are left out of the results the game is
// handed, so the list shows only rooms this build can join.

enum class LobbyKind : int { Unknown = -1, Normal = 0, MultiSlot = 1 };

// What a lobby says about its kind. maxMembers 0: not known.
struct LobbyFacts {
    std::uint32_t maxMembers = 0;
    bool hasSearchType = false;
    std::int64_t searchType = 0;
};
// SEARCH_TYPE decides: this build's mirrored family is MultiSlot, 0x90..0x9F normal. Without one (a lobby just
// created, before its first update) MaxMembers above four is MultiSlot.
LobbyKind KindOf(const LobbyFacts& facts);
// SEARCH_TYPE alone: this build's mirrored family MultiSlot, 0x90..0x9F normal, anything else Unknown (an earlier
// MultiSlot version's room, which this build cannot join).
LobbyKind SearchTypeKind(std::int64_t value);
// The capacity an update keeps in a MultiSlot lobby: its own MaxMembers, at most kMaxPlayers.
int CapacityToKeep(const LobbyFacts& facts);

// What the room update (749AD0, the room object in r13) publishes.
struct RoomUpdate {
    LobbyKind kind;  // Unknown: neither the lobby nor this machine knows; the game's own values stand
    int capacity;    // MultiSlot only
};
// Reads the room's lobby from EOS; when EOS has no copy of it, falls back to how this machine created that lobby
// (if it did). Logs the decision whenever it changes. Called on the game thread, inside the room update.
RoomUpdate DecideRoomUpdate(std::uintptr_t room);

// The lobby the game is in now (read once a second), for the menu label: Unknown outside a lobby and until read.
LobbyKind CurrentLobbyKind();
// Its size while it is a MultiSlot room (the lobby's MaxMembers), 0 otherwise.
int CurrentLobbyCapacity();
// This machine created the lobby the game is in now (the game's IsRoomHost can turn false while it stays in it).
bool CreatedCurrentLobby();

// The game entered `lobbyId`: created here for createdCapacity players, or joined (0). Test seam; the
// CreateLobby/JoinLobby completions call it.
void NoteLobbyEntered(void* lobby, const void* user, const char* lobbyId, std::uint32_t createdCapacity);
void NoteLobbyLeft();

// Redirects EOS_Lobby_LeaveLobby, EOS_Lobby_DestroyLobby, EOS_Lobby_UpdateLobby, EOS_Platform_Tick,
// EOS_Lobby_CreateLobby and EOS_Lobby_JoinLobby. Install it before the other lobby wrappers (syncmarker.h,
// netlog.h): the last one installed is called first, so these are the plugin's wrappers nearest to EOS and their
// own calls skip the others (another plugin's wrapper installed earlier still sits between them and EOS). False when the EOS SDK or one of its exports is missing, or an import could not be redirected.
bool InstallLobbyState(HMODULE game, ImportRedirect redirect);

}  // namespace multislot
