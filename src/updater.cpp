#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "log.h"

namespace dn {
namespace {

constexpr const wchar_t* kAgent = L"EDF6DirectNet";
constexpr const char* kLatestRelease = "https://api.github.com/repos/hajisensai/edf-coop-stable/releases/latest";
constexpr const char* kDllAsset = "EDF6DirectNet.dll";
constexpr const char* kShaAsset = "EDF6DirectNet.dll.sha256";
constexpr size_t kMaxJson = 1024 * 1024;
constexpr size_t kMaxDll = 16 * 1024 * 1024;
constexpr DWORD kTimeoutMs = 15000;

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
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

std::string format(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
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
    char latestText[32];
    snprintf(latestText, sizeof(latestText), "%d.%d.%d", latest.major, latest.minor, latest.patch);
    std::string dllUrl = assetUrl(json, kDllAsset), shaUrl = assetUrl(json, kShaAsset);
    if (dllUrl.empty() || shaUrl.empty()) {
        return format("UPDATE %s is out but has no %s to update from; get it from the release page", latestText, kDllAsset);
    }
    std::vector<uint8_t> dll, sha;
    if (!httpGet(dllUrl, false, kMaxDll, dll, &why) || !httpGet(shaUrl, false, 4096, sha, &why)) {
        return format("UPDATE %s download failed: %s", latestText, why.c_str());
    }
    if (!verifyUpdate(dll, std::string(sha.begin(), sha.end()), latest, &why)) {
        return format("UPDATE %s rejected: %s", latestText, why.c_str());
    }
    if (!installOver(installed, dll, &why)) {
        return format("UPDATE %s could not be installed: %s", latestText, why.c_str());
    }
    return format("UPDATE installed %s (was %s); it runs the next time you start the game", latestText, currentText.c_str());
}

}  // namespace

bool Version::newerThan(const Version& o) const {
    if (major != o.major) return major > o.major;
    if (minor != o.minor) return minor > o.minor;
    return patch > o.patch;
}

Version parseVersion(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9' || (i > 0 && text[i - 1] >= '0' && text[i - 1] <= '9')) continue;
        int a = -1, b = -1, c = -1;
        if (sscanf_s(text.c_str() + i, "%d.%d.%d", &a, &b, &c) == 3 && a >= 0 && b >= 0 && c >= 0) return {a, b, c};
    }
    return {};
}

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
    // Each asset object has "name" before its "browser_download_url".
    for (size_t at = releaseJson.find("\"assets\""); at != std::string::npos;) {
        at = releaseJson.find("\"name\"", at + 1);
        if (at == std::string::npos) break;
        if (jsonString(releaseJson, "name", at) != name) continue;
        std::string url = jsonString(releaseJson, "browser_download_url", at);
        return url.rfind("https://github.com/", 0) == 0 ? url : std::string();  // only GitHub itself
    }
    return {};
}

std::string sha256Hex(const std::vector<uint8_t>& data) {
    uint8_t digest[32] = {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) == 0 &&
              BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
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

bool installOver(const std::wstring& installed, const std::vector<uint8_t>& data, std::string* why) {
    std::wstring fresh = installed + L".new", old = installed + L".old";
    if (!writeFile(fresh, data)) {
        *why = "cannot write " + std::to_string(data.size()) + " bytes next to the plugin (error " +
               std::to_string(GetLastError()) + ")";
        return false;
    }
    DeleteFileW(old.c_str());  // a previous update's leftover, if this process does not have it mapped
    if (!MoveFileExW(installed.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *why = "cannot move the running plugin aside (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(fresh.c_str());
        return false;
    }
    if (!MoveFileExW(fresh.c_str(), installed.c_str(), 0)) {
        *why = "cannot put the new plugin in place (error " + std::to_string(GetLastError()) + ")";
        MoveFileExW(old.c_str(), installed.c_str(), 0);  // back to what was there
        DeleteFileW(fresh.c_str());
        return false;
    }
    return true;
}

void removeOldUpdate(const std::wstring& installed) {
    std::wstring old = installed + L".old";
    if (DeleteFileW(old.c_str())) logf("UPDATE removed the previous version's leftover file");
}

std::string updateOnce(const std::wstring& installed, const std::string& current) {
    return updateOnceImpl(installed, current);
}

void startAutoUpdate(const std::wstring& installed, const char* current) {
    std::thread([installed, version = std::string(current)] { logf("%s", updateOnce(installed, version).c_str()); })
        .detach();
}

}  // namespace dn
