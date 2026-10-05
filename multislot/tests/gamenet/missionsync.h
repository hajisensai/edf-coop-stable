#pragma once
// The mission start sync as the game runs it, between the machines of a room: every machine's script calls
// MissionSync_Begin (78E9A0) once and MissionSync_Update (790600) every frame until it answers 0. The host
// gathers everyone's request and writes the start message (MissionSync_Res, 78D0E0); every machine reads it
// (790600 through 773740). All of that is the game's code with EDF6Coop's hooks in it.
//
// Stood in for (the rest of the game, which these functions only consult):
//  - GameDataMgr (20B2890): zeroed, with this machine's loadout in the fields MissionSync_Begin reads;
//  - net::Network (20B2AC8), eos::Core (20B2AC0) with its room and eos::GameImpl: GameImpl lists the room's
//    users (the game's own eos::User objects of the Transport, slot = lobby order) and holds the sync objects;
//  - the event controller's two send entries (74E1B0 to one event, 750130 a new broadcast event): they frame
//    the message as 750380 does (with the game's writers) and hand it to the game's packet controller; what the
//    controller delivers is unframed with the game's readers and given to the game's receive (74C570).
#include <cstdint>
#include <string>
#include <vector>

#include "game.h"

namespace gamenet {

struct Loadout {
    std::int32_t soldierClass;  // 0-3
    std::int32_t marker;        // GameDataMgr+0x6E94, travels in the record (rec+8)
    std::int32_t armor;
    std::int32_t firstWeapon;  // six weapons from it, 11 apart
    std::uint32_t weaponRows = 1564;  // this machine's WEAPONTABLE (the stock one has 1564)
};

class MissionSync {
public:
    // `members` in lobby order (each one's player slot); `hostMission`/`hostDifficulty` only matter on the host.
    bool Build(Transport& transport, const std::vector<std::string>& members, const Loadout& loadout,
               std::int32_t mission, std::int32_t difficulty);
    // The end of a frame: the event controller sends what its builders hold.
    void EndFrame() const;
    // Another event message of `bytes` to every member, as the game sends others in the same frames (it shares the
    // builder, and so the controller record, with the start message).
    void Chatter(std::size_t bytes) const;
    void Begin(std::int32_t id) const;           // MissionSync_Begin
    std::int32_t Update(std::int32_t id) const;  // MissionSync_Update: 0 when done
    // What this machine's game holds after the sync: the player count and each player's 0xD4-byte record.
    std::int32_t Players() const;
    std::int32_t Mission() const;
    std::int32_t Difficulty() const;
    std::vector<std::uint8_t> Record(int slot) const;
    void Dump() const;  // the sync objects and users, for a test that went wrong

private:
    Transport* transport_ = nullptr;
    std::uint8_t* gdm_ = nullptr;
    std::uint8_t* gameImpl_ = nullptr;
};

}  // namespace gamenet
