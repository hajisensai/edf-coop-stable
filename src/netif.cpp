#include "netif.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cwctype>
#include <string>

namespace dn {
namespace {

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool looksVirtual(const IP_ADAPTER_ADDRESSES* a) {
    if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) return true;
    std::wstring text = std::wstring(a->Description ? a->Description : L"") + L" " +
                        std::wstring(a->FriendlyName ? a->FriendlyName : L"");
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return std::towlower(c); });
    for (const wchar_t* marker : {L"virtual", L"vpn", L"tap-", L"tun", L"wintun", L"clash", L"hyper-v", L"vmware",
                                  L"virtualbox", L"zerotier", L"tailscale", L"wireguard", L"loopback"}) {
        if (text.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

bool isGlobalIpv6(const sockaddr_in6* a) {
    uint8_t first = a->sin6_addr.u.Byte[0];
    return (first & 0xE0) == 0x20;  // 2000::/3 global unicast
}

}  // namespace

std::optional<PhysicalInterface> findPhysicalInterface() {
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return std::nullopt;

    const IP_ADAPTER_ADDRESSES* best = nullptr;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstGatewayAddress || looksVirtual(a)) continue;
        if (!best || a->Ipv4Metric < best->Ipv4Metric) best = a;
    }
    if (!best) return std::nullopt;

    PhysicalInterface pi;
    pi.name = narrow(best->FriendlyName);
    pi.description = narrow(best->Description);
    pi.ifIndexV4 = best->IfIndex;
    pi.ifIndexV6 = best->Ipv6IfIndex;
    std::vector<std::string> temporary;
    for (auto* u = best->FirstUnicastAddress; u; u = u->Next) {
        char text[INET6_ADDRSTRLEN] = {};
        const sockaddr* sa = u->Address.lpSockaddr;
        if (sa->sa_family == AF_INET && pi.ipv4.empty()) {
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, text, sizeof(text));
            pi.ipv4 = text;
        } else if (sa->sa_family == AF_INET6 && isGlobalIpv6(reinterpret_cast<const sockaddr_in6*>(sa))) {
            inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, text, sizeof(text));
            // Privacy (random) addresses rotate every day; list the stable address first.
            (u->SuffixOrigin == IpSuffixOriginRandom ? temporary : pi.globalIpv6).push_back(text);
        }
    }
    pi.globalIpv6.insert(pi.globalIpv6.end(), temporary.begin(), temporary.end());
    return pi;
}

}  // namespace dn
