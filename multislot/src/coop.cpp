#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "coop.h"

#include <string>
#include <vector>

#include "log.h"
#include "src/config.h"
#include "src/product.h"

namespace multislot {
namespace {

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

bool RetireReplacedPlugins(const std::wstring& dir) {
    bool clear = true;
    for (const wchar_t* name : coop::kReplacedPlugins) {
        const std::wstring path = dir + name, aside = path + L".disabled";
        const bool loaded = GetModuleHandleW(name) != nullptr;
        if (!Exists(path)) {
            if (loaded) {  // loaded from elsewhere, which is just as bad
                Log("REFUSED: %ls is running in this game, so EDF6Coop stays off (both would hook the same game "
                    "code). Remove it", name);
                clear = false;
            }
            continue;
        }
        if (!MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            Log("REFUSED: %ls is still in Mods\\Plugins and could not be renamed (error %lu), so EDF6Coop stays off "
                "(both would hook the same game code). Delete %ls by hand; EDF6Coop replaces it",
                name, GetLastError(), name);
            clear = false;
            continue;
        }
        if (loaded) {
            Log("REFUSED: %ls was loaded before EDF6Coop in this game, so EDF6Coop stays off this time (both would "
                "hook the same game code). It is renamed to %ls.disabled and EDF6Coop runs from the next start",
                name, name);
            clear = false;
        } else {
            Log("Replaced plugin: %ls renamed to %ls.disabled (EDF6Coop does what it did)", name, name);
        }
    }
    return clear;
}

void PrepareSettings(const std::wstring& dir, const std::wstring& iniPath, const char* roomDefaults) {
    const std::wstring oldUpnp = dir + L"EDF6DirectNet.upnp", newUpnp = dir + coop::kNameW + L".upnp";
    if (Exists(oldUpnp) && !Exists(newUpnp)) MoveFileExW(oldUpnp.c_str(), newUpnp.c_str(), 0);
    if (Exists(iniPath)) return;

    std::string text = dn::mergeIniTexts(dn::defaultIni(GetUserDefaultUILanguage()), roomDefaults);
    std::vector<dn::IniValue> carried;
    for (const wchar_t* name : coop::kReplacedSettings) {
        std::vector<dn::IniValue> values = dn::readIniValues(dir + name);
        if (!values.empty()) Log("Settings: carrying %zu values over from %ls", values.size(), name);
        carried.insert(carried.end(), values.begin(), values.end());
    }
    std::vector<std::string> dropped;
    text = dn::applyIniValues(text, carried, &dropped);
    for (const std::string& line : dropped) Log("Settings: %s is no longer a setting; not carried over", line.c_str());
    if (dn::writeNewIniUtf16(iniPath, text))
        Log("Settings: wrote %ls%ls", coop::kNameW, carried.empty() ? L".ini with the defaults"
                                                                     : L".ini (the old settings files are no longer read)");
    else
        Log("Settings: could not write %ls.ini (error %lu); running on the defaults", coop::kNameW, GetLastError());
}

void ForwardDirectNetLine(const char* line) { Log("[DN] %s", line); }

}  // namespace multislot
