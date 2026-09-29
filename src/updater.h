// Keeps the plugin current without anyone having to reinstall it.
//
// Every player in a room should run the same version (a fix often needs everyone), and friends do not
// follow releases. So at startup one background request asks GitHub for this repository's latest
// release; when it is newer, the plugin downloads the release's EDF6DirectNet.dll and its SHA-256,
// checks both (digest, a DLL, the expected version inside), and puts the file in place of its own.
// A loaded DLL cannot be overwritten but can be renamed: ours becomes EDF6DirectNet.dll.old and the
// new one takes its name, so it runs the next time the game starts (which deletes the .old).
//
// Only releases GitHub Actions built and tested are published, and anything that fails a check is
// thrown away with the installed file untouched. The request honours the system proxy and every
// failure is one log line; the game never waits for it. [Update] AutoUpdate=0 turns it off.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dn {

struct Version {
    int major = -1, minor = -1, patch = -1;
    bool valid() const { return major >= 0; }
    bool newerThan(const Version& other) const;
};

// The first x.y.z in `text` ("v0.3.6", "EDF6DirectNet 0.3.6"); invalid when there is none.
Version parseVersion(const std::string& text);

// The string value of the first `"field": "..."` at or after `from` (no escapes); "" when absent.
std::string jsonString(const std::string& json, const std::string& field, size_t from = 0);
// The browser_download_url of the release asset called `name`; "" when there is none.
std::string assetUrl(const std::string& releaseJson, const std::string& name);

std::string sha256Hex(const std::vector<uint8_t>& data);
// The marker every build carries, e.g. "EDF6DN_VERSION=0.3.6".
std::string versionMarker(const Version& v);
// A downloaded update is taken only when its digest matches the published one ("<hex>  name"), it is
// a DLL, and it carries the version marker of the release. `why` says what failed.
bool verifyUpdate(const std::vector<uint8_t>& dll, const std::string& shaText, const Version& version, std::string* why);

// Puts `data` in place of `installed`, which may be loaded: written to installed.new, then installed
// is renamed to installed.old and installed.new takes its name. Rolls back when a step fails.
bool installOver(const std::wstring& installed, const std::vector<uint8_t>& data, std::string* why);
// Deletes installed.old left by the previous update (no longer loaded once the game restarted).
void removeOldUpdate(const std::wstring& installed);

// Checks once and installs a newer release over `installed` (currently at `current`); blocks on the
// network. Returns the log line saying what happened.
std::string updateOnce(const std::wstring& installed, const std::string& current);
// Runs updateOnce on a background thread and logs its result.
void startAutoUpdate(const std::wstring& installed, const char* current);

}  // namespace dn
