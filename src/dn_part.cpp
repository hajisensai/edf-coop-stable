// The direct-link part of EDF6Coop (dn_part.h).
#include "dn_part.h"

#include <cstdint>
#include <mutex>
#include <string>

#include "config.h"
#include "direct_net.h"
#include "eos_hooks.h"
#include "log.h"
#include "netif.h"
#include "product.h"
#include "upnp.h"

namespace {

// Deliberately never destroyed: tearing down threads/Winsock from DllMain at process exit (loader
// lock held) deadlocks or crashes. The OS reclaims the socket; the host times the link out.
dn::DirectNet* g_net = nullptr;

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

// Returns whether it asked the router for a UPnP port mapping (recorded in `upnpRecord`).
bool startDirect(dn::Config& c, const std::wstring& upnpRecord) {
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
    if (c.direct.mode == dn::Mode::Off) return false;
    if (c.direct.mode == dn::Mode::Join && c.direct.hostAddress.empty()) {
        dn::logf("DIRECT Mode=join but HostAddress is empty; direct link disabled");
        return false;
    }
    if (c.direct.mode == dn::Mode::Host && c.direct.key.empty())
        dn::logf("DIRECT hosting without Key= : players still prove who they are through the room, but direct-link "
                 "packets carry no tag, so someone on the network path could alter them. Set the same Key= for "
                 "everyone in the room to prevent that.");
    g_net = new dn::DirectNet();
    if (!g_net->start(c.direct)) {
        dn::logf("DIRECT failed to start (is UDP port %u already in use?); direct link disabled",
                 c.direct.listenPort);
        delete g_net;
        g_net = nullptr;
        return false;
    }
    if (c.direct.mode != dn::Mode::Host) return false;
    uint16_t port = g_net->boundPort();
    if (!c.publicAddress.empty()) {
        g_advertised->set(c.publicAddress, {});
        dn::logf("DIRECT players who join your room connect to %s (PublicAddress; UPnP skipped)",
                 c.publicAddress.c_str());
        return false;
    }
    std::string v6 = pi && !pi->globalIpv6.empty() ? "[" + pi->globalIpv6.front() + "]:" + std::to_string(port) : "";
    if (!v6.empty()) {
        g_advertised->set(v6, {});
        dn::logf("DIRECT players who join your room connect to %s (public IPv6)", v6.c_str());
    }
    if (c.upnp && pi) {
        dn::upnpMapUdpAsync(port, pi->ipv4, upnpRecord, [port](const std::string& wan) {
            std::string v4 = wan + ":" + std::to_string(port);
            g_advertised->set({}, v4);
            dn::logf("DIRECT players who join your room can also connect to %s (UPnP)", v4.c_str());
        });
        return true;
    }
    if (v6.empty())
        dn::logf("DIRECT WARNING: no public IPv6 and UPnP is off; set PublicAddress= to your public IP:port");
    return false;
}

}  // namespace

namespace dn {

PartState startPart(const Config& settings, const std::wstring& dir, HMODULE game, HMODULE eos) {
    Config config = settings;
    logf("direct link: Mode=%s ListenPort=%u HostAddress=%s Key=%s EOS FixedPort=%u Relay=%s",
         modeName(config.direct.mode), config.direct.listenPort,
         config.direct.hostAddress.empty() ? "-" : config.direct.hostAddress.c_str(),
         config.direct.key.empty() ? "no" : "yes", config.eosFixedPort, relayName(config.eosRelay));
    for (const std::string& warning : config.warnings) logf("%s", warning.c_str());
    if (!config.enabled) {
        logf("[DirectNet] Enabled=0: no direct link, and the game's EOS calls are left alone");
        return {};
    }
    if (!eos) {
        logf("EOSSDK-Win64-Shipping.dll is not loaded: no online play, so the direct link stays off");
        return {};
    }
    // A router mapping lives until a start that does not map it: at game exit there is no safe point
    // for the network calls removing it (DllMain runs under the loader lock with the other threads gone).
    std::wstring upnpRecord = dir + coop::kNameW + L".upnp";
    if (!startDirect(config, upnpRecord)) upnpRemoveRecordedAsync(upnpRecord);
    if (installEosHooks(game, eos, config, g_net)) return {true, true};
    if (g_net) {
        g_net->stop();
        delete g_net;
        g_net = nullptr;
    }
    return {true, false};  // the UPnP thread may still be at work
}

void detachPart() { eosHooksShutdown(); }

}  // namespace dn
