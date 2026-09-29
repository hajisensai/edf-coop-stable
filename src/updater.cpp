#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>

#include "log.h"

namespace dn {
namespace {

constexpr const wchar_t* kAgent = L"EDF6DirectNet";
constexpr const char* kLatestRelease = "https://api.github.com/repos/hajisensai/edf-coop-stable/releases/latest";
// Every asset must come from here (followed by "<tag>/<name>").
constexpr const char* kDownloadPrefix = "https://github.com/hajisensai/edf-coop-stable/releases/download/";
constexpr const char* kReleasePage = "https://github.com/hajisensai/edf-coop-stable/releases";
constexpr const char* kDllAsset = "EDF6DirectNet.dll";
constexpr const char* kSigAsset = "EDF6DirectNet.dll.sig";
constexpr size_t kMaxJson = 1024 * 1024;
constexpr size_t kMaxDll = 16 * 1024 * 1024;
constexpr size_t kMaxSig = 4096;
constexpr DWORD kTimeoutMs = 15000;

// The release signing key (BCRYPT_ECCPUBLIC_BLOB: "ECS1", 32, X, Y). Its private half is the
// repository secret EDF6DN_UPDATE_SIGNING_KEY; the release workflow checks that the two match.
constexpr uint8_t kReleaseKey[72] = {
    0x45, 0x43, 0x53, 0x31, 0x20, 0x00, 0x00, 0x00, 0x6e, 0xf3, 0xe6, 0xc4, 0x79, 0x6c, 0x44, 0x94, 0x94, 0x1f,
    0x7b, 0xd1, 0x0d, 0x74, 0x95, 0x52, 0x02, 0x42, 0x8e, 0x9a, 0xd3, 0x54, 0xdc, 0x14, 0x7f, 0x0c, 0xdf, 0x03,
    0x94, 0xe4, 0xe9, 0xd8, 0x8f, 0xa9, 0xcb, 0x37, 0x4b, 0xa2, 0x0b, 0x7a, 0x15, 0x28, 0x11, 0x5d, 0xba, 0xa4,
    0x70, 0x92, 0xdf, 0x3f, 0x23, 0x43, 0x6f, 0x96, 0x01, 0xe0, 0x46, 0x56, 0xdb, 0xdf, 0x9d, 0x43, 0xa9, 0xc4,
};

// installOver, beginRun and confirmHealthy all move the same files; one at a time within a game.
std::mutex g_files;

// Set by the first EOS tick (noteGameRunning): a trial's health clock starts there, not at load, so a
// game quit at its title screen is not taken for a version that failed.
HANDLE g_gameRunning = CreateEventW(nullptr, TRUE, FALSE, nullptr);
std::atomic<bool> g_gameRunningNoted{false};

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// One HTTPS GET (redirects followed, system proxy honoured) into `out`, at most `maxBytes`.
bool httpGet(const std::string& url, bool api, size_t maxBytes, std::vector<uint8_t>& out, std::string* why) {
    out.clear();
    std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        *why = "bad URL " + url;
        return false;
    }
    std::wstring object = std::wstring(path) + extra;
    bool ok = false;
    HINTERNET session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)  // before Windows 8.1 there is no automatic proxy: use the WinHTTP default
        session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request) {
        WinHttpSetTimeouts(request, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        DWORD status = 0, size = sizeof(status);
        const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
        if (WinHttpSendRequest(request, headers, api ? static_cast<DWORD>(-1) : 0, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                                nullptr)) {
            if (status != 200) {
                *why = "HTTP " + std::to_string(status) + " from " + url;
            } else {
                ok = true;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail)) {
                        ok = false;
                        *why = "connection lost while downloading " + url;
                        break;
                    }
                    if (avail == 0) break;
                    if (out.size() + avail > maxBytes) {
                        ok = false;
                        *why = "download larger than expected: " + url;
                        break;
                    }
                    size_t at = out.size();
                    out.resize(at + avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, out.data() + at, avail, &read)) {
                        ok = false;
                        *why = "connection lost while downloading " + url;
                        break;
                    }
                    out.resize(at + read);
                }
            }
        } else {
            *why = "cannot reach " + std::string(url.substr(0, url.find('/', 8))) + " (error " +
                   std::to_string(GetLastError()) + ")";
        }
    } else {
        *why = "cannot start a request (error " + std::to_string(GetLastError()) + ")";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

bool writeFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size() &&
              FlushFileBuffers(f);
    DWORD error = ok ? 0 : GetLastError();
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    SetLastError(error);  // the caller reports the write's error, not the cleanup's
    return ok;
}

