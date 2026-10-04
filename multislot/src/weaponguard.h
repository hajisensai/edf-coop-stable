#pragma once
#include <cstdint>
#include <vector>

#include "midhook.h"
#include "patches.h"

namespace multislot {

// Weapon ids from other players that this machine's WEAPONTABLE does not have. A mod can add rows to the table
// (EDF6VehicleCrew's call-in weapons: rows 1564..1584 after the stock 1564), and the game looks another player's
// ids up without a bounds check: a guest with the stock table crashed in the room at EDF+95A433 when the host
// equipped one (2026-10-04), and a soldier built from such an id in a mission reads past the table too.
//
// They are replaced where other players' ids come in, before anything looks them up:
//   - a member's room info (744AB0, parsed from its lobby attributes): an unknown id becomes an empty slot, which
//     the game draws as one;
//   - the mission start message (790600, every player's loadout record, the sidecars and EDF6Coop's own split
//     records included): an unknown id becomes this machine's own weapon of that class and slot, which the game
//     can build, with this machine's level of it.
// Every replacement is logged once per id.

constexpr int kWeaponClasses = 4;
constexpr int kWeaponSlots = 6;

// The table row an id names: -1 is an empty slot, and an id below -1 stands for |id| - 2 (745659).
constexpr std::int64_t WeaponRow(std::int32_t id) {
    return id >= -1 ? id : -static_cast<std::int64_t>(id) - 2;
}
// Whether this machine can look `id` up: an empty slot, or a row of its table of `rows`.
constexpr bool KnownWeapon(std::int32_t id, std::uint32_t rows) {
    const std::int64_t row = WeaponRow(id);
    return row == -1 || row < static_cast<std::int64_t>(rows);
}

// This machine's weapons: its table's size and each class's saved equipment (-1 empty), as GameStatus holds them.
struct LocalWeapons {
    std::uint32_t rows = 0;
    std::int32_t equipped[kWeaponClasses][kWeaponSlots]{};
    std::uint64_t (*level)(std::int32_t id) = nullptr;  // this machine's level of a weapon it knows; null: 0
};

// What was replaced, for the log.
struct WeaponSwap {
    int slot;
    std::int32_t from;
    std::int32_t to;  // -1: emptied
};

// A member's six weapon ids from its room info: those this machine does not know become empty.
std::vector<WeaponSwap> GuardRoomWeapons(std::int32_t (&ids)[kWeaponSlots], std::uint32_t rows);

// A loadout record as the mission start message decodes it: class at +0, the six ids at +0x8, their levels at
// +0xA4 (8 bytes each). An unknown id becomes this machine's own weapon of that class and slot - or, if it has
// none there, the first it has of the class - with this machine's level of it. A record of no class 0..3 is left.
constexpr std::size_t kLoadoutClass = 0x0, kLoadoutWeapons = 0x8, kLoadoutLevels = 0xA4;
std::vector<WeaponSwap> GuardLoadout(std::uint8_t* record, const LocalWeapons& local);

// The game's side. InitWeaponGuard first; the handlers run at WeaponGuardHooks (patches.h). With [Mission]
// Extend the mission phase hooks the start message's site itself, and plugin.cpp runs MissionWeaponsHandler
// before that handler.
void InitWeaponGuard(const unsigned char* gameBase);
void RoomWeaponsHandler(CpuContext* context);
void MissionWeaponsHandler(CpuContext* context);

}  // namespace multislot
