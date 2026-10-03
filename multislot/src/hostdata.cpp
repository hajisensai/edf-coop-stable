#include "hostdata.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cctype>

#include "src/auth.h"

namespace multislot::hostdata {
namespace {

constexpr std::array<std::uint8_t, 4> kBundleMagic{'E', 'D', 'H', 'B'};
constexpr std::array<std::uint8_t, 4> kPacketMagic{'E', 'D', 'H', 'D'};
constexpr std::uint8_t kFormat = 1;
constexpr std::size_t kPacketHead = 4 + 1 + 1 + 32;  // magic, format, type, digest
constexpr std::size_t kPartHead = kPacketHead + 4 + 4;
static_assert(kPartHead + kPartBytes <= kMaxPacket, "a part must fit one EOS packet");

bool NameChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// A name Windows opens as a device, whatever the extension: NUL.SGO is the NUL device, not a file.
bool DeviceName(std::string_view stem) {
    constexpr std::string_view kDevices[] = {"CON", "PRN", "AUX", "NUL"};
    if (std::find(std::begin(kDevices), std::end(kDevices), stem) != std::end(kDevices)) return true;
    return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '0' && stem[3] <= '9';
}

// "<NAME>.SGO" with NAME made of A-Z 0-9 _ only (and not a device): one dot, so WEAPONTEXT.EN.SGO is not one.
bool SgoName(std::string_view name) {
    constexpr std::string_view kExt = ".SGO";
    if (name.size() <= kExt.size() || !name.ends_with(kExt)) return false;
    const std::string_view stem = name.substr(0, name.size() - kExt.size());
    return std::all_of(stem.begin(), stem.end(), NameChar) && !DeviceName(stem);
}

void Put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::uint32_t Get32(const std::uint8_t* at) {
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
           (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
}

bool Fail(std::string* why, const std::string& text) {
    if (why) *why = text;
    return false;
}

// One file as MakeBundle and ParseBundle both require it.
bool CheckFile(const DataFile& file, std::string* why) {
    if (!SharedPath(file.path)) return Fail(why, file.path + " is not a weapon or vehicle data file");
    if (file.bytes.size() > kMaxFileBytes)
        return Fail(why, file.path + " is larger than " + std::to_string(kMaxFileBytes / 1024) + " KB");
    if (!LooksLikeSgo(file.bytes)) return Fail(why, file.path + " is not an SGO file");
    return true;
}

// Every file, then the limits on all of them. `files` sorted by path.
bool CheckFiles(const std::vector<DataFile>& files, std::string* why) {
    if (files.size() > kMaxFiles) return Fail(why, "more than " + std::to_string(kMaxFiles) + " files");
    std::size_t total = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!CheckFile(files[i], why)) return false;
        if (i && files[i - 1].path == files[i].path) return Fail(why, files[i].path + " twice");
        total += files[i].bytes.size();
    }
    if (total > kMaxTotalBytes) return Fail(why, "more than " + std::to_string(kMaxTotalBytes / 1024 / 1024) + " MB");
    return true;
}

std::vector<std::uint8_t> PacketHead(PacketType type, const Digest& digest) {
    std::vector<std::uint8_t> out(kPacketMagic.begin(), kPacketMagic.end());
    out.push_back(kFormat);
    out.push_back(static_cast<std::uint8_t>(type));
    out.insert(out.end(), digest.begin(), digest.end());
    return out;
}

std::string Narrow(const std::wstring& wide) {
    std::string out;
    for (wchar_t c : wide) {
        if (c <= 0 || c >= 0x80) return {};
        out.push_back(static_cast<char>(c));
    }
    return out;
}

bool ReadSmallFile(const std::wstring& path, std::vector<std::uint8_t>& out, std::string* why) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Fail(why, "cannot be read");
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != 0;
    if (ok && static_cast<unsigned long long>(size.QuadPart) > kMaxFileBytes) {
        ok = Fail(why, "is larger than " + std::to_string(kMaxFileBytes / 1024) + " KB");
    } else if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                             read == out.size());
        if (!ok) Fail(why, "cannot be read");
    }
    CloseHandle(file);
    return ok;
}

