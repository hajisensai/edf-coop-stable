#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace dn {

// Asks the router (UPnP IGD) to forward UDP `port` to `localIpv4`. Runs on a background thread and
// logs the outcome; `onPublicIpv4` runs (on that thread) with the router's WAN address when the
// mapping worked and that address is public.
void upnpMapUdpAsync(uint16_t port, const std::string& localIpv4,
                     std::function<void(const std::string&)> onPublicIpv4 = nullptr);

}  // namespace dn