bool writeText(const std::wstring& path, const std::string& text) {
    return writeFile(path, std::vector<uint8_t>(text.begin(), text.end()));
}

// The first `maxBytes` of a file; "" when it cannot be read.
std::string readFile(const std::wstring& path, size_t maxBytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string data(maxBytes, '\0');
    DWORD read = 0;
    if (!ReadFile(f, data.data(), static_cast<DWORD>(maxBytes), &read, nullptr)) read = 0;
    CloseHandle(f);
    data.resize(read);
    return data;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string format(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

bool sha256(const void* data, size_t size, uint8_t digest[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) == 0 &&
              BCryptFinishHash(hash, digest, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ECDSA P-256 over SHA-256 of `message`; `signature` is r||s (64 bytes).
bool verifySignature(const std::string& message, const uint8_t signature[64], const std::vector<uint8_t>& publicKey) {
    uint8_t digest[32];
    if (!sha256(message.data(), message.size(), digest)) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, const_cast<PUCHAR>(publicKey.data()),
                                  static_cast<ULONG>(publicKey.size()), 0) == 0 &&
              BCryptVerifySignature(key, nullptr, digest, sizeof(digest), const_cast<PUCHAR>(signature), 64, 0) == 0;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool isLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

bool lowerHexLine(const std::string& line, size_t digits) {
    return line.size() == digits && std::all_of(line.begin(), line.end(), isLowerHex);
}

std::wstring sibling(const std::wstring& installed, const wchar_t* suffix) { return installed + suffix; }

// "0.3.7 1234" in installed.trial: the game (pid) running the version on trial. pid 0: the last game running
// it ended the normal way (noteCleanExit) before the version had proven itself - not a failure.
struct Trial {
    Version version;
    DWORD pid = 0;
};

Trial readTrial(const std::wstring& installed) {
    std::string text = readFile(sibling(installed, L".trial"), 64);
    Trial t;
    t.version = parseVersion(text);
    size_t space = text.find(' ');
    if (space != std::string::npos) t.pid = static_cast<DWORD>(strtoul(text.c_str() + space + 1, nullptr, 10));
    return t;
}

// Another game started from the same EDF6.exe is still running as `pid` (then its trial is not over).
bool sameGameRunning(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return pid != 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t theirs[MAX_PATH] = {}, ours[MAX_PATH] = {};
    DWORD size = MAX_PATH, code = 0;
    bool running = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE && QueryFullProcessImageNameW(p, 0, theirs, &size) &&
                   GetModuleFileNameW(nullptr, ours, MAX_PATH) && _wcsicmp(theirs, ours) == 0;
    CloseHandle(p);
    return running;
}

// The version a DLL file says it is ("?" when it does not say).
std::string fileVersion(const std::wstring& path) {
    std::string data = readFile(path, kMaxDll);
    size_t at = data.find("EDF6DN_VERSION=");
    Version v = at == std::string::npos ? Version{} : parseVersion(data.substr(at, 40));
    return v.valid() ? versionText(v) : "?";
}

std::string updateOnceImpl(const std::wstring& installed, const std::string& currentText) {
    Version current = parseVersion(currentText);
    std::string why;
    std::vector<uint8_t> body;
    if (!httpGet(kLatestRelease, true, kMaxJson, body, &why)) {
        return format("UPDATE check skipped: %s", why.c_str());
    }
    std::string json(body.begin(), body.end());
    Version latest = parseVersion(jsonString(json, "tag_name"));
    if (!latest.valid()) {
        return format("UPDATE check skipped: the latest release has no version");
    }
    if (!latest.newerThan(current)) {
        return format("UPDATE %s is the latest version", currentText.c_str());
    }
    std::string latestText = versionText(latest);
    if (badVersion(installed) == latest) {
        return format("UPDATE %s is out, but it was rolled back here after it failed to run; waiting for a newer "
                      "release (delete EDF6DirectNet.dll.bad to try it again)",
                      latestText.c_str());
    }
    std::string dllUrl = assetUrl(json, kDllAsset), sigUrl = assetUrl(json, kSigAsset);
    if (dllUrl.empty()) {
        return format("UPDATE %s is out but has no %s to update from; get it from %s", latestText.c_str(), kDllAsset,
                      kReleasePage);
    }
    if (sigUrl.empty()) {
        return format("UPDATE %s rejected: the release has no signature (%s), so it cannot be checked; get it from %s "
                      "if you trust it",
                      latestText.c_str(), kSigAsset, kReleasePage);
    }
    std::vector<uint8_t> dll, sig;
    if (!httpGet(dllUrl, false, kMaxDll, dll, &why) || !httpGet(sigUrl, false, kMaxSig, sig, &why)) {
        return format("UPDATE %s download failed: %s", latestText.c_str(), why.c_str());
    }
    if (!verifyRelease(dll, std::string(sig.begin(), sig.end()), latest, current, releaseSigningKey(), &why)) {
        return format("UPDATE %s rejected: %s. Not installed; see %s", latestText.c_str(), why.c_str(), kReleasePage);
    }
    if (!installOver(installed, dll, &why)) {
        return format("UPDATE %s could not be installed: %s", latestText.c_str(), why.c_str());
    }
    return format("UPDATE installed %s (was %s); it runs the next time you start the game", latestText.c_str(),
                  currentText.c_str());
}

}  // namespace

bool Version::newerThan(const Version& o) const {
    if (major != o.major) return major > o.major;
    if (minor != o.minor) return minor > o.minor;
    return patch > o.patch;
}

namespace {
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// A run of 1-6 digits at `at` (nothing else, no sign or space), moving `at` past it; -1 when there is none.
int readNumber(const std::string& text, size_t& at) {
    size_t start = at;
    int n = 0;
    while (at < text.size() && isDigit(text[at]) && at - start < 6) n = n * 10 + (text[at++] - '0');
    return at == start || (at < text.size() && isDigit(text[at])) ? -1 : n;
}
}  // namespace

Version parseVersion(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (!isDigit(text[i]) || (i > 0 && isDigit(text[i - 1]))) continue;
        size_t at = i;
        int part[3];
        int k = 0;
        for (; k < 3; ++k) {
            if (k > 0 && (at >= text.size() || text[at++] != '.')) break;
            if ((part[k] = readNumber(text, at)) < 0) break;
        }
        if (k == 3) return {part[0], part[1], part[2]};
    }
    return {};
}

std::string versionText(const Version& v) { return format("%d.%d.%d", v.major, v.minor, v.patch); }

std::string jsonString(const std::string& json, const std::string& field, size_t from) {
    size_t at = json.find("\"" + field + "\"", from);
    if (at == std::string::npos) return {};
    at = json.find_first_not_of(" \t\r\n", at + field.size() + 2);
    if (at == std::string::npos || json[at] != ':') return {};
    at = json.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string::npos || json[at] != '"') return {};
    size_t end = json.find('"', at + 1);
    return end == std::string::npos ? std::string() : json.substr(at + 1, end - at - 1);
}

std::string assetUrl(const std::string& releaseJson, const std::string& name) {
    // Only this repository's download of this release's own tag, spelled exactly the way GitHub does:
    // no other repository, host, tag or path tricks ("..", "%2e", a query) can match.
    std::string tag = jsonString(releaseJson, "tag_name");
    if (tag.empty() || tag.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                           std::string::npos)
        return {};
    std::string expected = std::string(kDownloadPrefix) + tag + "/" + name;
    // Each asset object has "name" before its "browser_download_url".
    for (size_t at = releaseJson.find("\"assets\""); at != std::string::npos;) {
        at = releaseJson.find("\"name\"", at + 1);
        if (at == std::string::npos) break;
        if (jsonString(releaseJson, "name", at) != name) continue;
        std::string url = jsonString(releaseJson, "browser_download_url", at);
        return url == expected ? url : std::string();
    }
    return {};
}

std::string sha256Hex(const std::vector<uint8_t>& data) {
    uint8_t digest[32] = {};
    if (!sha256(data.data(), data.size(), digest)) return {};
    std::string hex;
    char b[3];
    for (uint8_t d : digest) {
        snprintf(b, sizeof(b), "%02x", d);
        hex += b;
    }
    return hex;
}

std::string versionMarker(const Version& v) {
    char buf[48];
    snprintf(buf, sizeof(buf), "EDF6DN_VERSION=%d.%d.%d", v.major, v.minor, v.patch);
    return buf;
}

bool verifyUpdate(const std::vector<uint8_t>& dll, const std::string& shaText, const Version& version, std::string* why) {
    std::string expected;
    for (char c : shaText) {
        if (expected.size() == 64) break;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) expected += c;
        else if (c >= 'A' && c <= 'F') expected += static_cast<char>(c - 'A' + 'a');
        else if (!expected.empty()) break;
    }
    if (expected.size() != 64) {
        *why = "the published SHA-256 is unreadable";
        return false;
    }
    std::string actual = sha256Hex(dll);
    if (actual != expected) {
        *why = "SHA-256 mismatch (download damaged or changed)";
        return false;
    }
    if (dll.size() < 1024 || dll[0] != 'M' || dll[1] != 'Z') {
        *why = "not a DLL";
        return false;
    }
    std::string marker = versionMarker(version);
    if (std::search(dll.begin(), dll.end(), marker.begin(), marker.end()) == dll.end()) {
        *why = "the DLL does not say it is version " + marker.substr(marker.find('=') + 1);
        return false;
    }
    return true;
}

