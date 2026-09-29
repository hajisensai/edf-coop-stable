// Tells plugin users apart from vanilla players in the same lobby.
//
// On joining or creating a lobby the plugin publishes a member attribute on its own member. EDF.dll
// never reads member attributes (it imports none of those functions), so the game cannot see it,
// while every other plugin reads it from the lobby details EOS already keeps locally. That makes the
// "is this peer running the plugin" question safe to answer without sending anything over P2P,
// where the game would read any unknown packet as game data.
#pragma once
#include <windows.h>

#include <mutex>
#include <string>

#include "eos_min.h"

namespace dn {

class LobbyMarker {
public:
    static constexpr const char* kKey = "EDF6DN";  // value: protocol version
    static constexpr int64_t kVersion = 1;

    // Resolves the EOS functions it needs. Returns false (marker unavailable) when any is missing.
    bool init(HMODULE eos);

    // The local user is now in `lobbyId`: remember it and publish our marker. Runs on the EOS tick.
    void entered(EOS_HLobby lobby, const char* lobbyId, EOS_ProductUserId localUser);

    // True when `remote` carries the marker in our current lobby.
    bool hasMarker(EOS_ProductUserId remote) const;

private:
    PFN_EOS_Lobby_UpdateLobbyModification updateModification_ = nullptr;
    PFN_EOS_Lobby_UpdateLobby update_ = nullptr;
    PFN_EOS_LobbyModification_AddMemberAttribute addMemberAttribute_ = nullptr;
    PFN_EOS_LobbyModification_Release releaseModification_ = nullptr;
    PFN_EOS_Lobby_CopyLobbyDetailsHandle copyDetails_ = nullptr;
    PFN_EOS_LobbyDetails_CopyMemberAttributeByKey copyMemberAttribute_ = nullptr;
    PFN_EOS_LobbyDetails_Release releaseDetails_ = nullptr;
    PFN_EOS_Lobby_Attribute_Release releaseAttribute_ = nullptr;

    mutable std::mutex mu_;
    EOS_HLobby lobby_ = nullptr;
    std::string lobbyId_;
    EOS_ProductUserId localUser_ = nullptr;
};

}  // namespace dn
