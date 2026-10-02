#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>

#include "midhook.h"
#include "patches.h"
#include "roomview.h"

namespace multislot {

// "<n>Player MOD": what kind of room you create as host.
//   OFF (0, default) - a normal 4-player room anyone can find and join, exactly as without the mod; the room
//                      search lists normal rooms only, as the game asks.
//   n (5..32)        - a MultiSlot room for n players that only players with this mod can find and join; the
//                      room search lists MultiSlot rooms only, of every size (invitations still reach any room).
// Every machine has slots for kMaxPlayers, so the size is the host's alone: it goes into the lobby's
// MaxMembers, which every member follows (lobbystate.h). F2 steps through kRoomSizes and OFF on menu screens
// outside a room (saved to the INI as RoomSize). A room keeps the size it was created with for as long as it
// exists. The menu frame (UI/LYT_MAINFRAME.SGO, which the plugin writes to Mods\UI with an extra text field
// MSLabel, modfile.h) shows the label in its lower left corner. The main script plays that frame on the HQ,
// lobby and room screens but not in missions, so the label is gone once a mission starts. In a room it shows
// the room's size to its host and, as a control guide, which keys switch the member page (see ComposeLabel).
constexpr int kRoomSizes[] = {8, 10, 12, 16, 24, 32};
static_assert(kRoomSizes[std::size(kRoomSizes) - 1] == kMaxPlayers, "F2 must reach the largest room");
// A size the INI may hold: 0 (OFF) or kVanillaPlayers+1..kMaxPlayers.
constexpr bool ValidRoomSize(int size) { return size == 0 || (size > kVanillaPlayers && size <= kMaxPlayers); }
// What F2 switches to: the next of kRoomSizes above `size`, OFF after the largest.
constexpr int NextRoomSize(int size) {
    for (int step : kRoomSizes)
        if (step > size) return step;
    return 0;
}

// The SEARCH_TYPE range (high << 32 | low) the room list asks for, for a room kind whose vanilla range is
// [0x91, high]. ON: MultiSlot rooms of every size, [mirror(high), mirror(0x91)]. OFF: normal rooms only, as
// the game asks.
constexpr std::uint64_t SearchTypeRange(std::uint32_t high, bool on) {
    const std::uint64_t top = on ? 2 * kSearchTypeCenter - 0x91 : high;
    const std::uint64_t bottom = on ? 2 * kSearchTypeCenter - high : 0x91;
    return (top << 32) | bottom;
}

// iniPath may be null (nothing is saved). `key` and `padButton` switch the setting, which only happens
// outside a room; `hint` is what the label calls them ("F2/LS").
void InitHostMode(unsigned char* gameBase, const wchar_t* iniPath, int roomSize, int key, std::uint32_t padButton,
                  const wchar_t* hint);
// The size rooms created now get, 0 when OFF.
int HostRoomSize();

// Mid-function hooks for HostModeHooks() in patches.h, by site RVA.
MidHandler HostModeHookHandler(std::uint32_t rva);
// HUiLobby::OnUpdate vtable slot (LobbySlot() in patches.h): the room list screen. F2 pressed while it is shown
// makes it search again with the new setting, as soon as no search is running and no dialog is open over it.
std::uint64_t LobbyOnUpdateHook(void* lobby, void* context);
// Test seam: whether the list may start a search now. searchActive/resultsIn are the search's bytes +0x43 (set when
// a search starts) and +0x40 (set when its results are in); dialogOpen is the lobby's pending dialog callback.
bool LobbyMaySearchAgain(std::uint8_t searchActive, std::uint8_t resultsIn, bool dialogOpen);
// HUiMainFrame::OnUpdate vtable slot (MainFrameSlot() in patches.h).
std::uint64_t MainFrameOnUpdateHook(void* frame, void* context);

// Test seams.
struct MenuContext {
    bool inRoom;               // an online room session exists
    bool roomHost;             // and this player hosts it
    RoomPageView pages;        // the room screen (roomview.h)
    const wchar_t* pageHint;   // "F3/Tab/RS"
    int ghosts;                // ghost players the solo test harness would add (0 = off or not installed)
    const wchar_t* copyArmorHint;  // "F3/RS" while copy armor is installed, nullptr or "" when it is not
    const wchar_t* hostModeHint;   // "F2/LS", what switches the setting outside a room
    int copyArmorTo;               // the armor copy armor is giving this player, 0 when it is giving none
    bool copyArmorAtMax;           // and that armor is this class's ceiling, not what was found in the room
    int roomMode = -1;             // the room's kind as its lobby says (LobbyKind: 1 MultiSlot, 0 normal), -1 unknown
    int roomCapacity = 0;          // and in a MultiSlot room its size (the lobby's MaxMembers)
};
// Long enough for the fullest line a room can show: the room's setting, the page guide and copy armor.
constexpr std::size_t kLabelChars = 96;
// Only what the player can do where they are. Outside a room: "F2/LS 12Player MOD :ON" / "F2/LS Player MOD :OFF",
// the setting for the rooms they create, which is the one thing there is to do there. In a room: the host also
// sees that room's own size ("12Player MOD :ON"), which nothing can change now but nothing else reports; then the
// page guide "F3/Tab/RS: Members 5-8" (the page those inputs switch to) while more than four members are shown, and
// always in a MultiSlot room this player hosts; then "F4/LS copy armor :ON" / ":OFF". Nothing to show is a single
// space. roomSize: the setting (0 OFF); createdSize: the size this machine created its last room with (0 normal),
// shown while context.roomMode is -1.
std::size_t ComposeLabel(const MenuContext& context, int roomSize, int createdSize, wchar_t* out, std::size_t outChars);
// One menu frame update: the Player MOD input edge (down), then the label.
void UpdateMenuFrame(void* frame, bool down, const MenuContext& context);
// The size this machine created its last room with, 0 for a normal room.
int CreatedRoomSize();

}  // namespace multislot