const std::vector<uint8_t>& releaseSigningKey() {
    static const std::vector<uint8_t> key(std::begin(kReleaseKey), std::end(kReleaseKey));
    return key;
}

bool readSignedManifest(const std::string& sigFile, const std::vector<uint8_t>& publicKey, SignedManifest* out,
                        std::string* why) {
    std::string lines[3];
    size_t at = 0;
    for (std::string& line : lines) {
        size_t end = sigFile.find('\n', at);
        if (end == std::string::npos) break;
        line = sigFile.substr(at, end - at);
        at = end + 1;
    }
    Version version = parseVersion(lines[0]);
    bool shaped = at == sigFile.size() && version.valid() && lines[0] == "EDF6DirectNet " + versionText(version) &&
                  lowerHexLine(lines[1], 64) && lowerHexLine(lines[2], 128);
    if (!shaped) {
        *why = "the signature file is malformed";
        return false;
    }
    uint8_t signature[64];
    for (size_t i = 0; i < 64; ++i) signature[i] = static_cast<uint8_t>(std::stoi(lines[2].substr(2 * i, 2), nullptr, 16));
    if (!verifySignature(lines[0] + "\n" + lines[1] + "\n", signature, publicKey)) {
        *why = "the signature is not valid (not signed by the EDF6DirectNet release key)";
        return false;
    }
    out->version = version;
    out->sha256 = lines[1];
    return true;
}

