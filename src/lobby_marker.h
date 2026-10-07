// Shares plugin facts through our own lobby member, where the game cannot see them.
//
// On joining or creating a lobby the plugin publishes member attributes on its own member: a marker
// saying it runs the plugin, the commitment of its direct-link identity (a direct-link host lets a
// player in only as the member whose identity it proves, and a joiner accepts as host only the room
// owner proving its identity) and, for a direct-link host, the address
// to connect to. EDF.dll never
// reads member attributes (it imports none of those functions), while every other plugin reads them
// from the lobby details EOS already keeps locally. That answers "does this peer run the plugin" and
// "where is the host" without sending anything over P2P, where the game would read any unknown
// packet as game data.
#pragma once
#include <windows.h>

#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "eos_min.h"

namespace dn {

class LobbyMarker {
public:
    static constexpr const char* kKey = "EDF6DN";           // value: protocol version
    static constexpr const char* kAddressKey = "EDF6DN_ADDR";  // value: space-separated host addresses
    static constexpr const char* kSeqKey = "EDF6DN_SEQ";       // value: bumped on every publish
    static constexpr const char* kIdentityKey = "EDF6DN_ID";   // value: Identity::commitment() (see auth.h)
    static constexpr int64_t kVersion = 1;

    // Resolves the EOS functions it needs. Returns false (unavailable) when any is missing.
    bool init(HMODULE eos);

    // The local user is now in `lobbyId` (as its owner when it created it). Thread-safe.
    void entered(EOS_HLobby lobby, const char* lobbyId, EOS_ProductUserId localUser, bool owner);
    // The local user left its lobby. Thread-safe.
    void left();
    // The local user became the owner of its lobby (the previous owner left). Thread-safe.
    void promoted();
    // Someone else joined our lobby: publish our attributes again. A joining player does not get
    // member attributes set before it joined until they change, so without this it would never see
    // our marker or the host's address. Thread-safe.
    void memberJoined();
    // `member` left the room (or was removed): forget what we learnt about it. Thread-safe.
    void memberGone(const std::string& member);
    // The direct-link address this player hosts on ("" = not hosting). Thread-safe.
    void setAddress(const std::string& address);
    // The commitment of this player's direct-link identity, published by every player. Thread-safe.
    void setIdentity(const std::string& commitment);
    // What we publish: the address we host on ("" when not hosting) and our identity commitment. Thread-safe.
    std::string ownAddress() const;
    std::string ownIdentity() const;

    // Publishes our attributes when something changed. EOS calls: run on the EOS tick only.
    void tick();

    bool inLobby() const;
    bool isOwner() const;
    // What we learnt about a member stays true for as long as it is in the room: our copy of the
    // lobby has been seen to lose other members' attributes (a joiner saw the host's marker and
    // address vanish seconds after joining), and a member's plugin or a host's address never change.
    // True when `remote` carries the marker in our current lobby, or did since it joined.
    bool hasMarker(EOS_ProductUserId remote);
    // The lobby owner and the address it advertises ("" when none). Owner may be null.
    std::string ownerAddress(EOS_ProductUserId* owner);
    // Diagnostics: what our copy of the lobby shows about its owner.
    std::string describeOwner() const;
    // The direct-link identity commitments the members of our lobby published (EOS id -> commitment),
    // kept like hasMarker() for as long as the member is in the room. Members without the plugin (or
    // with 0.3.6 and older) are absent. EOS calls: run on the EOS tick only.
    std::map<std::string, std::string> memberIdentities();
    // The members our copy of the lobby lists now (EOS ids), or nothing when we have no copy (`known` false).
    // EOS calls: run on the EOS tick only.
    std::vector<std::string> members(bool* known);

private:
    // Copies member attribute `key` of `member` as a string ("" when absent). Caller holds mu_.
    bool readAttribute(EOS_HLobbyDetails details, EOS_ProductUserId member, const char* key, std::string* value) const;
    EOS_HLobbyDetails copyDetailsLocked() const;
    std::string idString(EOS_ProductUserId id) const;

    PFN_EOS_Lobby_UpdateLobbyModification updateModification_ = nullptr;
    PFN_EOS_Lobby_UpdateLobby update_ = nullptr;
    PFN_EOS_LobbyModification_AddMemberAttribute addMemberAttribute_ = nullptr;
    PFN_EOS_LobbyModification_Release releaseModification_ = nullptr;
    PFN_EOS_Lobby_CopyLobbyDetailsHandle copyDetails_ = nullptr;
    PFN_EOS_LobbyDetails_CopyMemberAttributeByKey copyMemberAttribute_ = nullptr;
    PFN_EOS_LobbyDetails_GetLobbyOwner getOwner_ = nullptr;
    PFN_EOS_LobbyDetails_GetMemberAttributeCount getMemberAttributeCount_ = nullptr;  // optional
    PFN_EOS_LobbyDetails_GetMemberCount getMemberCount_ = nullptr;                    // optional
    PFN_EOS_LobbyDetails_GetMemberByIndex getMemberByIndex_ = nullptr;                // optional
    PFN_EOS_LobbyDetails_CopyMemberAttributeByIndex copyMemberAttributeByIndex_ = nullptr;  // optional
    PFN_EOS_ProductUserId_ToString idToString_ = nullptr;
    PFN_EOS_LobbyDetails_Release releaseDetails_ = nullptr;
    PFN_EOS_Lobby_Attribute_Release releaseAttribute_ = nullptr;
    bool ready_ = false;

    mutable std::mutex mu_;
    EOS_HLobby lobby_ = nullptr;
    std::string lobbyId_;
    EOS_ProductUserId localUser_ = nullptr;
    bool owner_ = false;
    std::string address_;
    std::string identity_;  // our commitment, published as kIdentityKey
    bool dirty_ = false;  // our attributes differ from what the lobby has
    int64_t seq_ = 0;     // EDF6DN_SEQ: makes every publish a real change that EOS sends to everyone
    std::unordered_set<std::string> marked_;  // members seen with the marker since they joined
    std::map<std::string, std::string> identities_;  // member -> identity commitment seen since it joined
    std::string knownOwner_, knownOwnerAddress_;  // the last address seen advertised, and by whom
};

}  // namespace dn
