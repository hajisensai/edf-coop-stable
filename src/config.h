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

const char* modeName(Mode m);
const char* relayName(int relay);

}  // namespace dn
