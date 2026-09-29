// EDF6DirectNet: EDFModLoader plugin entry point.
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "config.h"
#include "direct_net.h"
#include "eos_hooks.h"
#include "log.h"
#include "netif.h"
#include "upnp.h"

namespace {

constexpr uint32_t kVersionMajor = 0, kVersionMinor = 3, kVersionPatch = 0;
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

void logInterface(const dn::PhysicalInterface& pi) {
    dn::logf("NET physical adapter '%s' (%s), LAN IPv4 %s, ifIndex v4=%u v6=%u", pi.name.c_str(),
             pi.description.c_str(), pi.ipv4.empty() ? "-" : pi.ipv4.c_str(), pi.ifIndexV4, pi.ifIndexV6);
    for (const auto& v6 : pi.globalIpv6) dn::logf("NET public IPv6 %s", v6.c_str());
    if (pi.globalIpv6.empty()) dn::logf("NET no public IPv6 address on this adapter");
}

// What a host advertises in the lobby: PublicAddress as written, else its public IPv6 plus the
// router's public IPv4 once UPnP has mapped the port.
class Advertised {
public:
    void set(std::string ipv6, std::string ipv4) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!ipv6.empty()) ipv6_ = std::move(ipv6);
        if (!ipv4.empty()) ipv4_ = std::move(ipv4);
        std::string all = ipv4_ + (!ipv4_.empty() && !ipv6_.empty() ? " " : "") + ipv6_;
        dn::setAdvertisedAddress(all);
    }

private:
    std::mutex mu_;
    std::string ipv6_, ipv4_;
};
Advertised* g_advertised = new Advertised();  // never destroyed, like g_net

void startDirect(dn::Config& c) {
    auto pi = dn::findPhysicalInterface();
    if (pi) {
        logInterface(*pi);
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
    if (c.direct.mode != dn::Mode::Host) return;
    uint16_t port = g_net->boundPort();
    if (!c.publicAddress.empty()) {
        g_advertised->set(c.publicAddress, {});
        dn::logf("DIRECT players who join your room connect to %s (PublicAddress; UPnP skipped)",
                 c.publicAddress.c_str());
        return;
    }
    std::string v6 = pi && !pi->globalIpv6.empty() ? "[" + pi->globalIpv6.front() + "]:" + std::to_string(port) : "";
    if (!v6.empty()) {
        g_advertised->set(v6, {});
        dn::logf("DIRECT players who join your room connect to %s (public IPv6)", v6.c_str());
    }
    if (c.upnp && pi)
        dn::upnpMapUdpAsync(port, pi->ipv4, [port](const std::string& wan) {
            std::string v4 = wan + ":" + std::to_string(port);
            g_advertised->set({}, v4);
            dn::logf("DIRECT players who join your room can also connect to %s (UPnP)", v4.c_str());
        });
    else if (v6.empty())
        dn::logf("DIRECT WARNING: no public IPv6 and UPnP is off; set PublicAddress= to your public IP:port");
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
