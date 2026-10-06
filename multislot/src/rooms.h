#pragma once
#include <cstdint>

namespace multislot {

// Resolves EDF.dll's LobbyDetails member-count wrapper and the EOS exports used below.
void InitRooms(const unsigned char* gameBase);

// Both replace calls to EDF+12AEB30 (rcx = LobbyDetails handle holder, returns the member count),
// so a room's real EOS capacity is used instead of the hard-coded 4.
// Room info parse: returns (capacity << 32) | members.
std::uint64_t RoomCountAndCapacity(void* holder);
// Host room update: returns 4 when the room is full and 0 otherwise; the game compares the
// result with 4 and publishes it as HIDDEN.
std::uint32_t RoomFullCount(void* holder);

// Who reads a room-list handle's info (EOS_LobbyDetails_CopyInfo / EOS_LobbyDetails_Info_Release): EOS's
// exports from InitRooms on; the direct-link part's once it puts an entry of its own in the list, which EOS
// knows nothing of.
using LobbyInfoCopyFn = std::int32_t (*)(void* details, const void* options, void** info);
using LobbyInfoReleaseFn = void (*)(void* info);
void RouteLobbyInfo(LobbyInfoCopyFn copy, LobbyInfoReleaseFn release);

// What both show of a room: its members and its capacity (0: EOS data inconsistent, the vanilla view stands).
struct RoomCount {
    std::uint32_t members;
    std::uint32_t capacity;
};
// Pure decision used by both, kept separate for tests. `members`: what the game's member count read (Epic's lobby
// members; for the room this machine is in, also the members beyond Epic's lobby that the direct-link part lists to
// the game, docs/net-re/roomsize.md §4). roomSize: the room's published size (kRoomSizeKey, patches.h; 0 none), which
// counts once the room is larger than an EOS lobby holds; publishedMembers (kRoomMembersKey; 0 none): its owner's
// count of the members its game has, which a room list entry - Epic's lobby only - shows for such a room.
RoomCount RoomCountFromInfo(std::uint32_t members, std::uint32_t availableSlots, std::uint32_t maxMembers,
                            std::uint32_t roomSize = 0, std::uint32_t publishedMembers = 0);
// Its capacity alone.
std::uint32_t CapacityFromInfo(std::uint32_t members, std::uint32_t availableSlots, std::uint32_t maxMembers,
                               std::uint32_t roomSize = 0);

// The members the game reads in lobby `lobbyId` (EOS_Lobby_CopyLobbyDetailsHandle and EOS_LobbyDetails_GetMemberCount
// through EDF.dll's imports, so with the members beyond Epic's lobby the direct-link part adds to its own room), 0
// when there is no copy of it. What the owner of a room larger than an EOS lobby publishes (kRoomMembersKey).
std::uint32_t GameRoomMemberCount(void* lobbyInterface, const void* user, const char* lobbyId);

}  // namespace multislot
