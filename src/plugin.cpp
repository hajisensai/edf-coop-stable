// EDF6DirectNet: EDFModLoader plugin entry point.
#include <windows.h>

#include <cstdint>
#include <string>

#include "config.h"
#include "direct_net.h"
#include "eos_hooks.h"
#include "log.h"
#include "netif.h"
#include "upnp.h"

namespace {

constexpr uint32_t kVersionMajor = 0, kVersionMinor = 2, kVersionPatch = 0;
constexpr const char* kVersionText = "0.3.0";

// EDFModLoader's plugin info block (infoVersion 1): the loader rejects 0 and anything above 1.
struct PluginInfo {
    uint32_t infoVersion;
    const char* name;
    uint32_t version;
};

// Deliberately never destroyed: tearing down threads/Winsock from DllMain at process exit (loader
// lock held) deadlocks or crashes. The OS reclaims the socket; the host times the link out.
dn::DirectNet* g_net = nullptr;

std::wstring pluginDirectory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&pluginDirectory), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

void logInterface(const dn::PhysicalInterface& pi, const dn::Config& c) {
    dn::logf("NET physical adapter '%s' (%s), LAN IPv4 %s, ifIndex v4=%u v6=%u", pi.name.c_str(),
             pi.description.c_str(), pi.ipv4.empty() ? "-" : pi.ipv4.c_str(), pi.ifIndexV4, pi.ifIndexV6);
    for (const auto& v6 : pi.globalIpv6) {
        if (c.direct.mode == dn::Mode::Host)
            dn::logf("NET public IPv6 for friends: HostAddress=[%s]:%u", v6.c_str(), c.direct.listenPort);
        else
            dn::logf("NET public IPv6 %s", v6.c_str());
    }
    if (pi.globalIpv6.empty()) dn::logf("NET no public IPv6 address on this adapter");
}

void startDirect(dn::Config& c) {
    auto pi = dn::findPhysicalInterface();
    if (pi) {
        logInterface(*pi, c);
        if (c.bindPhysicalInterface) {
            c.direct.ifIndexV4 = pi->ifIndexV4;
            c.direct.ifIndexV6 = pi->ifIndexV6;
        }
    } else {
        dn::logf("NET could not identify the physical adapter; using normal OS routing");
    }
    if (c.direct.mode == dn::Mode::Off) return;
    if (c.direct.mode == dn::Mode::Join && c.direct.hostAddress.empty()) {
        dn::logf("DIRECT Mode=join but HostAddress is empty; direct link disabled");
        return;
    }
    if (c.direct.mode == dn::Mode::Host && c.direct.key.empty())
        dn::logf("DIRECT WARNING: hosting without Key= ; anyone who knows your address and a player's EOS id "
                 "can disturb that player's direct link. Set the same Key= for everyone in the room.");
    g_net = new dn::DirectNet();
    if (!g_net->start(c.direct)) {
        dn::logf("DIRECT failed to start (is UDP port %u already in use?); direct link disabled",
                 c.direct.listenPort);
        delete g_net;
        g_net = nullptr;
        return;
    }
    if (c.direct.mode == dn::Mode::Host && c.upnp && pi) dn::upnpMapUdpAsync(g_net->boundPort(), pi->ipv4);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    // At exit the worker thread is already killed (possibly holding a lock) and static objects are
    // about to be destroyed, while EDF.dll may still call EOS through our hooks.
    if (reason == DLL_PROCESS_DETACH) dn::eosHooksShutdown();
    return TRUE;
}

extern "C" __declspec(dllexport) bool EML6_Load(PluginInfo* info) {
    info->infoVersion = 1;
    info->name = "EDF6DirectNet";
    info->version = (kVersionMajor << 24) | (kVersionMinor << 16) | (kVersionPatch << 8);

    std::wstring dir = pluginDirectory();
    dn::Config config = dn::loadConfig(dir + L"EDF6DirectNet.ini");
    dn::logOpen(dir + L"EDF6DirectNet.log", 2 * 1024 * 1024);
    dn::logf("==== EDF6DirectNet %s starting: Mode=%s ListenPort=%u HostAddress=%s Key=%s EOS FixedPort=%u Relay=%s",
             kVersionText, dn::modeName(config.direct.mode), config.direct.listenPort,
             config.direct.hostAddress.empty() ? "-" : config.direct.hostAddress.c_str(),
             config.direct.key.empty() ? "no" : "yes", config.eosFixedPort, dn::relayName(config.eosRelay));
    if (!config.enabled) {
        dn::logf("disabled by Enabled=0");
        dn::logClose();
        return false;  // the loader unloads us
    }

    HMODULE game = GetModuleHandleW(L"EDF.dll");
    HMODULE eos = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
    if (!game || !eos) {
        dn::logf("EDF.dll or EOSSDK-Win64-Shipping.dll is not loaded; nothing to do");
        return true;
    }
    startDirect(config);
    if (!dn::installEosHooks(game, eos, config, g_net) && g_net) {
        g_net->stop();
        delete g_net;
        g_net = nullptr;
    }
    return true;
}
