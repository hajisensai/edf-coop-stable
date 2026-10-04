// Host data (hostdata.h) without a game: which files may travel, the bundle they travel in, the packets that
// carry it, and where the game is pointed while a player uses the host's files.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/hostdata.h"

using namespace multislot::hostdata;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

std::vector<std::uint8_t> Sgo(std::size_t size, std::uint8_t fill) {
    std::vector<std::uint8_t> bytes(size, fill);
    const char magic[4] = {'S', 'G', 'O', 0};
    std::copy(magic, magic + 4, bytes.begin());
    return bytes;
}

DataFile File(const char* path, std::size_t size = 64, std::uint8_t fill = 7) { return DataFile{path, Sgo(size, fill)}; }

void Paths() {
    Check(SharedPath("WEAPON/AWEAPON346.SGO"), "a weapon is shared");
    Check(SharedPath("WEAPON/V_GUN_01.SGO"), "underscores in a weapon name");
    Check(!SharedPath("WEAPON/WEAPONTABLE.SGO"), "the weapon table never travels");
    Check(!SharedPath("WEAPON/WEAPONTEXT.EN.SGO"), "weapon names never travel");
    Check(!SharedPath("WEAPON/SUB/X.SGO"), "no sub folders");
    for (const char* device : {"WEAPON/NUL.SGO", "WEAPON/CON.SGO", "WEAPON/PRN.SGO", "WEAPON/AUX.SGO", "WEAPON/COM1.SGO",
                               "WEAPON/LPT9.SGO", "WEAPON/COM0.SGO"})
        Check(!SharedPath(device), "a device name is never a file");
    Check(SharedPath("WEAPON/COM10.SGO") && SharedPath("WEAPON/NULL.SGO") && SharedPath("WEAPON/CONSOLE.SGO"),
          "names that only start like a device are files");
    Check(!SharedPath("WEAPON/../EDF.DLL"), "no way out of the folder");
    Check(!SharedPath("WEAPON/X.DLL"), "no DLLs");
    Check(!SharedPath("WEAPON/.SGO"), "a name is needed");
    Check(!SharedPath("weapon/aweapon346.sgo"), "paths are upper case once normal");
    Check(SharedPath("OBJECT/V401.SGO"), "a vehicle is shared");
    Check(SharedPath("OBJECT/V401_TANK.SGO"), "a vehicle with a suffix");
    Check(!SharedPath("OBJECT/E101.SGO"), "enemies do not travel");
    Check(!SharedPath("OBJECT/VA01.SGO"), "a vehicle number is three digits");
    Check(SharedPath("OBJECT/VEHICLE404_BIGTANK_AI.SGO") && SharedPath("OBJECT/VEHICLE404.SGO"), "VEHICLE<nnn> too");
    Check(!SharedPath("OBJECT/VEHICLE40.SGO") && !SharedPath("OBJECT/VEHICLEX404.SGO") && !SharedPath("OBJECT/VEHICLE.SGO"),
          "VEHICLE needs its three digits right after it");
    Check(!SharedPath("OBJECT/EDF6VC_JET.SGO") && !SharedPath("OBJECT/VEHICLE404.MRAB"), "other objects do not travel");
    Check(!SharedPath("PATCHES/X.TXT"), "patches never travel");
    Check(!SharedPath("PLUGINS/X.DLL"), "plugins never travel");
    Check(!SharedPath(std::string("WEAPON/") + std::string(60, 'A') + ".SGO"), "paths are short");

    Check(NormalPath("weapon\\aweapon346.sgo") == "WEAPON/AWEAPON346.SGO", "normal path");
    Check(NormalPath("").empty(), "empty path");
    Check(NormalPath("WEAPON/\xC3\xA9.SGO").empty(), "non-ASCII path");

    Check(LooksLikeSgo(Sgo(16, 0)), "SGO magic");
    std::vector<std::uint8_t> dsgo(32, 0);
    dsgo[0] = 'D', dsgo[1] = 'S', dsgo[2] = 'G', dsgo[3] = 'O';
    Check(LooksLikeSgo(dsgo), "DSGO magic");
    Check(!LooksLikeSgo(std::vector<std::uint8_t>(32, 'M')), "no magic");
    Check(!LooksLikeSgo(Sgo(8, 0)), "too short to be one");
}

