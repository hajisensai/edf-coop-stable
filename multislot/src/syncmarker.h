#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "packetfit.h"

namespace multislot {

// Who in our lobby reads a split mission start message (packetfit.h), learnt without sending anything over P2P.
//
// A machine without the split cannot be sent either half of it. Its record reader 773740 never fails: 12B4660 reads
// the stub's 0xA0 tag as the number 0 and moves one byte, so every later record is read out of line, and
// MissionSync_Update writes the garbage player index it gets unchecked above zero (790878: `js` only, then
// [r15 + index * 0xD4 + 0x118]) - memory corruption on that machine. A side packet would reach its game as game
// data (the game's receive loop 12C8D84 queues whatever EOS hands it, any channel). Older MultiSlot builds share
// rooms with this one (same SEARCH_TYPE family, patches.h), and the 1.5.14 packages of 2026-09-30 already called
// themselves 1.5.14 without the split, so neither the version nor the room family can tell them apart.
//
// So every machine with the split publishes kSplitSyncKey on its own lobby member, where EDF.dll never looks (it
// imports no member-attribute function), and the host reads the members' attributes from the lobby details EOS
// keeps locally. A packet that carries a stub only goes to members seen with the marker (PacketFitSend).
constexpr const char* kSplitSyncKey = "EDF6MS_SPLITSYNC";  // value: the split format, kSplitSyncFormat
constexpr const char* kSplitSyncSeqKey = "EDF6MS_SEQ";      // bumped on every publish, so EOS sends it to everyone
constexpr std::int64_t kSplitSyncFormat = 1;

// What the lobby says about one member, read on the EOS tick.
struct LobbyMember {
    std::string id;     // EOS_ProductUserId as text
    bool marked;        // carries kSplitSyncKey >= kSplitSyncFormat
};

// The lobby state behind PeerReadsSplitSync, without EOS: tests drive it directly. Thread-safe.
class SplitSyncRoom {
public:
    // We are in a lobby now (a new one: nothing is known about its members).
    void Entered(const std::string& lobbyId);
    void Left();
    // The members the lobby details list now. A marker once seen stays for as long as we are in this room: our
    // copy of a lobby has been seen to lose other members' attributes, and members, for a while (EDF6DirectNet,
    // lobby_marker.h), and a member's build does not change while it is in the room. Returns true when someone new
    // is listed, who needs our marker published again (a member does not get attributes set before it joined
    // until they change - a host migrating to it would not know us).
    bool Observe(const std::vector<LobbyMember>& members);
    bool InLobby() const;
    std::string LobbyId() const;
    bool Marked(const std::string& id) const;
    // Members listed without the marker, for the log.
    std::vector<std::string> Unmarked() const;

private:
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::string lobby_;
    std::vector<std::string> members_;  // everyone listed since we entered
    std::vector<std::string> marked_;   // seen with the marker since we entered
    std::vector<std::string> refused_;  // logged as unable to read a split message (once each per room)

    friend bool PeerReadsSplitSync(const void* remote);
};

SplitSyncRoom& SplitSync();

// Whether `remote` (an EOS_ProductUserId) shows the marker, for the log when a split sync's records go to it (see
// SetSplitSyncReaders). A member without the marker is logged once per room. Thread-safe; asks EOS for the id's text, like the net log does on any thread.
bool PeerReadsSplitSync(const void* remote);

// --- text attributes of other features (hostdata), on the same beat ---

// Our lobby as the beat read it: who owns it, and the watched attributes of each member (absent: not set).
struct LobbyView {
    std::string lobbyId;
    std::string self;   // EOS_ProductUserId as text
    std::string owner;  // empty when the lobby details do not say
    struct Member {
        std::string id;
        std::map<std::string, std::string, std::less<>> texts;
    };
    std::vector<Member> members;
};
using LobbyObserver = std::function<void(const LobbyView& view)>;
// Read `keys` of every member on each beat (once a second while we are in a lobby) and hand them to `observer`, on
// the EOS tick, outside every lock of this file. Leaving a lobby hands it an empty view.
void WatchMemberTexts(std::vector<std::string> keys, LobbyObserver observer);
// Our value of `key` (at most 1000 characters), published on our lobby member on the next beat and in every lobby
// we enter afterwards. Any thread. Never empty: EOS refuses an empty text with EOS_InvalidParameters, and with it the
// whole update - our split marker and every other text - so an empty value is logged and not published.
void PublishMemberText(const std::string& key, const std::string& value);
// Called after every EOS tick with the platform handle, outside every lock of this file (hostdatanet.h: P2P).
void ListenToTicks(std::function<void(void* platform)> listener);

// Redirects EOS_Lobby_CreateLobby, EOS_Lobby_JoinLobby, EOS_Lobby_LeaveLobby, EOS_Lobby_DestroyLobby and
// EOS_Platform_Tick (the tick publishes and reads attributes, on the thread EOS wants its calls on). `splitReader`:
// this machine reads a split start message (both packetfit imports are ours), so kSplitSyncKey is published; without
// it only the text attributes are. Returns false when the EOS functions it needs are missing or an import could
// not be redirected: then nobody is known to read a split message and a host never sends one (packetfit.h).
bool InstallSyncMarker(HMODULE game, ImportRedirect redirect, bool splitReader);

}  // namespace multislot