bool verifyRelease(const std::vector<uint8_t>& dll, const std::string& sigFile, const Version& release,
                   const Version& current, const std::vector<uint8_t>& publicKey, std::string* why) {
    SignedManifest m;
    if (!readSignedManifest(sigFile, publicKey, &m, why)) return false;
    if (!(m.version == release)) {
        *why = "the signed version " + versionText(m.version) + " is not the release " + versionText(release);
        return false;
    }
    if (!m.version.newerThan(current)) {
        *why = "the signed version " + versionText(m.version) + " is not newer than the running " + versionText(current);
        return false;
    }
    return verifyUpdate(dll, m.sha256, m.version, why);
}

namespace {
// Renames `from` over `to` in one step. With POSIX semantics (Windows 10 1709 and later, NTFS) this
// works while someone has `to` open, e.g. a virus scanner or Explorer looking at the plugin; the plain
// rename below fails with access denied then.
bool renameOver(const std::wstring& from, const std::wstring& to) {
    HANDLE f = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO) + to.size() * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
        info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
        info->FileNameLength = static_cast<DWORD>(to.size() * sizeof(wchar_t));
        memcpy(info->FileName, to.c_str(), to.size() * sizeof(wchar_t));
        bool ok = SetFileInformationByHandle(f, FileRenameInfoEx, info, static_cast<DWORD>(buf.size()));
        CloseHandle(f);
        if (ok) return true;
    }
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
}  // namespace