// The files `pattern` (e.g. "V*.SGO") matches in `mods`\`folder`, top level only.
void ScanFolder(const std::wstring& mods, const wchar_t* folder, const wchar_t* pattern, std::vector<DataFile>& out,
                std::vector<std::string>* skipped) {
    const std::wstring dir = mods + L"\\" + folder + L"\\";
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((dir + pattern).c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string path = NormalPath(Narrow(std::wstring(folder) + L"/" + found.cFileName));
        std::string why;
        DataFile file{path, {}};
        if (path.empty() || !SharedPath(path)) {
            why = "is not a weapon or vehicle data file (only A-Z 0-9 _ names are shared)";
        } else if (ReadSmallFile(dir + found.cFileName, file.bytes, &why) && !LooksLikeSgo(file.bytes)) {
            why = "is not an SGO file";
        }
        if (why.empty()) {
            out.push_back(std::move(file));
        } else if (skipped) {
            skipped->push_back(Narrow(std::wstring(folder) + L"\\" + found.cFileName) + " " + why);
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

bool StartsWith(const wchar_t* text, std::wstring_view prefix) {
    return std::wstring_view(text).starts_with(prefix);
}

}  // namespace

bool SharedPath(std::string_view path) {
    if (path.size() > kMaxPathChars) return false;
    constexpr std::string_view kWeapon = "WEAPON/", kObject = "OBJECT/";
    if (path.starts_with(kWeapon)) {
        const std::string_view name = path.substr(kWeapon.size());
        return SgoName(name) && name != "WEAPONTABLE.SGO";
    }
    if (path.starts_with(kObject)) {
        const std::string_view name = path.substr(kObject.size());
        return name.size() > 4 && name[0] == 'V' && std::isdigit(static_cast<unsigned char>(name[1])) &&
               std::isdigit(static_cast<unsigned char>(name[2])) && std::isdigit(static_cast<unsigned char>(name[3])) &&
               SgoName(name);
    }
    return false;
}

std::string NormalPath(std::string_view path) {
    if (path.empty() || path.size() > kMaxPathChars) return {};
    std::string out;
    for (char c : path) {
        if (static_cast<unsigned char>(c) >= 0x80 || c < 0x20) return {};
        out.push_back(c == '\\' ? '/' : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool LooksLikeSgo(const std::vector<std::uint8_t>& bytes) {
    constexpr std::array<std::uint8_t, 4> kSgo{'S', 'G', 'O', 0}, kDsgo{'D', 'S', 'G', 'O'};
    if (bytes.size() < 16) return false;
    return std::equal(kSgo.begin(), kSgo.end(), bytes.begin()) || std::equal(kDsgo.begin(), kDsgo.end(), bytes.begin());
}

std::optional<Bundle> MakeBundle(std::vector<DataFile> files, std::string* why) {
    std::sort(files.begin(), files.end(), [](const DataFile& a, const DataFile& b) { return a.path < b.path; });
    if (!CheckFiles(files, why)) return std::nullopt;
    Bundle bundle;
    std::vector<std::uint8_t>& out = bundle.bytes;
    out.assign(kBundleMagic.begin(), kBundleMagic.end());
    out.push_back(kFormat);
    out.push_back(static_cast<std::uint8_t>(files.size()));
    out.push_back(static_cast<std::uint8_t>(files.size() >> 8));
    for (const DataFile& file : files) {
        out.push_back(static_cast<std::uint8_t>(file.path.size()));
        out.insert(out.end(), file.path.begin(), file.path.end());
        Put32(out, static_cast<std::uint32_t>(file.bytes.size()));
    }
    for (const DataFile& file : files) out.insert(out.end(), file.bytes.begin(), file.bytes.end());
    const std::optional<Digest> digest = Sha256(out);
    if (!digest) {
        Fail(why, "SHA-256 is unavailable");
        return std::nullopt;
    }
    bundle.digest = *digest;
    bundle.files = files.size();
    return bundle;
}

std::optional<std::vector<DataFile>> ParseBundle(const std::vector<std::uint8_t>& bytes, std::string* why) {
    constexpr std::size_t kHead = 4 + 1 + 2;
    if (bytes.size() < kHead || !std::equal(kBundleMagic.begin(), kBundleMagic.end(), bytes.begin()) ||
        bytes[4] != kFormat) {
        Fail(why, "not a host data bundle of this version");
        return std::nullopt;
    }
    const std::size_t count = bytes[5] | (static_cast<std::size_t>(bytes[6]) << 8);
    if (count > kMaxFiles) {
        Fail(why, "more than " + std::to_string(kMaxFiles) + " files");
        return std::nullopt;
    }
    std::vector<DataFile> files(count);
    std::size_t at = kHead;
    for (DataFile& file : files) {
        const std::size_t length = at < bytes.size() ? bytes[at] : 0;
        if (!length || at + 1 + length + 4 > bytes.size()) {
            Fail(why, "the file list is cut short");
            return std::nullopt;
        }
        file.path.assign(reinterpret_cast<const char*>(&bytes[at + 1]), length);
        const std::uint32_t size = Get32(&bytes[at + 1 + length]);
        if (size > kMaxFileBytes) {
            Fail(why, file.path + " is too large");
            return std::nullopt;
        }
        file.bytes.resize(size);
        at += 1 + length + 4;
    }
    for (DataFile& file : files) {
        if (file.bytes.size() > bytes.size() - at) {
            Fail(why, "the file contents are cut short");
            return std::nullopt;
        }
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(at), file.bytes.size(), file.bytes.begin());
        at += file.bytes.size();
    }
    if (at != bytes.size()) {
        Fail(why, "bytes after the last file");
        return std::nullopt;
    }
    if (!std::is_sorted(files.begin(), files.end(), [](const DataFile& a, const DataFile& b) { return a.path < b.path; }) ||
        !CheckFiles(files, why)) {
        if (why && why->empty()) *why = "the files are not in order";
        return std::nullopt;
    }
    return files;
}

std::vector<DataFile> ScanMods(const std::wstring& mods, std::vector<std::string>* skipped) {
    std::vector<DataFile> files;
    ScanFolder(mods, L"WEAPON", L"*.SGO", files, skipped);
    ScanFolder(mods, L"OBJECT", L"V*.SGO", files, skipped);
    std::sort(files.begin(), files.end(), [](const DataFile& a, const DataFile& b) { return a.path < b.path; });
    return files;
}

std::optional<Digest> Sha256(const std::vector<std::uint8_t>& bytes) { return dn::sha256(bytes.data(), bytes.size()); }

std::string DigestHex(const Digest& digest) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 15]);
    }
    return out;
}

