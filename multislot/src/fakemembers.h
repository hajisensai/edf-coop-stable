#pragma once
#include <cstddef>
#include <cstdint>

namespace multislot {

// One entry of the room member list: shared_ptr<eos::RoomInfo::PlayerInfo> {object, control block}.
struct RoomMember {
    void* object;
    void* control;
};

// The container the room member list builder (EDF+7468C0) fills for its callers. Its first field belongs
// to the game and is never touched here.
struct RoomMemberList {
    void* reserved;
    RoomMember* data;
    std::uint64_t capacity;
    std::uint64_t size;
};

// The game's own helpers for that container: EDF+748E70 reallocates the buffer to a capacity, EDF+749040
// grows the list with copies of one entry, counting each reference.
using ReserveFn = std::uint8_t(__fastcall*)(RoomMemberList*, std::uint64_t);
using ResizeFn = void(__fastcall*)(RoomMemberList*, std::uint64_t, const RoomMember*);

// Test mode ([RoomScreen] DummyMembers=1): appends `fakes` copies of the first member, up to `limit` members
// in total, so everything that asks the game for the room's members - the room screen panels, the voice chat
// HUD, anything added later - runs with 5-8 members on one machine. Returns how many were added.
std::size_t AppendFakeMembers(RoomMemberList* list, std::size_t fakes, std::size_t limit, ReserveFn reserve, ResizeFn resize);

// Lets go of one entry's reference (an MSVC shared_ptr's control block) and empties it.
using ReleaseMemberFn = void (*)(RoomMember& member);
// Always: a list of more than `limit` members is cut to `limit`, each dropped entry's reference released, and
// the number dropped returned. The voice chat HUD's update (961140) writes one record per listed member into a
// vector of kMaxPlayers (patches.h), so a room larger than this build's - another mod's, reached through the
// vanilla SEARCH_TYPE family and its capacity of up to 64 - wrote past it and crashed every machine in it, the
// way a fifth member did on 2026-09-18.
std::size_t ClampMembers(RoomMemberList* list, std::size_t limit, ReleaseMemberFn release);

void InitFakeMembers(unsigned char* gameBase);
// Every call to the member list builder is redirected (patches.h MemberListCalls): the list is cut to kMaxPlayers,
// and with dummy members on the fake ones are added.
void* MemberListCallHandler(std::uint32_t rva);

}  // namespace multislot
