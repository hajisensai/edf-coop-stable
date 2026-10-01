#pragma once
#include <string>
#include <vector>

#include "direct_net.h"

namespace dn {

struct Config {
    bool enabled = true;
    DirectOptions direct;
    // host: what joiners connect to, published in the lobby ("" = public IPv6 + UPnP IPv4, found
    // automatically). Written as-is, so an external port different from ListenPort works.
    std::string publicAddress;
    // Not hosting: connect directly to a room host that advertises its address in the lobby.
    bool autoJoin = true;
    bool upnp = true;
    bool bindPhysicalInterface = true;
    uint16_t eosFixedPort = 0;  // 0 = leave EOS on random ports
    int eosRelay = -1;          // -1 = leave default, 0 = no relays, 1 = allow, 2 = force
    // Hide transient EOS connection loss from the game while reconnecting (see hold.h):
    // off, auto (direct-link members and members carrying the lobby marker), all (every member runs the plugin).
    enum class Hold { Off, Auto, All } hold = Hold::Auto;
    uint32_t graceMs = 30000;  // bounded: a hidden disconnect may make everyone wait at a sync point
    // Send EDF6's UnreliableUnordered game packets as ReliableUnordered (EOS and direct link).
    bool reliableGameTraffic = true;
    // Install newer releases from GitHub by itself (they run from the next game start). Also on for a
    // settings file without [Update] AutoUpdate= (written before auto-update): 0.3.6 already updated those
    // players, and turning it off now would leave them without later fixes. The log says how to opt out.
    bool autoUpdate = true;
    // Log lines about the settings file: values that were not understood, settings that are off.
    std::vector<std::string> warnings;
};

// Reads the INI; writes a commented default file first when it does not exist, commented in the
// Windows display language (Chinese, Japanese, otherwise English). The file is read as UTF-8 (with or
// without BOM; UTF-16 with BOM and the ANSI code page also work), `;` and `#` start comments, also
// after a value (`Mode=host ; me`), and a value that is not understood keeps its default (warned).
Config loadConfig(const std::wstring& iniPath);
// The default settings file for a Windows language id (LANGID).
std::string defaultIni(unsigned short langId);

// One key=value of a settings file, read the way loadConfig reads it (comment and quotes removed).
// `section` and `key` are as written; INI names compare without case.
struct IniValue {
    std::wstring section, key, value;
};
// Every key=value in the file at `path`, in file order; empty when there is no such file.
std::vector<IniValue> readIniValues(const std::wstring& path);
// `base` with the lines of `extra` added (both UTF-8 INI text): a section both have gets extra's lines at
// the end of base's, so it stays one section (the Windows profile functions read only the first of two
// with the same name); every other section of extra follows base's. Lines end in CRLF.
std::string mergeIniTexts(const std::string& base, const std::string& extra);
// `text` (UTF-8 INI) with every value of `values` written over the line of the same [section] key, the first
// value winning, as when reading. Values whose key `text` does not have are not written; each is listed in
// `dropped` as "[Section] Key=value".
std::string applyIniValues(const std::string& text, const std::vector<IniValue>& values,
                           std::vector<std::string>* dropped);
// Creates `path` holding `utf8` as UTF-16LE with BOM, the encoding both the Windows profile functions and
// this reader take without loss; false when the file exists already or cannot be written.
bool writeNewIniUtf16(const std::wstring& path, const std::string& utf8);

const char* modeName(Mode m);
const char* relayName(int relay);

}  // namespace dn
