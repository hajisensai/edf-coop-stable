#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "fakemembers.h"

#include "log.h"
#include "patches.h"
#include "roomview.h"

namespace multislot {
namespace {

constexpr std::uint32_t kMemberList = 0x7468C0;  // (room info, out list) -> 0 on success
constexpr std::uint32_t kReserve = 0x748E70;     // (list, capacity)
constexpr std::uint32_t kResize = 0x749040;      // (list, size, entry): appends copies, counts references

unsigned char* game = nullptr;
using MemberListFn = std::uint32_t(__fastcall*)(void*, RoomMemberList*);
std::size_t lastAdded = 0;
std::size_t lastDropped = 0;
bool failureLogged = false;

// MSVC's _Ref_count_base: the vtable's first two entries destroy the object and free the block.
struct ControlBlock;
struct ControlBlockVtable {
    void(__fastcall* destroy)(ControlBlock*);
    void(__fastcall* deleteThis)(ControlBlock*);
};
struct ControlBlock {
    ControlBlockVtable* vtable;
    long uses;
    long weaks;
};

void ReleaseMember(RoomMember& member) {
    auto* control = static_cast<ControlBlock*>(member.control);
    member = {};
    if (control && InterlockedDecrement(&control->uses) == 0) {
        control->vtable->destroy(control);
        if (InterlockedDecrement(&control->weaks) == 0) control->vtable->deleteThis(control);
    }
}

std::size_t GuardedClamp(RoomMemberList* list) {
    __try {
        return ClampMembers(list, static_cast<std::size_t>(kMaxPlayers), &ReleaseMember);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The list and its helpers belong to the game; a wrong assumption about them must not end the process.
std::size_t GuardedAppend(RoomMemberList* list, std::size_t fakes) {
    __try {
        return AppendFakeMembers(list, fakes, static_cast<std::size_t>(kMaxPlayers),
                                 reinterpret_cast<ReserveFn>(game + kReserve), reinterpret_cast<ResizeFn>(game + kResize));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uint32_t __fastcall MemberListHook(void* roomInfo, RoomMemberList* out) {
    const std::uint32_t result = reinterpret_cast<MemberListFn>(game + kMemberList)(roomInfo, out);
    if (result || !out) {
        lastAdded = 0;
        return result;
    }
    const std::size_t listed = static_cast<std::size_t>(out->size);
    const std::size_t dropped = GuardedClamp(out);
    if (dropped != lastDropped) {
        lastDropped = dropped;
        if (dropped)
            Log("ROOM the member list has %zu members, more than this build's %d: only the first %d are shown and "
                "get voice chat records (a room this large is another mod's, and the rest would crash the voice "
                "chat HUD)", listed, kMaxPlayers, kMaxPlayers);
    }
    const std::size_t fakes = DummyCount();
    if (!fakes) {
        lastAdded = 0;
        return result;
    }
    // An empty list (outside a room, or a room whose members are not there yet) is nothing to report.
    const bool copyable = out && out->size && out->data;
    const std::size_t added = copyable ? GuardedAppend(out, fakes) : 0;
    if (copyable && added != lastAdded)
        Log("ROOMVIEW %zu fake member(s) added to the room's %zu-member list (test mode)", added,
            static_cast<std::size_t>(out->size) - added);
    if (copyable && !added && !failureLogged) {
        failureLogged = true;
        Log("ROOMVIEW fake members could not be added to the member list (members %zu, capacity %zu)",
            static_cast<std::size_t>(out->size), static_cast<std::size_t>(out->capacity));
    }
    lastAdded = added;
    return result;
}

}  // namespace

std::size_t AppendFakeMembers(RoomMemberList* list, std::size_t fakes, std::size_t limit, ReserveFn reserve, ResizeFn resize) {
    if (!list || !fakes || !reserve || !resize) return 0;
    const auto size = static_cast<std::size_t>(list->size);
    if (!size || !list->data || size >= limit || size > list->capacity) return 0;
    const std::size_t wanted = size + fakes > limit ? limit - size : fakes;
    const RoomMember model = list->data[0];
    if (!model.object) return 0;
    if (list->capacity < size + wanted) reserve(list, size + wanted);
    if (!list->data || list->capacity < size + wanted || list->size != size) return 0;
    resize(list, size + wanted, &model);
    return static_cast<std::size_t>(list->size) == size + wanted ? wanted : 0;
}

std::size_t ClampMembers(RoomMemberList* list, std::size_t limit, ReleaseMemberFn release) {
    if (!list || !release || !list->data || list->size <= limit || list->size > list->capacity) return 0;
    const auto size = static_cast<std::size_t>(list->size);
    // The list is cut first, so the game never sees an entry whose reference is already gone.
    list->size = limit;
    for (std::size_t i = limit; i < size; ++i) release(list->data[i]);
    return size - limit;
}

void InitFakeMembers(unsigned char* gameBase) { game = gameBase; }

void* MemberListCallHandler(std::uint32_t rva) {
    for (const auto& call : MemberListCalls())
        if (call.rva == rva) return reinterpret_cast<void*>(&MemberListHook);
    return nullptr;
}

}  // namespace multislot