bool swapIn(const std::wstring& target, const std::wstring& replacement, const std::wstring& aside, std::string* why) {
    if (!DeleteFileW(aside.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        *why = "cannot remove the old backup (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (CreateHardLinkW(aside.c_str(), target.c_str(), nullptr)) {
        if (renameOver(replacement, target)) return true;
        *why = "cannot put the new file in place (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(aside.c_str());  // only a second name: the file itself is untouched
        return false;
    }
    // No hard links on this drive: ReplaceFileW renames target to aside, then replacement to target.
    if (ReplaceFileW(target.c_str(), replacement.c_str(), aside.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr,
                     nullptr))
        return true;
    DWORD error = GetLastError();
    *why = "cannot put the new file in place (error " + std::to_string(error) + ")";
    if (!exists(target) && !MoveFileExW(aside.c_str(), target.c_str(), 0))  // back to what was there
        *why += "; EDF6DirectNet.dll is now MISSING: rename EDF6DirectNet.dll.old back to EDF6DirectNet.dll";
    return false;
}

bool installOver(const std::wstring& installed, const std::vector<uint8_t>& data, std::string* why) {
    // Per process, so two game instances updating at once never touch each other's download.
    std::wstring fresh = installed + L".new" + std::to_wstring(GetCurrentProcessId());
    if (!writeFile(fresh, data)) {
        *why = "cannot write " + std::to_string(data.size()) + " bytes next to the plugin (error " +
               std::to_string(GetLastError()) + ")";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_files);
    // The rollback target must be a version that has proven itself: the running one, unless it is still on
    // trial - then the one kept before it stays, and the running one is only moved aside.
    std::wstring old = sibling(installed, L".old"), trial = sibling(installed, L".trial");
    bool onTrial = exists(trial) && exists(old);
    if (!swapIn(installed, fresh, onTrial ? sibling(installed, L".rolledback") : old, why)) {
        DeleteFileW(fresh.c_str());
        return false;
    }
    DeleteFileW(trial.c_str());  // the running version's trial is over either way: it is not installed anymore
    return true;
}

void removeUpdateLeftovers(const std::wstring& installed) {
    DeleteFileW(sibling(installed, L".rolledback").c_str());  // fails while a game still has it loaded
    // Downloads a game that quit mid-install left behind: installed.new<pid>.
    std::wstring dir = installed.substr(0, installed.find_last_of(L"\\/") + 1);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((installed + L".new*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = found.cFileName;
        size_t tail = name.rfind(L".new");
        if (tail != std::wstring::npos && tail + 4 < name.size() &&
            name.find_first_not_of(L"0123456789", tail + 4) == std::wstring::npos)
            DeleteFileW((dir + name).c_str());
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

Version badVersion(const std::wstring& installed) { return parseVersion(readFile(sibling(installed, L".bad"), 64)); }

RunState beginRun(const std::wstring& installed, const std::string& versionString) {
    std::lock_guard<std::mutex> lock(g_files);
    removeUpdateLeftovers(installed);
    Version me = parseVersion(versionString);
    std::wstring old = sibling(installed, L".old"), trialPath = sibling(installed, L".trial");
    Trial trial = readTrial(installed);
    if (!exists(old)) {  // nothing to go back to: installed by hand, or proven healthy
        DeleteFileW(trialPath.c_str());
        return RunState::Normal;
    }
    bool rolledBackBefore = badVersion(installed) == me;  // an older updater installed it again
    if (!rolledBackBefore && !(trial.version == me)) {
        writeText(trialPath, versionText(me) + " " + std::to_string(GetCurrentProcessId()) + "\n");
        logf("UPDATE first run of %s: keeping %s as EDF6DirectNet.dll.old until this version has run for %u seconds "
             "past the game's first EOS tick",
             versionString.c_str(), fileVersion(old).c_str(), kHealthySeconds);
        return RunState::Trial;
    }
    if (!rolledBackBefore && sameGameRunning(trial.pid)) return RunState::Trial;  // that game's trial, not over
    if (!rolledBackBefore && trial.pid == 0) {  // the last game quit normally before it was proven: go on trying
        writeText(trialPath, versionText(me) + " " + std::to_string(GetCurrentProcessId()) + "\n");
        logf("UPDATE %s is still on trial (the last game ended normally before it had run long enough)",
             versionString.c_str());
        return RunState::Trial;
    }
    std::string previous = fileVersion(old), why;
    if (!swapIn(installed, old, sibling(installed, L".rolledback"), &why)) {
        logf("UPDATE %s did not run properly last time, but the previous version could not be restored: %s. "
             "Reinstall from %s",
             versionString.c_str(), why.c_str(), kReleasePage);
        return RunState::Normal;  // the trial stays: the next start tries again
    }
    writeText(sibling(installed, L".bad"), versionText(me) + "\n");
    DeleteFileW(trialPath.c_str());
    logf("UPDATE ROLLED BACK: %s %s; %s is restored and runs from the next game start, and %s will not be "
         "installed again (a newer release will). EDF6DirectNet is off for this session. Please report this at %s",
         versionString.c_str(),
         rolledBackBefore ? "was rolled back before and has been installed again"
                          : "crashed or was killed before it had run for a while last time",
         previous.c_str(), versionString.c_str(), kReleasePage);
    return RunState::RolledBack;
}

void confirmHealthy(const std::wstring& installed, const std::string& versionString) {
    std::lock_guard<std::mutex> lock(g_files);
    if (!(readTrial(installed).version == parseVersion(versionString))) return;
    DeleteFileW(sibling(installed, L".trial").c_str());
    DeleteFileW(sibling(installed, L".old").c_str());
    logf("UPDATE %s runs fine; the previous version kept for rollback is removed", versionString.c_str());
}

void startHealthWatch(const std::wstring& installed, const char* version) {
    std::thread([installed, v = std::string(version)] {
        WaitForSingleObject(g_gameRunning, INFINITE);
        std::this_thread::sleep_for(std::chrono::seconds(kHealthySeconds));
        confirmHealthy(installed, v);
    }).detach();
}

void noteGameRunning() {
    if (!g_gameRunningNoted.exchange(true)) SetEvent(g_gameRunning);
}

bool noteCleanExit(const std::wstring& installed, const std::string& versionString, bool mayWait) {
    std::unique_lock<std::mutex> lock(g_files, std::defer_lock);
    // At process exit the other threads are gone, and one of them may have died holding the lock.
    if (mayWait) lock.lock(); else if (!lock.try_lock()) return false;
    Trial trial = readTrial(installed);
    if (!(trial.version == parseVersion(versionString)) || trial.pid != GetCurrentProcessId()) return false;
    return writeText(sibling(installed, L".trial"), versionText(trial.version) + " 0\n");
}

std::string updateOnce(const std::wstring& installed, const std::string& current) {
    return updateOnceImpl(installed, current);
}

void startAutoUpdate(const std::wstring& installed, const char* current) {
    std::thread([installed, version = std::string(current)] { logf("%s", updateOnce(installed, version).c_str()); })
        .detach();
}

}  // namespace dn
