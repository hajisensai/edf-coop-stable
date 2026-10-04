// Other players' weapon ids this machine's WEAPONTABLE does not have (weaponguard.h), without the game.
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/weaponguard.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

constexpr std::uint32_t kStockRows = 1564;

void Ids() {
    Check(WeaponRow(-1) == -1 && WeaponRow(0) == 0 && WeaponRow(1563) == 1563, "-1 is empty, others are rows");
    Check(WeaponRow(-2) == 0 && WeaponRow(-3) == 1 && WeaponRow(-1581) == 1579, "below -1: |id| - 2 (745659)");
    Check(WeaponRow(INT32_MIN) == 2147483646LL, "and the lowest id does not overflow");
    Check(KnownWeapon(-1, kStockRows) && KnownWeapon(0, kStockRows) && KnownWeapon(1563, kStockRows),
          "an empty slot and the stock rows are known");
    Check(!KnownWeapon(1564, kStockRows) && !KnownWeapon(1579, kStockRows) && !KnownWeapon(-1581, kStockRows),
          "rows another table added are not, however they are written");
    Check(KnownWeapon(1579, 1585), "but they are on a machine with that table");
}

void Room() {
    std::int32_t ids[kWeaponSlots]{12, 1579, -1, 1563, 1564, -1581};
    const auto swaps = GuardRoomWeapons(ids, kStockRows);
    const std::int32_t expected[kWeaponSlots]{12, -1, -1, 1563, -1, -1};
    Check(std::memcmp(ids, expected, sizeof(ids)) == 0, "unknown ids in a member's room info become empty slots");
    Check(swaps.size() == 3 && swaps[0].slot == 1 && swaps[0].from == 1579 && swaps[0].to == -1 && swaps[2].from == -1581,
          "and each replacement is told");
    std::int32_t known[kWeaponSlots]{1, 2, 3, 4, 5, 6};
    Check(GuardRoomWeapons(known, kStockRows).empty(), "known ids are left alone");
}

std::uint64_t Level(std::int32_t id) { return 1000 + static_cast<std::uint64_t>(WeaponRow(id)); }

struct Record {
    std::uint8_t bytes[0xD4]{};
    void Set(std::int32_t weaponClass, const std::int32_t (&ids)[kWeaponSlots]) {
        std::memcpy(bytes + kLoadoutClass, &weaponClass, 4);
        std::memcpy(bytes + kLoadoutWeapons, ids, sizeof(ids));
        for (int s = 0; s < kWeaponSlots; ++s) {
            const std::uint64_t stale = 77;
            std::memcpy(bytes + kLoadoutLevels + s * 8, &stale, 8);
        }
    }
    std::int32_t Id(int slot) const {
        std::int32_t id = 0;
        std::memcpy(&id, bytes + kLoadoutWeapons + slot * 4, 4);
        return id;
    }
    std::uint64_t LevelAt(int slot) const {
        std::uint64_t level = 0;
        std::memcpy(&level, bytes + kLoadoutLevels + slot * 8, 8);
        return level;
    }
};

void Loadout() {
    LocalWeapons local;
    local.rows = kStockRows;
    local.level = &Level;
    for (int c = 0; c < kWeaponClasses; ++c)
        for (int s = 0; s < kWeaponSlots; ++s) local.equipped[c][s] = 100 * (c + 1) + s;
    local.equipped[3][2] = -1;    // Air Raider: nothing in slot 3
    local.equipped[3][4] = 1580;  // and a slot whose own id this table does not have either

    Record record;
    record.Set(3, {1579, 310, 1564, -1, 1581, -1581});
    const auto swaps = GuardLoadout(record.bytes, local);
    Check(record.Id(0) == 400 && record.LevelAt(0) == Level(400), "an unknown id: this machine's own of that class and slot");
    Check(record.Id(1) == 310 && record.LevelAt(1) == 77, "a known id and its level are left alone");
    Check(record.Id(2) == 400 && record.LevelAt(2) == Level(400),
          "nothing of its own in that slot: the first weapon this machine has of the class");
    Check(record.Id(3) == -1, "an empty slot stays empty");
    Check(record.Id(4) == 400, "its own unknown there too: the first known one of the class");
    Check(record.Id(5) == 405 && record.LevelAt(5) == Level(405), "an id written below -1 is decoded first");
    Check(swaps.size() == 4 && swaps[0].from == 1579 && swaps[0].to == 400 && swaps[3].slot == 5, "each replacement is told");

    Record other;
    other.Set(7, {1579, 1, 2, 3, 4, 5});
    Check(GuardLoadout(other.bytes, local).empty() && other.Id(0) == 1579, "a record of no class 0..3 is left");

    LocalWeapons empty;
    empty.rows = kStockRows;
    for (auto& slots : empty.equipped)
        for (auto& id : slots) id = -1;
    Record nothing;
    nothing.Set(0, {1579, 1, 2, 3, 4, 5});
    Check(GuardLoadout(nothing.bytes, empty).empty() && nothing.Id(0) == 1579,
          "with no weapon of the class at all nothing is made up");
    LocalWeapons noLevels = local;
    noLevels.level = nullptr;
    Record unlevelled;
    unlevelled.Set(0, {1579, 1, 2, 3, 4, 5});
    GuardLoadout(unlevelled.bytes, noLevels);
    Check(unlevelled.Id(0) == 100 && unlevelled.LevelAt(0) == 0, "without levels the replacement gets level 0");
}

}  // namespace

int main() {
    Ids();
    Room();
    Loadout();
    if (failures) {
        std::printf("%d weapon guard check(s) failed\n", failures);
        return 1;
    }
    std::printf("weapon guard: all checks passed\n");
    return 0;
}
