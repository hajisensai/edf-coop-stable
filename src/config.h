#pragma once
#include <string>

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
};

// Reads the INI; writes a commented default file first when it does not exist.
Config loadConfig(const std::wstring& iniPath);

const char* modeName(Mode m);
const char* relayName(int relay);

}  // namespace dn
