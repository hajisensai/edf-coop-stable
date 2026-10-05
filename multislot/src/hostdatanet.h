// Weapon and vehicle files between machines (hostdata.h): the room host's modified files, and weapon pages.
//
// Weapon pages: Mods\Variants\<page>\WEAPON\*.SGO (and OBJECT\V*/VEHICLE*.SGO) hold modified weapons that take the
// place of the game's own weapons of the same file name - a mod weapon on an existing weapon's slot, so the weapon
// table, the save and every other player's list stay as they are. A player picks one page with WeaponPageKey (or none);
// it is used from the next mission on, offline too.
//
// In a room every machine uses the same files. Each brings what it has: the owner its Mods (kHostDigestKey) and
// every member the page it picked (kPageDigestKey), published on its own lobby member (syncmarker.h: the game never
// reads member attributes). The room's files are those in a fixed order (PlanRoomSources): the owner's Mods, the
// owner's page, then the other members' pages as the lobby lists them; a file two of them have is the first one's.
// Every machine works the same order out of the same lobby, fetches what it does not have (unless Accept=Never) and
// is pointed at the result between missions; the menu says so at the front. Accept=Auto uses what arrived at once;
// Ask (the default) first asks on a menu screen, naming every file and what either answer risks
// (hostdataprompt.h). The answer holds for those bundles in this lobby; AcceptKey switches all of them later.
//
// The files travel over the P2P link the game already has to every member: the game's own socket, on
// kHostDataChannel (the game sends on channel 0 and reads any). EOS_P2P_ReceivePacket is wrapped next to EOS, before
// every other wrapper, and our packets never get past it. They only ever go to a member that published kHostDataKey
// (a player asks only a member that published it; a member only answers whoever asked), so a machine that would
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
constexpr const char* kHostDigestKey = "EDF6CO_HDD";  // the bundle of its Mods (hex SHA-256), read from the owner
constexpr const char* kPageDigestKey = "EDF6CO_HDP";  // the bundle of the weapon page it uses, kPageNone for none
// No page. Not "": EOS refuses an empty text (syncmarker.h, PublishMemberText). Anything that is no hex digest reads
// as no page, so members that published "" before keep reading the same.
constexpr const char* kPageNone = "none";
constexpr const char* kHostDataFormat = "1";
constexpr std::uint8_t kHostDataChannel = 0x48;

enum class HostAccept { Ask, Auto, Never };

// One bundle the room's files come from.
struct RoomSource {
    std::string member;  // who brings it (EOS_ProductUserId as text)
    hostdata::Digest digest{};
    bool mods = false;   // the owner's Mods; otherwise a member's page
    bool operator==(const RoomSource&) const = default;
};
// The room's sources in the order that decides a file two of them have: the owner's Mods, the owner's page, then
// the other members' pages in the lobby's order. Only members that publish kHostDataKey in our format; a digest
// that is not one is no source. selfMods and selfPage stand for what this machine brings (the lobby may not show
// it yet): its Mods when it owns the lobby and shares them, and its page.
std::vector<RoomSource> PlanRoomSources(const LobbyView& view, const std::optional<hostdata::Digest>& selfMods,
                                        const std::optional<hostdata::Digest>& selfPage);

// A source's files as this machine has them: their paths and the folder the game reads them from ("" for files
// the game reads anyway: this machine's own Mods).
struct SourceFiles {
    std::vector<std::string> paths;
    std::wstring folder;
};
struct RoomOverlay {
    hostdata::Overlay overlay;      // every path a source with a folder won, pointed at it
    std::vector<std::size_t> lost;  // per source: its files an earlier source has too
};
// The room's files: each path is the first source's that has it. A null source is one this machine goes without
// (not here, not approved, or failed); it takes no place.
RoomOverlay MergeSources(const std::vector<const SourceFiles*>& sources);