std::optional<Digest> DigestFromHex(std::string_view hex) {
    if (hex.size() != 64) return std::nullopt;
    Digest digest{};
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = hex[i];
        int value = -1;
        if (c >= '0' && c <= '9') value = c - '0';
        if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        if (value < 0) return std::nullopt;
        digest[i / 2] = static_cast<std::uint8_t>(digest[i / 2] | (i % 2 ? value : value << 4));
    }
    return digest;
}

std::string OpenedDataPath(const wchar_t* gamePath) {
    if (!gamePath) return {};
    constexpr std::wstring_view kBind = L"/cri_bind/", kMods = L"./Mods/";
    std::wstring_view rest;
    if (StartsWith(gamePath, kBind)) {
        rest = std::wstring_view(gamePath).substr(kBind.size());
    } else if (StartsWith(gamePath, kMods)) {
        rest = std::wstring_view(gamePath).substr(kMods.size());
    } else {
        return {};
    }
    if (rest.size() > kMaxPathChars) return {};
    std::string path = NormalPath(Narrow(std::wstring(rest)));
    return SharedPath(path) ? path : std::string();
}

std::optional<std::wstring> OverlayPath(const wchar_t* gamePath, const Overlay& overlay) {
    if (overlay.paths.empty()) return std::nullopt;
    const std::string path = OpenedDataPath(gamePath);
    if (path.empty() || !std::binary_search(overlay.paths.begin(), overlay.paths.end(), path)) return std::nullopt;
    return overlay.folder + std::wstring(path.begin(), path.end());
}

