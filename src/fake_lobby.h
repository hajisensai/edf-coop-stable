// Lobby details for a room Epic's lobby service cannot give us: the room we were last in.
//
// While we are in someone else's room the plugin keeps what is needed to come back into it without
// Epic: who hosts it, the identity it proves itself with, where it listens, and the room's attributes as
// the game's room list reads them (LastRoom). The room list then gets one entry more for it, a
// LobbyDetails handle the plugin answers itself (FakeLobbies), and joining that entry goes to the host
// over the direct link (eos_hooks.cpp). The game reads such a handle exactly as an EOS one: attributes
// by index, members, owner, info; and releases it, its attributes and its info the EOS way, so every
// one of those functions must tell our pointers from EOS's (it never dereferences one it did not make).
// Thread-safe.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "eos_min.h"

namespace dn {

// One lobby attribute, owned (EOS_Lobby_AttributeData points into EOS's memory).
struct LobbyAttribute {
    std::string key;
    int32_t type = 0;  // 0 bool, 1 int64, 2 double, 3 string
    int64_t integer = 0;  // bool and int64
    double number = 0.0;
    std::string text;
    int32_t visibility = 0;
};

// The room we were last in as a member (not its owner), enough to come back into it over the direct link.
struct LastRoom {
    std::string roomId;
    std::string host;          // the room owner's EOS id
    std::string hostIdentity;  // the direct-link identity commitment it published
    std::string hostAddress;   // the addresses it advertised
    uint32_t maxMembers = 0;
    std::vector<std::string> members;  // who was in it besides us, the host included
    std::vector<LobbyAttribute> attributes;

    bool usable() const { return !roomId.empty() && !host.empty() && !hostIdentity.empty() && !hostAddress.empty(); }
};

// What a fake LobbyDetails handle answers.
struct FakeDetails {
    std::string roomId;
    std::string owner;
    std::vector<std::string> members;
    uint32_t maxMembers = 0;
    std::vector<LobbyAttribute> attributes;
};

class FakeLobbies {
public:
    FakeLobbies();
    ~FakeLobbies();  // out of line: the owned copies are only complete in fake_lobby.cpp
    FakeLobbies(const FakeLobbies&) = delete;
    FakeLobbies& operator=(const FakeLobbies&) = delete;

    EOS_HLobbyDetails make(FakeDetails details);
    // The details behind `handle`, or false when it is not one of ours.
    bool lookup(EOS_HLobbyDetails handle, FakeDetails* out) const;
    bool owns(EOS_HLobbyDetails handle) const;
    // Frees `handle` when it is ours. False: it is EOS's.
    bool release(EOS_HLobbyDetails handle);

    // An EOS_Lobby_Attribute copy of `attribute`, released through releaseAttribute().
    EOS_Lobby_Attribute* copyAttribute(const LobbyAttribute& attribute);
    bool releaseAttribute(EOS_Lobby_Attribute* attribute);
    // An EOS_LobbyDetails_Info for `details`; `owner` is the owner's EOS handle (the game compares them).
    EOS_LobbyDetails_Info* copyInfo(const FakeDetails& details, EOS_ProductUserId owner);
    bool releaseInfo(EOS_LobbyDetails_Info* info);

    size_t liveHandles() const;  // tests and diagnostics
    size_t liveCopies() const;

private:
    struct OwnedAttribute;
    struct OwnedInfo;
    mutable std::mutex mu_;
    std::unordered_map<void*, std::unique_ptr<FakeDetails>> handles_;
    std::unordered_map<void*, std::unique_ptr<OwnedAttribute>> attributes_;
    std::unordered_map<void*, std::unique_ptr<OwnedInfo>> infos_;
};

}  // namespace dn
