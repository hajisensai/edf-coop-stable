#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dn {

struct PhysicalInterface {
    std::string name;
    std::string description;
    uint32_t ifIndexV4 = 0;
    uint32_t ifIndexV6 = 0;
    std::string ipv4;                      // first IPv4 unicast address (LAN address behind the router)
    std::vector<std::string> globalIpv6;   // public IPv6 addresses, stable ones first
};

// The adapter that owns the real default gateway, ignoring VPN/TUN/virtual adapters.
std::optional<PhysicalInterface> findPhysicalInterface();

}  // namespace dn