std::vector<std::uint8_t> EncodeGet(const Digest& digest) { return PacketHead(PacketType::Get, digest); }

std::vector<std::uint8_t> EncodeNone(const Digest& digest) { return PacketHead(PacketType::None, digest); }

std::size_t PartCount(std::size_t bundleBytes) { return (bundleBytes + kPartBytes - 1) / kPartBytes; }

std::vector<std::uint8_t> EncodePart(const Bundle& bundle, std::size_t index) {
    const std::size_t offset = index * kPartBytes;
    if (offset >= bundle.bytes.size()) return {};
    std::vector<std::uint8_t> out = PacketHead(PacketType::Part, bundle.digest);
    Put32(out, static_cast<std::uint32_t>(bundle.bytes.size()));
    Put32(out, static_cast<std::uint32_t>(offset));
    const std::size_t size = std::min(kPartBytes, bundle.bytes.size() - offset);
    const auto from = bundle.bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    out.insert(out.end(), from, from + static_cast<std::ptrdiff_t>(size));
    return out;
}

std::optional<Packet> DecodePacket(const std::uint8_t* data, std::size_t size) {
    if (!data || size < kPacketHead || size > kMaxPacket || !std::equal(kPacketMagic.begin(), kPacketMagic.end(), data) ||
        data[4] != kFormat)
        return std::nullopt;
    Packet packet;
    packet.type = static_cast<PacketType>(data[5]);
    std::copy_n(data + 6, packet.digest.size(), packet.digest.begin());
    switch (packet.type) {
        case PacketType::Get:
        case PacketType::None:
            return size == kPacketHead ? std::optional<Packet>(packet) : std::nullopt;
        case PacketType::Part:
            if (size <= kPartHead) return std::nullopt;
            packet.total = Get32(data + kPacketHead);
            packet.offset = Get32(data + kPacketHead + 4);
            packet.data.assign(data + kPartHead, data + size);
            return packet;
        default:
            return std::nullopt;
    }
}

bool Assembler::Add(const Packet& part) {
    // A bundle is at most the files, their list and its head.
    constexpr std::size_t kMaxBundle = kMaxTotalBytes + kMaxFiles * (1 + kMaxPathChars + 4) + 7;
    if (part.type != PacketType::Part || part.digest != digest_ || !part.total || part.total > kMaxBundle) return false;
    if (!started_) {
        started_ = true;
        bytes_.assign(part.total, 0);
        parts_.assign(PartCount(part.total), false);
    }
    if (part.total != bytes_.size() || part.offset % kPartBytes || part.offset >= bytes_.size()) return false;
    const std::size_t index = part.offset / kPartBytes;
    if (part.data.size() != std::min(kPartBytes, bytes_.size() - part.offset)) return false;
    if (parts_[index]) return true;
    std::copy(part.data.begin(), part.data.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(part.offset));
    parts_[index] = true;
    ++received_;
    return true;
}

std::optional<std::vector<std::uint8_t>> Assembler::Take(std::string* why) {
    if (!Complete()) {
        Fail(why, "incomplete");
        return std::nullopt;
    }
    const std::optional<Digest> digest = Sha256(bytes_);
    if (!digest || *digest != digest_) {
        Fail(why, "what arrived is not what the host published (SHA-256 differs)");
        return std::nullopt;
    }
    return std::move(bytes_);
}

}  // namespace multislot::hostdata
