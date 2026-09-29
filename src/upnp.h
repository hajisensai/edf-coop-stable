#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dn {

// Asks the router (UPnP IGD) to forward UDP `port` to `localIpv4`. Runs on a background thread and
// logs the outcome; `onPublicIpv4` runs (on that thread) with the router's WAN address when the
// mapping worked and that address is usable from the internet (isPublicIpv4). The mapped port is
// written to `record`, so that it can be removed later (upnpRemoveRecordedAsync, the uninstaller); a
// mapping recorded for another port is removed first.
void upnpMapUdpAsync(uint16_t port, const std::string& localIpv4, const std::wstring& record,
                     std::function<void(const std::string&)> onPublicIpv4 = nullptr);

// Removes the mapping named in `record` (then the record itself) on a background thread, when it still
// forwards to this PC under our description. For starts that do not host through UPnP: the plugin
// has no safe point at game exit (see plugin.cpp), so a mapping lives until the next such start.
void upnpRemoveRecordedAsync(const std::wstring& record);

// The description every mapping of ours carries.
constexpr const char* kUpnpDescription = "EDF6DirectNet";

// An IPv4 address other players can reach: false for private (10/8, 172.16/12, 192.168/16),
// carrier-grade NAT (100.64/10), "this network" (0/8), loopback (127/8), link-local (169.254/16),
// IETF/benchmark ranges (192.0.0/24, 198.18/15), multicast and reserved (224/3, incl. broadcast) and
// anything that is not exactly four decimal octets.
bool isPublicIpv4(const std::string& text);
// RFC 1918 private or carrier-grade NAT: the router itself sits behind another NAT.
bool isBehindNatIpv4(const std::string& text);
// An existing router mapping may be replaced by ours only when it forwards to this PC; it may be
// removed only when, in addition, it carries our description.
bool upnpMayReplace(const std::string& internalClient, const std::vector<std::string>& localIpv4s);
bool upnpMayRemove(const std::string& internalClient, const std::string& description,
                   const std::vector<std::string>& localIpv4s);

}  // namespace dn