// What the menu says about weapons, at the front of the label.
struct WeaponsView {
    const wchar_t* pageKey = L"";    // "F6"
    const wchar_t* acceptKey = L"";  // "F1"
    std::size_t pages = 0;           // pages under Mods\Variants
    std::wstring page;               // the one picked, "" for none
    bool inRoom = false;
    std::size_t remote = 0;          // sources in the room this machine does not have of its own
    HostAccept accept = HostAccept::Ask;
    bool wanted = false;             // the room's files are taken (Ask: once the player said so)
    int percent = -1;                // fetching them: 0..100; -1 not
    std::size_t failed = 0;          // sources that could not be fetched
    bool using_ = false;             // the game is pointed at the room's files
    std::size_t lost = 0;            // files of this machine's page another source has first
};
std::wstring WeaponsNotice(const WeaponsView& view);

// The transfer, without EOS: one machine's side. Tests wire several of them together.
class HostDataLink {
public:
    // Sends `packet` to `peer` (an EOS_ProductUserId as text); false when it cannot go now (it is tried again).
    using Send = std::function<bool(const std::string& peer, const std::vector<std::uint8_t>& packet)>;
    static constexpr std::size_t kPartsPerTick = 8;      // of all sends together: about 0.5 MB/s at 60 ticks
    static constexpr std::uint64_t kStallMs = 15000;     // no part for this long: ask again
    static constexpr int kMaxAsks = 3;                   // then give up
    static constexpr std::size_t kMaxServed = 8;         // bundles sent at once; more wait for a re-ask

    explicit HostDataLink(Send send) : send_(std::move(send)) {}
    // The bundles we serve to whoever asks for one (none: an empty list).
    void Share(std::vector<std::shared_ptr<const hostdata::Bundle>> bundles);
    void Share(std::shared_ptr<const hostdata::Bundle> bundle);
    // Starts fetching `digest` from `member`; a fetch already running is dropped.
    void Fetch(const std::string& member, const hostdata::Digest& digest, std::uint64_t now);
    void Cancel();
    // A packet from `peer` that DecodePacket took.
    void Received(const std::string& peer, const hostdata::Packet& packet, std::uint64_t now);
    // Sends what is due: parts we serve, a fetch's question again after a stall.
    void Tick(std::uint64_t now);

    enum class Fetching { Idle, Running, Done, Failed };
    Fetching State() const { return state_; }
    int Percent() const;
    const std::string& Failure() const { return failure_; }
    // What the running (or last) fetch asks for, and whom.
    const std::string& FetchMember() const { return member_; }
    const hostdata::Digest& FetchDigest() const { return digest_; }
    // The fetched bundle, once Done (then Idle).
    std::vector<std::uint8_t> TakeBundle();
    std::size_t Serving() const { return served_.size(); }

private:
    struct Served {
        std::string peer;
        std::shared_ptr<const hostdata::Bundle> bundle;
        std::size_t next = 0;
    };
    void Fail(const std::string& why);
    void Ask(std::uint64_t now);

    Send send_;
    std::vector<std::shared_ptr<const hostdata::Bundle>> shared_;
    std::vector<Served> served_;
    Fetching state_ = Fetching::Idle;
    std::string member_;
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
    const wchar_t* keyName = L"";      // the accept key, for the menu
    const wchar_t* pageKeyName = L"";  // the page key, for the menu
    std::wstring page;                 // the page picked last time ([HostData] Page), "" for none
    std::wstring iniPath;              // where the page picked is saved, "" not saved
    std::wstring gameFolder;           // the folder EDF6.exe is in
};

// At load: scans this machine's Mods and weapon pages, publishes, watches the lobby, and redirects
// EOS_P2P_ReceivePacket. Before InstallPacketFit and the net log (their wrappers go in front of this one), after
// InstallSyncMarker.
bool StartHostData(HMODULE game, ImportRedirect redirect, const HostDataSettings& settings);
// A menu frame (hostmode.h, WeaponFeature): the accept key went down in a room, the page key went down. The text
// for the front of the label.
std::wstring HostDataMenuFrame(bool inRoom, bool acceptPressed, bool pagePressed);

}  // namespace multislot
