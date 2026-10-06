#pragma once
// Remote player sync in the game (W2, docs/net-re/player.md; the arithmetic is netplayer.h).
//
// Twenty vtable slots of the five player classes (SoldierBase, AssultSoldier, Engineer, HeavyArmor, PaleWing):
//   slot 92 (record mask)  adds kPlayerBlockBit every SendIntervalFrames sync frames for this machine's player;
//   slot 52 (write)        appends the block after the record's own fields;
//   slot 53 (read)         reads it when the bit is there and keeps the sample for that remote copy;
//   slot 55 (NetworkUpdate) for a remote copy with fresh samples: turns the game's own correction off for the
//                          frame and drives the copy itself (extrapolation, feed-forward, convergence).
// Plus one constant: the packet controller's 90 ms flush timer (EDF+12CB66C), FlushIntervalMs. That one is shared
// with the transport work (W1) and is listed as such in the RE notes.
//
// A member that does not send the block keeps the game's own path on every machine: nothing changes for its copy.
// Every slot and every instruction the hooks rely on is checked first; anything else and nothing is installed.
#include <cstdint>

namespace multislot {

struct PlayerSyncSettings {
    int sendIntervalFrames = 2;  // 1 = every sync frame (60 Hz)
    int flushIntervalMs = 45;    // 0 = leave the game's 90 ms
    float tauMs = 80.0f;
    float maxExtrapolateMs = 200.0f;
    float snapDistance = 8.0f;
    float feedForward = 1.0f;
};

PlayerSyncSettings ReadPlayerSyncSettings(const wchar_t* iniPath);
// True when installed (logged either way). The feature gate (netfeature.h) is asked every frame, not here.
bool InstallPlayerSync(unsigned char* base, const PlayerSyncSettings& settings);

// Slot / patch sites, also checked by tests against EDF.dll.
struct PlayerSyncSlot {
    std::uint32_t rva;     // the vtable entry
    std::uint32_t target;  // what it points at in the game
};
constexpr std::uint32_t kFlushIntervalSite = 0x12CB66C;  // mov dword [r14+0x58], 90.0f (packet Controller ctor)

}  // namespace multislot
