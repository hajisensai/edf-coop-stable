#pragma once
#include <cstdint>
#include <string>

namespace dn {

// Asks the router (UPnP IGD) to forward UDP `port` to `localIpv4`. Runs on a background thread and
// only logs the outcome, including the router's public IPv4 address when it reports one.
void upnpMapUdpAsync(uint16_t port, const std::string& localIpv4);

}  // namespace dn
