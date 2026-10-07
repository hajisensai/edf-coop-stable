#pragma once
// The game's own network objects, built in a test process by the game's own constructors (EDF.dll RVAs below,
// v1.0 of the Steam release that EDF6Coop supports). Only what the game would get from the rest of the game is
// stood in for: the EOS core (eos::internal_Core, a stub with the real observables) and, for the mission start
// message, the few objects listed in mission.cpp. Everything that encrypts, frames, sends, receives, decrypts,
// dispatches, writes and reads a message is the game's code, with EDF6Coop's patches and hooks in it.
//
// The harness is built like the game (MSVC, /MD, no iterator debugging): the std types handed to the game
// (std::string, std::wstring, std::vector, std::function, the shared_ptr layout) have the game's layout.
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "machine.h"

namespace gamenet {

// A field of one of the game's objects, which are raw memory to this harness: read and written by value.
template <typename T>
T Get(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}
template <typename T>
void Put(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}
// A function of this harness as the game stores one (in a vtable, behind a jump).
template <typename F>
void* Address(F function) {
    return reinterpret_cast<void*>(function);
}

// std::shared_ptr's layout: what the game passes and stores. Reference counts live in the control block
// (uses at +8, weaks at +0xC).
struct Shared {
    void* object = nullptr;
    void* control = nullptr;
};
Shared AddRef(const Shared& shared);

struct Game {
    const Machine* machine = nullptr;
    template <typename T>
    T Fn(std::uintptr_t rva) const {
        return machine->At<T>(rva);
    }
    void* New(std::size_t size) const;  // the game's operator new (it frees with its own operator delete)
    // A block laid out as make_shared does: {RefCount vtable `refVtable`, uses 1, weaks 1}, the object at +0x10.
    Shared MakeShared(std::size_t objectSize, std::uintptr_t refVtable) const;
};

// eos::packet::Controller and what it needs, for one machine in a room.
class Transport {
public:
    // `members`: every member's EOS ProductUserId in lobby order (the game adds them in that order, which gives
    // each its network index), this machine among them.
    bool Start(const Machine& machine, const std::string& lobbyId, const std::vector<std::string>& members);
    // eos::Users::Add (12B7F50) / Users::Remove (12B87C0) of one member, as the game's member sync (12BD460) and its
    // member-status handler do when one joins or leaves after Start.
    bool Add(const std::string& member);
    bool Remove(const std::string& member);
    std::vector<std::string> Members() const;  // added and not removed, in the order added
    // One frame of the game's network (eos::internal_Core::Update): EOS_Platform_Tick through EDF.dll's import,
    // the P2P receive loop and link handshakes (p2p::Manager), then the packet controller.
    void Tick() const;
    // Whether the game regards `member` as connected (User flag bit 0, set by the P2P handshake).
    bool Connected(const std::string& member) const;
    int NetworkIndex(const std::string& member) const;  // User+0x40, -1 if unknown
    void* User(const std::string& member) const;        // eos::User*
    Shared UserShared(const std::string& member) const;  // the shared_ptr Users::Add gave (no reference added)
    // eos::packet::Controller::SendReliable (12D0AC0): one record of `type` to `member`.
    bool SendReliable(const std::string& member, std::uint32_t type, const void* data, std::size_t size) const;
    // eos::packet::Controller::SendUnreliable (12D1040): one record of `type` to `member`, never resent.
    bool SendUnreliable(const std::string& member, std::uint32_t type, const void* data, std::size_t size) const;
    // Every record of `type` the controller hands to its subscribers from now on.
    using Handler = std::function<void(int fromIndex, const std::uint8_t* data, std::size_t size)>;
    void Subscribe(std::uint32_t type, Handler handler);
    void* Controller() const { return controller_.object; }
    void* Core() const { return core_; }
    const Game& game() const { return game_; }

private:
    Game game_;
    void* core_ = nullptr;
    Shared users_, manager_, controller_;
    std::vector<std::pair<std::string, Shared>> members_;  // eos::User of every member
    std::vector<Shared> subscriptions_;                     // held weakly by the controller: kept alive here
    void* local_ = nullptr;                                 // the local user (Room+0x30), for our own Add
};

}  // namespace gamenet
