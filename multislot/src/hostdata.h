// Host data: the room host's weapon and vehicle data, used by the players in its room who accept it.
//
// A player's weapon and vehicle data are what EDFModLoader loads from Mods\ in place of the game's (its
// Redirect, ModLoader.ini). Players in one room with different data see different weapons: the host's
// modified gun fires as the original on everyone else's screen. Here the host offers its files, and a player
// who accepts them loads those instead, for that room only.
//
// What may travel is data only, and only some of it (SharedPath):
//   WEAPON\<name>.SGO       a weapon or a vehicle's gun; not WEAPONTABLE.SGO (the list of weapons: new IDs
//                           there would sit in the save, which crashes the menu once the files are gone) and
//                           not WEAPONTEXT.*.SGO (names, loaded once at start)
//   OBJECT\V<nnn>*.SGO      a vehicle; also OBJECT\VEHICLE<nnn>*.SGO (VEHICLE404_BIGTANK_AI.SGO)
// Never a DLL, Patches\*.txt (machine code) or anything else. Every file starts with an SGO magic, is at most
// kMaxFileBytes and there are at most kMaxFiles of them, kMaxTotalBytes in all.
//
// The files travel as one bundle (MakeBundle), named by its SHA-256. The host publishes that digest on its
// own lobby member (syncmarker.h), which only the host can write, so a player checks what arrived against what
// the host said - whoever sent the bytes. A bundle is parsed only after its digest matched, and then still
// checked entry by entry (ParseBundle).
//
// A player keeps an accepted bundle in its own folder (Mods\Plugins\EDF6Coop.hostdata\<digest>), outside the
// folders EDFModLoader reads, and the game is pointed at it file by file while the overlay is on (OverlayPath).
// The overlay lives in memory: leaving the room, quitting or a crash all bring back the player's own data.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace multislot::hostdata {

using Digest = std::array<std::uint8_t, 32>;

constexpr std::size_t kMaxFiles = 128;
constexpr std::size_t kMaxFileBytes = 256 * 1024;
constexpr std::size_t kMaxTotalBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxPathChars = 64;

// One shared file: its path under Mods ("WEAPON/AWEAPON346.SGO": upper case, forward slashes) and its bytes.
struct DataFile {
    std::string path;
    std::vector<std::uint8_t> bytes;
};

// `path` (as DataFile::path) is one of the shared kinds above.
bool SharedPath(std::string_view path);
// `path` as a DataFile path: '\' to '/', upper case. Empty when it cannot be one (not ASCII, too long).
std::string NormalPath(std::string_view path);
// The bytes start with an SGO magic.
bool LooksLikeSgo(const std::vector<std::uint8_t>& bytes);

struct Bundle {
    std::vector<std::uint8_t> bytes;  // what travels
    Digest digest{};                  // SHA-256 of bytes: the name the host publishes
    std::size_t files = 0;
};

// The bundle of `files` (sorted by path, so the same files always make the same bundle). nullopt with `why`
// when a file is not shareable or the limits are exceeded. No files make an empty bundle, which is valid.
std::optional<Bundle> MakeBundle(std::vector<DataFile> files, std::string* why);
// The files of a bundle, every one checked as MakeBundle checks them; nullopt with `why` otherwise.
std::optional<std::vector<DataFile>> ParseBundle(const std::vector<std::uint8_t>& bytes, std::string* why);

// What a player's Mods holds of the shared kinds (`mods`: the Mods folder). Files that cannot be shared are
// skipped and named in `skipped` with the reason.
std::vector<DataFile> ScanMods(const std::wstring& mods, std::vector<std::string>* skipped);

std::optional<Digest> Sha256(const std::vector<std::uint8_t>& bytes);
std::string DigestHex(const Digest& digest);
std::optional<Digest> DigestFromHex(std::string_view hex);

// --- the overlay ---

// The paths the game opens a file by (the game's CRI file system, after EDFModLoader): "/cri_bind/WEAPON/X.SGO"
// for one in the game's archive, "./Mods/WEAPON/X.SGO" for one EDFModLoader found under Mods. The DataFile path
// either names, or empty for any other path.
std::string OpenedDataPath(const wchar_t* gamePath);

// The host's files a player uses: their DataFile paths, sorted, and the folder they are in, as the game's file
// system takes it ("./Mods/Plugins/EDF6Coop.hostdata/<digest>/").
struct Overlay {
    std::vector<std::string> paths;
    std::wstring folder;
};
// Where the game should read `gamePath` from instead, or nullopt to read it as it asked.
std::optional<std::wstring> OverlayPath(const wchar_t* gamePath, const Overlay& overlay);

// --- the transfer (EOS P2P, a socket of our own; hostdatanet.cpp) ---

constexpr std::size_t kMaxPacket = 1170;  // EOS_P2P_MAX_PACKET_SIZE
constexpr std::size_t kPartBytes = 1024;

enum class PacketType : std::uint8_t { Get = 1, Part = 2, None = 3 };
struct Packet {
    PacketType type = PacketType::Get;
    Digest digest{};
    std::uint32_t total = 0;   // Part: the bundle's size
    std::uint32_t offset = 0;  // Part: where these bytes go
    std::vector<std::uint8_t> data;
};
// A player asks for the bundle `digest`.
std::vector<std::uint8_t> EncodeGet(const Digest& digest);
// The host has no bundle `digest` (any more).
std::vector<std::uint8_t> EncodeNone(const Digest& digest);
// Part number `index` of `bundle`; empty past its end.
std::vector<std::uint8_t> EncodePart(const Bundle& bundle, std::size_t index);
std::size_t PartCount(std::size_t bundleBytes);
std::optional<Packet> DecodePacket(const std::uint8_t* data, std::size_t size);

// A bundle arriving in parts. Parts of another bundle, out of range or of a wrong size are ignored.
class Assembler {
public:
    explicit Assembler(const Digest& digest) : digest_(digest) {}
    // False when the part was ignored.
    bool Add(const Packet& part);
    bool Complete() const { return started_ && received_ == parts_.size(); }
    // Received parts, of how many (0 until the first part said).
    std::size_t Received() const { return received_; }
    std::size_t Parts() const { return parts_.size(); }
    // The bundle, once complete and only when it hashes to the digest asked for.
    std::optional<std::vector<std::uint8_t>> Take(std::string* why);

private:
    Digest digest_;
    bool started_ = false;
    std::vector<std::uint8_t> bytes_;
    std::vector<bool> parts_;
    std::size_t received_ = 0;
};

}  // namespace multislot::hostdata
