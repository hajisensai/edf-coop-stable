// Shares plugin facts through our own lobby member, where the game cannot see them.
//
// On joining or creating a lobby the plugin publishes member attributes on its own member: a marker
// saying it runs the plugin and, for a direct-link host, the address to connect to. EDF.dll never
// reads member attributes (it imports none of those functions), while every other plugin reads them
// from the lobby details EOS already keeps locally. That answers "does this peer run the plugin" and
// "where is the host" without sending anything over P2P, where the game would read any unknown
// packet as game data.
#pragma once
#include <windows.h>

#include <mutex>
#include <string>

#include "eos_min.h"

namespace dn {

class LobbyMarker {
public:
    static constexpr const char* kKey = "EDF6DN";           // value: protocol version
    static constexpr const char* kAddressKey = "EDF6DN_ADDR";  // value: space-separated host addresses
    static constexpr int64_t kVersion = 1;

    // Resolves the EOS functions it needs. Returns false (unavailable) when any is missing.
    bool init(HMODULE eos);

    // The local user is now in `lobbyId` (as its owner when it created it). Thread-safe.
    void entered(EOS_HLobby lobby, const char* lobbyId, EOS_ProductUserId localUser, bool owner);
    // The local user left its lobby. Thread-safe.
    void left();
    // The direct-link address this player hosts on ("" = not hosting). Thread-safe.
    void setAddress(const std::string& address);

    // Publishes our attributes when something changed. EOS calls: run on the EOS tick only.
    void tick();

    bool inLobby() const;
    bool isOwner() const;
    // True when `remote` carries the marker in our current lobby.
    bool hasMarker(EOS_ProductUserId remote) const;
    // The lobby owner and the address it advertises ("" when none). Owner may be null.
    std::string ownerAddress(EOS_ProductUserId* owner) const;

private:
    // Copies member attribute `key` of `member` as a string ("" when absent). Caller holds mu_.
    bool readAttribute(EOS_HLobbyDetails details, EOS_ProductUserId member, const char* key, std::string* value) const;
    EOS_HLobbyDetails copyDetailsLocked() const;

    PFN_EOS_Lobby_UpdateLobbyModification updateModification_ = nullptr;
    PFN_EOS_Lobby_UpdateLobby update_ = nullptr;
    PFN_EOS_LobbyModification_AddMemberAttribute addMemberAttribute_ = nullptr;
    PFN_EOS_LobbyModification_Release releaseModification_ = nullptr;
    PFN_EOS_Lobby_CopyLobbyDetailsHandle copyDetails_ = nullptr;
    PFN_EOS_LobbyDetails_CopyMemberAttributeByKey copyMemberAttribute_ = nullptr;
    PFN_EOS_LobbyDetails_GetLobbyOwner getOwner_ = nullptr;
    PFN_EOS_LobbyDetails_Release releaseDetails_ = nullptr;
    PFN_EOS_Lobby_Attribute_Release releaseAttribute_ = nullptr;
    bool ready_ = false;

    mutable std::mutex mu_;
    EOS_HLobby lobby_ = nullptr;
    std::string lobbyId_;
    EOS_ProductUserId localUser_ = nullptr;
    bool owner_ = false;
    std::string address_;
    bool dirty_ = false;  // our attributes differ from what the lobby has
};

}  // namespace dn
