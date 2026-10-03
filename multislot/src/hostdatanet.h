// Host data between machines (hostdata.h): what each member says in the lobby, what the menu tells the player,
// and getting the host's files.
//
// Every machine with host data on publishes kHostDataKey on its lobby member, and one that shares publishes the
// digest of its bundle under kHostDigestKey (syncmarker.h: the game never reads member attributes). A player in
// someone else's room compares the owner's digest with its own: the same files need nothing; different ones are
// offered in the menu (Accept=Ask: AcceptKey takes them) or taken at once (Always).
//
// The files travel over the P2P link the game already has to the host: the game's own socket, on kHostDataChannel
// (the game sends on channel 0 and reads any). EOS_P2P_ReceivePacket is wrapped next to EOS, before every other
// wrapper, and our packets never get past it. They only ever go to a member that published kHostDataKey (a player
// asks the host only once the host published it; the host only answers whoever asked), so a machine that would
// read them as game data never gets one.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "hostdata.h"
#include "packetfit.h"
#include "syncmarker.h"

namespace multislot {

constexpr const char* kHostDataKey = "EDF6CO_HD";     // this machine takes part, in this packet format
constexpr const char* kHostDigestKey = "EDF6CO_HDD";  // and shares the bundle with this SHA-256 (hex)
constexpr const char* kHostDataFormat = "1";
constexpr std::uint8_t kHostDataChannel = 0x48;

enum class HostAccept { Ask, Always, Never };

// What a lobby says about host data, for this machine.
struct HostDataRoom {
    bool inRoom = false;
    bool hosting = false;        // we own the lobby
    bool hostTakesPart = false;  // the owner publishes kHostDataKey in our format
    std::optional<hostdata::Digest> hostDigest;  // and shares the bundle with this digest
    std::string hostId;          // the owner's EOS_ProductUserId as text
};
HostDataRoom ReadHostDataRoom(const LobbyView& view);

// Where this machine stands with the host's files in its room.
enum class HostDataStage {
    None,         // not in a room, hosting it, or the host shares nothing
    Same,         // the host's files are ours
    HostHasNone,  // the host shares no files and we have some: nothing to take
    Differs,      // the host's files differ from ours, not taken
    Fetching,     // being fetched from the host
    Ready,        // here and checked; used from the next menu frame
    Using,        // the game reads them
    Failed,       // could not be fetched or checked; see the log
};
// The menu's words for a stage in a room ("" for none). keyName: the accept key ("F1", "" for none); percent: of a
// fetch; extra: our own files the host's leave as they are (Using).
std::wstring HostDataNotice(HostDataStage stage, HostAccept accept, const wchar_t* keyName, int percent, std::size_t extra);

// The transfer, without EOS: one machine's side. Tests wire two of them together.
class HostDataLink {
public:
    // Sends `packet` to `peer` (an EOS_ProductUserId as text); false when it cannot go now (it is tried again).
    using Send = std::function<bool(const std::string& peer, const std::vector<std::uint8_t>& packet)>;
    static constexpr std::size_t kPartsPerTick = 8;      // of all sends together: about 0.5 MB/s at 60 ticks
    static constexpr std::uint64_t kStallMs = 15000;     // no part for this long: ask again
    static constexpr int kMaxAsks = 3;                   // then give up
    static constexpr std::size_t kMaxServed = 8;         // players fetched from us at once; more wait for a re-ask

    explicit HostDataLink(Send send) : send_(std::move(send)) {}
    // The bundle we serve to whoever asks for it (nullptr: none).
    void Share(std::shared_ptr<const hostdata::Bundle> bundle);
    // Starts fetching `digest` from `host`; a fetch already running is dropped.
    void Fetch(const std::string& host, const hostdata::Digest& digest, std::uint64_t now);
    void Cancel();
    // A packet from `peer` that DecodePacket took.
    void Received(const std::string& peer, const hostdata::Packet& packet, std::uint64_t now);
    // Sends what is due: parts we serve, a fetch's question again after a stall.
    void Tick(std::uint64_t now);

    enum class Fetching { Idle, Running, Done, Failed };
    Fetching State() const { return state_; }
    int Percent() const;
    const std::string& Failure() const { return failure_; }
    // The fetched bundle, once Done (then Idle).
    std::vector<std::uint8_t> TakeBundle();
    std::size_t Serving() const { return served_.size(); }

private:
    struct Served {
        std::string peer;
        std::size_t next = 0;
    };
    void Fail(const std::string& why);
    void Ask(std::uint64_t now);

    Send send_;
    std::shared_ptr<const hostdata::Bundle> shared_;
    std::vector<Served> served_;
    Fetching state_ = Fetching::Idle;
    std::string host_;
    hostdata::Digest digest_{};
    std::unique_ptr<hostdata::Assembler> assembler_;
    std::vector<std::uint8_t> fetched_;
    std::string failure_;
    std::uint64_t lastProgress_ = 0;
    int asks_ = 0;
    bool asked_ = false;  // the question is out (a send that failed is tried again on the next tick)
};

struct HostDataSettings {
    bool share = true;
    HostAccept accept = HostAccept::Ask;
    const wchar_t* keyName = L"";  // the accept key, for the menu
    std::wstring gameFolder;       // the folder EDF6.exe is in
};

// At load: scans this machine's Mods, publishes, watches the lobby, and redirects EOS_P2P_ReceivePacket. Before
// InstallPacketFit and the net log (their wrappers go in front of this one), after InstallSyncMarker.
bool StartHostData(HMODULE game, ImportRedirect redirect, const HostDataSettings& settings);
// A menu frame (hostmode.h, RoomFeature): `pressed` the accept key went down. The text for the label in a room.
std::wstring HostDataMenuFrame(bool inRoom, bool pressed);

}  // namespace multislot
