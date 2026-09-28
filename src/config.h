#pragma once
#include <string>

#include "direct_net.h"

namespace dn {

struct Config {
    bool enabled = true;
    DirectOptions direct;
    bool upnp = true;
    bool bindPhysicalInterface = true;
    uint16_t eosFixedPort = 0;  // 0 = leave EOS on random ports
    int eosRelay = -1;          // -1 = leave default, 0 = no relays, 1 = allow, 2 = force
    bool holdDisconnects = true;  // hide transient EOS connection loss from the game while we reconnect
    uint32_t graceMs = 30000;
};

// Reads the INI; writes a commented default file first when it does not exist.
Config loadConfig(const std::wstring& iniPath);

const char* modeName(Mode m);
const char* relayName(int relay);

}  // namespace dn