void Bundles() {
    std::string why;
    const auto a = MakeBundle({File("WEAPON/B.SGO"), File("WEAPON/A.SGO", 100, 3), File("OBJECT/V401.SGO")}, &why);
    Check(a.has_value(), "three files make a bundle");
    if (!a) return;
    Check(a->files == 3, "file count");
    const auto b = MakeBundle({File("OBJECT/V401.SGO"), File("WEAPON/A.SGO", 100, 3), File("WEAPON/B.SGO")}, &why);
    Check(b && b->digest == a->digest && b->bytes == a->bytes, "the same files make the same bundle in any order");
    const auto c = MakeBundle({File("WEAPON/B.SGO"), File("WEAPON/A.SGO", 100, 4), File("OBJECT/V401.SGO")}, &why);
    Check(c && c->digest != a->digest, "one changed byte changes the digest");

    const auto files = ParseBundle(a->bytes, &why);
    Check(files && files->size() == 3, "a bundle parses");
    if (files) {
        Check((*files)[0].path == "OBJECT/V401.SGO" && (*files)[1].path == "WEAPON/A.SGO", "sorted by path");
        Check((*files)[1].bytes == Sgo(100, 3), "contents survive");
    }
    const auto empty = MakeBundle({}, &why);
    Check(empty && ParseBundle(empty->bytes, &why) && ParseBundle(empty->bytes, &why)->empty(), "no files is valid");

    Check(!MakeBundle({File("WEAPON/WEAPONTABLE.SGO")}, &why), "the table is refused");
    Check(!MakeBundle({DataFile{"WEAPON/A.SGO", std::vector<std::uint8_t>(64, 'M')}}, &why), "not an SGO");
    Check(!MakeBundle({File("WEAPON/A.SGO", kMaxFileBytes + 1)}, &why), "a file too large");
    Check(!MakeBundle({File("WEAPON/A.SGO"), File("WEAPON/A.SGO")}, &why), "a file twice");
    std::vector<DataFile> many;
    for (std::size_t i = 0; i <= kMaxFiles; ++i) many.push_back(File(("WEAPON/W" + std::to_string(i) + ".SGO").c_str()));
    Check(!MakeBundle(many, &why), "too many files");
    std::vector<DataFile> big;
    for (int i = 0; i < 17; ++i) big.push_back(File(("WEAPON/W" + std::to_string(i) + ".SGO").c_str(), kMaxFileBytes));
    Check(!MakeBundle(big, &why), "too much in all");

    // Whatever a peer sends is parsed with every check again.
    std::vector<std::uint8_t> cut(a->bytes.begin(), a->bytes.end() - 1);
    Check(!ParseBundle(cut, &why), "cut short");
    std::vector<std::uint8_t> extra = a->bytes;
    extra.push_back(0);
    Check(!ParseBundle(extra, &why), "bytes after the end");
    std::vector<std::uint8_t> renamed = a->bytes;
    const auto at = std::search(renamed.begin(), renamed.end(), "OBJECT/V401.SGO", "OBJECT/V401.SGO" + 15);
    Check(at != renamed.end(), "path in the bundle");
    if (at != renamed.end()) {
        std::copy_n("OBJECT/V401.DLL", 15, at);
        Check(!ParseBundle(renamed, &why), "a forbidden path inside a bundle");
    }
    std::vector<std::uint8_t> huge = a->bytes;
    huge[5] = 0xff, huge[6] = 0xff;
    Check(!ParseBundle(huge, &why), "a count past the limit");
    Check(!ParseBundle({}, &why), "nothing");
}

