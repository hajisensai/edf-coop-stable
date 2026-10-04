#include "weaponguard.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstring>
#include <mutex>
#include <set>

#include "log.h"

namespace multislot {
namespace {

constexpr std::uint32_t kGameStatus = 0x20B2890;   // GameStatus*, as 745670 and 8F7A8C load it
constexpr std::uint32_t kWeaponRows = 0x959F80;    // () -> the WEAPONTABLE's rows (e23f0 on GameStatus+0x130)
constexpr std::size_t kEquipped = 0x6E98;          // GameStatus: six ids per class, 0x18 apart (78E9A0 sends them)
constexpr std::size_t kLevels = 0xEB54;            // GameStatus: a weapon row's 8-byte level, 12 apart (d8a90)
constexpr std::size_t kRoomWeapons = 0x38;         // RoomInfo::PlayerInfo: the six ids (744AB0 writes them)
constexpr std::size_t kMissionRecord = 0x24;       // 790600 at 790887: the decoded record (class) at rbp+0x24

const unsigned char* game = nullptr;
const unsigned char* status = nullptr;  // GameStatus, read with the rows by ReadLocal

std::int32_t ReadId(const std::uint8_t* at) {
    std::int32_t id = 0;
    std::memcpy(&id, at, sizeof(id));
    return id;
}

void WriteId(std::uint8_t* at, std::int32_t id) { std::memcpy(at, &id, sizeof(id)); }

// The first weapon this machine has of `weaponClass`, -1 when none.
std::int32_t FirstOwn(const LocalWeapons& local, int weaponClass) {
    for (const std::int32_t id : local.equipped[weaponClass])
        if (id != -1 && KnownWeapon(id, local.rows)) return id;
    return -1;
}

// GameStatus and the table's rows, or false (the game not that far yet, or not this build of it).
bool ReadStatus(std::uint32_t& rows) {
    __try {
        std::uint64_t pointer = 0;
        std::memcpy(&pointer, game + kGameStatus, sizeof(pointer));
        if (!pointer) return false;
        status = reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(pointer));
        using RowsFn = std::uint32_t(__fastcall*)();
        rows = reinterpret_cast<RowsFn>(game + kWeaponRows)();
        return rows != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadEquipped(std::int32_t (&equipped)[kWeaponClasses][kWeaponSlots]) {
    __try {
        std::memcpy(equipped, status + kEquipped, sizeof(equipped));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint64_t LocalLevel(std::int32_t id) {
    const std::int64_t row = WeaponRow(id);
    if (row < 0 || !status) return 0;
    __try {
        std::uint64_t level = 0;
        std::memcpy(&level, status + kLevels + static_cast<std::size_t>(row) * 12, sizeof(level));
        return level;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadLocal(LocalWeapons& local) {
    if (!game || !ReadStatus(local.rows) || !ReadEquipped(local.equipped)) return false;
    local.level = &LocalLevel;
    return true;
}

// Each unknown id once per run: a room's info is parsed again on every lobby update.
bool FirstTime(std::int32_t id) {
    static std::mutex lock;
    static std::set<std::int32_t> logged;
    std::scoped_lock hold(lock);
    return logged.insert(id).second;
}

void LogSwaps(const std::vector<WeaponSwap>& swaps, const char* where, std::uint32_t rows) {
    for (const WeaponSwap& swap : swaps) {
        if (!FirstTime(swap.from)) continue;
        if (swap.to == -1)
            Log("WEAPONS %s names weapon %d (slot %d), which this machine's weapon table (%u rows) does not have: "
                "shown as an empty slot. Another player's mod added it; install the same mod to see it",
                where, swap.from, swap.slot + 1, rows);
        else
            Log("WEAPONS %s names weapon %d (slot %d), which this machine's weapon table (%u rows) does not have: "
                "this machine builds its own weapon %d there instead. Another player's mod added it; install the same "
                "mod to see it",
                where, swap.from, swap.slot + 1, rows, swap.to);
    }
}

}  // namespace

std::vector<WeaponSwap> GuardRoomWeapons(std::int32_t (&ids)[kWeaponSlots], std::uint32_t rows) {
    std::vector<WeaponSwap> swaps;
    for (int slot = 0; slot < kWeaponSlots; ++slot) {
        if (KnownWeapon(ids[slot], rows)) continue;
        swaps.push_back({slot, ids[slot], -1});
        ids[slot] = -1;
    }
    return swaps;
}

std::vector<WeaponSwap> GuardLoadout(std::uint8_t* record, const LocalWeapons& local) {
    std::vector<WeaponSwap> swaps;
    const std::int32_t weaponClass = ReadId(record + kLoadoutClass);
    if (weaponClass < 0 || weaponClass >= kWeaponClasses) return swaps;
    for (int slot = 0; slot < kWeaponSlots; ++slot) {
        std::uint8_t* at = record + kLoadoutWeapons + slot * sizeof(std::int32_t);
        const std::int32_t id = ReadId(at);
        if (KnownWeapon(id, local.rows)) continue;
        std::int32_t own = local.equipped[weaponClass][slot];
        if (own == -1 || !KnownWeapon(own, local.rows)) own = FirstOwn(local, weaponClass);
        // An empty slot here would still be built from (59DC90 reads every slot of the class): only a weapon will do.
        if (own == -1) continue;
        WriteId(at, own);
        const std::uint64_t level = local.level ? local.level(own) : 0;
        std::memcpy(record + kLoadoutLevels + slot * sizeof(level), &level, sizeof(level));
        swaps.push_back({slot, id, own});
    }
    return swaps;
}

void InitWeaponGuard(const unsigned char* gameBase) {
    game = gameBase;
    status = nullptr;
}

void RoomWeaponsHandler(CpuContext* context) {
    std::uint32_t rows = 0;
    if (!game || !ReadStatus(rows)) return;
    auto* info = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(context->r13));
    std::int32_t ids[kWeaponSlots]{};
    std::memcpy(ids, info + kRoomWeapons, sizeof(ids));
    const std::vector<WeaponSwap> swaps = GuardRoomWeapons(ids, rows);
    if (swaps.empty()) return;
    std::memcpy(info + kRoomWeapons, ids, sizeof(ids));
    LogSwaps(swaps, "a member's room info", rows);
}

void MissionWeaponsHandler(CpuContext* context) {
    LocalWeapons local;
    if (!ReadLocal(local)) return;
    auto* record = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(context->rbp + kMissionRecord));
    const std::vector<WeaponSwap> swaps = GuardLoadout(record, local);
    if (!swaps.empty()) LogSwaps(swaps, "a player's mission loadout", local.rows);
}

}  // namespace multislot
