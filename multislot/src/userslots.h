#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "midhook.h"

namespace multislot {

// Member slots: the game numbers the members of its room by their eos::Users slot. Users::Add (12B7F50) puts a member
// in the first empty slot; Users::Remove (12B87C0) empties the slot and nothing moves up, so the next member to come
// takes it. That number (User+0x40, the network index) goes in the game's packets and in its mission start sync (each
// member writes its own), so every game must have every member in the same slot - and a game that entered later
// (from the lobby, in Epic's order) or saw the joins and leaves in another order does not. The host's game is the
// one the room goes by: its slots go to every member with its Room list (the direct-link part, room_view.h), and
// every other game adds each member in the slot the host's game has it in (UserSlotHooks).

// Where a member of someone else's room learns the host's slot of `member` (dn::hostSlotOf), -1 unknown; and how a
// ProductUserId reads (identity.h ProductUserIdText). Both set by the plugin; unset: no slot is chosen.
using HostSlotFn = int (*)(const std::string& member);
using IdTextFn = const char* (*)(const void* id, char* out, std::size_t size);
void SetUserSlotSources(HostSlotFn hostSlot, IdTextFn idText);

// The slot a member is added in: the host's slot when it is known, inside the table and empty here; otherwise the
// first empty one, as the game chooses.
int ChooseUserSlot(int firstEmpty, int hostSlot, std::size_t slots, bool hostSlotEmpty);

// This machine's game's slots, as Users::Add and Users::Remove left them in its current room: index = slot, the
// member's ProductUserId, "" for an empty slot (trailing empty slots left out). What a host sends (dn::setGameSlotsSource).
std::vector<std::string> GameSlotTable();

// Handlers of UserSlotHooks (patches.h).
MidHandler UserSlotHookHandler(std::uint32_t rva);

}  // namespace multislot