void Digests() {
    Digest d{};
    for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<std::uint8_t>(i * 9);
    const std::string hex = DigestHex(d);
    Check(hex.size() == 64 && hex.substr(0, 4) == "0009", "hex");
    Check(DigestFromHex(hex) == d, "hex round trip");
    Check(!DigestFromHex(hex.substr(1)), "short hex");
    Check(!DigestFromHex(std::string(64, 'G')), "not hex");
    Check(!DigestFromHex(std::string(64, 'A')), "upper case is not what we publish");
    const auto known = Sha256({'a', 'b', 'c'});
    Check(known && DigestHex(*known) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");
}

void Overlays() {
    Check(OpenedDataPath(L"/cri_bind/WEAPON/AWEAPON346.SGO") == "WEAPON/AWEAPON346.SGO", "an archive weapon");
    Check(OpenedDataPath(L"./Mods/WEAPON/aweapon346.sgo") == "WEAPON/AWEAPON346.SGO", "a Mods weapon");
    Check(OpenedDataPath(L"/cri_bind/WEAPON/WEAPONTABLE.SGO").empty(), "the table is never redirected");
    Check(OpenedDataPath(L"/cri_bind/MISSION/M001.SGO").empty(), "missions are not ours");
    Check(OpenedDataPath(L"./Mods/Plugins/EDF6Coop.hostdata/x/WEAPON/A.SGO").empty(), "our own files are not ours");
    Check(OpenedDataPath(nullptr).empty(), "no path");

    Overlay overlay{{"OBJECT/V401.SGO", "WEAPON/A.SGO"}, L"./Mods/Plugins/EDF6Coop.hostdata/ab/"};
    const auto to = OverlayPath(L"/cri_bind/WEAPON/A.SGO", overlay);
    Check(to && *to == L"./Mods/Plugins/EDF6Coop.hostdata/ab/WEAPON/A.SGO", "redirected to the host's file");
    Check(OverlayPath(L"./Mods/OBJECT/v401.sgo", overlay).has_value(), "the player's own mod gives way too");
    Check(!OverlayPath(L"/cri_bind/WEAPON/B.SGO", overlay), "a file the host does not change");
    Check(!OverlayPath(L"/cri_bind/WEAPON/A.SGO", Overlay{}), "no overlay");
}

void Packets() {
    std::vector<DataFile> files;
    for (int i = 0; i < 5; ++i) files.push_back(File(("WEAPON/W" + std::to_string(i) + ".SGO").c_str(), 700, static_cast<std::uint8_t>(i)));
    std::string why;
    const auto bundle = MakeBundle(files, &why);
    Check(bundle.has_value(), "a bundle to send");
    if (!bundle) return;

    const auto get = EncodeGet(bundle->digest);
    const auto decoded = DecodePacket(get.data(), get.size());
    Check(decoded && decoded->type == PacketType::Get && decoded->digest == bundle->digest, "GET");
    const auto none = EncodeNone(bundle->digest);
    Check(DecodePacket(none.data(), none.size())->type == PacketType::None, "NONE");
    Check(!DecodePacket(get.data(), get.size() - 1), "a short GET");
    std::vector<std::uint8_t> game(40, 0x11);
    Check(!DecodePacket(game.data(), game.size()), "not ours");

    const std::size_t parts = PartCount(bundle->bytes.size());
    Check(parts > 3, "several parts");
    Check(EncodePart(*bundle, parts).empty(), "no part past the end");
    std::vector<std::vector<std::uint8_t>> sent;
    for (std::size_t i = 0; i < parts; ++i) {
        sent.push_back(EncodePart(*bundle, i));
        Check(sent.back().size() <= kMaxPacket, "a part fits one packet");
    }

    // Out of order, twice over, with a stranger's part in between.
    Assembler assembler(bundle->digest);
    Digest other = bundle->digest;
    other[0] ^= 1;
    Packet stranger = *DecodePacket(sent[0].data(), sent[0].size());
    stranger.digest = other;
    Check(!assembler.Add(stranger), "another bundle's part");
    for (std::size_t i = parts; i-- > 0;) Check(assembler.Add(*DecodePacket(sent[i].data(), sent[i].size())), "part");
    Check(assembler.Add(*DecodePacket(sent[1].data(), sent[1].size())), "a repeat");
    Check(assembler.Received() == parts && assembler.Complete(), "complete");
    const auto got = assembler.Take(&why);
    Check(got && *got == bundle->bytes, "the bundle arrives");

    // Right digest in the header, wrong bytes: caught by the hash.
    Assembler forged(bundle->digest);
    for (std::size_t i = 0; i < parts; ++i) {
        Packet p = *DecodePacket(sent[i].data(), sent[i].size());
        if (i == 2) p.data[10] ^= 0xff;
        forged.Add(p);
    }
    Check(forged.Complete() && !forged.Take(&why), "forged bytes are refused");

    Assembler odd(bundle->digest);
    Packet p = *DecodePacket(sent[0].data(), sent[0].size());
    p.offset = 3;
    Check(!odd.Add(p), "an offset off the grid");
    p = *DecodePacket(sent[0].data(), sent[0].size());
    p.data.pop_back();
    Check(!odd.Add(p), "a part of a wrong size");
    p = *DecodePacket(sent[0].data(), sent[0].size());
    p.total = 0x7fffffff;
    Check(!odd.Add(p), "an absurd total");
    Check(!odd.Complete() && !odd.Take(&why), "nothing taken early");
}

void Scan(const char* scratch) {
    const std::string folder(scratch);
    const std::wstring root = std::wstring(folder.begin(), folder.end()) + L"\\Mods";
    CreateDirectoryW(std::wstring(folder.begin(), folder.end()).c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\WEAPON").c_str(), nullptr);
    CreateDirectoryW((root + L"\\OBJECT").c_str(), nullptr);
    const auto put = [&root](const wchar_t* name, const std::vector<std::uint8_t>& bytes) {
        const HANDLE f = CreateFileW((root + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0;
        WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(f);
    };
    put(L"WEAPON\\aweapon1.sgo", Sgo(64, 1));
    put(L"WEAPON\\WEAPONTABLE.SGO", Sgo(64, 2));
    put(L"WEAPON\\WEAPONTEXT.EN.SGO", Sgo(64, 3));
    put(L"WEAPON\\BROKEN.SGO", std::vector<std::uint8_t>(64, 'M'));
    put(L"OBJECT\\V401.SGO", Sgo(64, 4));
    put(L"OBJECT\\E101.SGO", Sgo(64, 5));
    put(L"WEAPON\\HUGE.SGO", Sgo(kMaxFileBytes + 1, 6));
    std::vector<std::string> skipped;
    const auto files = ScanMods(root, &skipped);
    Check(files.size() == 2, "two shareable files");
    if (files.size() == 2) {
        Check(files[0].path == "OBJECT/V401.SGO" && files[1].path == "WEAPON/AWEAPON1.SGO", "scanned paths");
        Check(files[1].bytes == Sgo(64, 1), "scanned bytes");
    }
    Check(skipped.size() == 4, "table, names, the broken and the too large file are skipped and said");
    Check(std::any_of(skipped.begin(), skipped.end(),
                      [](const std::string& line) { return line.find("HUGE.SGO is larger than") != std::string::npos; }),
          "a file over the size limit says so");
    Check(ScanMods(root + L"\\nowhere", nullptr).empty(), "no Mods folder");
}

}  // namespace

int main(int argc, char** argv) {
    Paths();
    Bundles();
    Digests();
    Overlays();
    Packets();
    if (argc > 1) Scan(argv[1]);
    std::printf(failures ? "%d failures\n" : "hostdata: all passed\n", failures);
    return failures ? 1 : 0;
}
