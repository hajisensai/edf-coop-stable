#include "upnp.h"

#include <windows.h>
#include <natupnp.h>
#include <oleauto.h>

#include <cstdio>
#include <string>
#include <thread>

#include "log.h"

namespace dn {
namespace {

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string narrow(BSTR b) {
    if (!b) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, b, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, b, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// RFC 1918 private ranges plus RFC 6598 carrier-grade NAT (100.64.0.0/10).
bool isPrivateIpv4(const std::string& text) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf_s(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127);
}

void mapPort(uint16_t port, const std::string& localIpv4) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    IUPnPNAT* nat = nullptr;
    IStaticPortMappingCollection* mappings = nullptr;
    IStaticPortMapping* mapping = nullptr;
    BSTR proto = SysAllocString(L"UDP");
    BSTR client = SysAllocString(widen(localIpv4).c_str());
    BSTR desc = SysAllocString(L"EDF6DirectNet");
    HRESULT hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT),
                                  reinterpret_cast<void**>(&nat));
    if (SUCCEEDED(hr)) hr = nat->get_StaticPortMappingCollection(&mappings);
    if (SUCCEEDED(hr) && mappings) {
        // Replace an older mapping of the same port (it may point at a previous LAN address).
        mappings->Remove(port, proto);
        hr = mappings->Add(port, proto, port, client, VARIANT_TRUE, desc, &mapping);
    }
    if (SUCCEEDED(hr) && mapping) {
        BSTR external = nullptr;
        mapping->get_ExternalIPAddress(&external);
        std::string ext = narrow(external);
        logf("UPNP router now forwards UDP %u -> %s:%u; router WAN address: %s", port, localIpv4.c_str(), port,
             ext.empty() ? "(not reported)" : ext.c_str());
        if (isPrivateIpv4(ext)) {
            logf("UPNP WARNING: the router WAN address %s is private (carrier-grade NAT). Friends cannot reach "
                 "your IPv4; use your public IPv6 address instead.",
                 ext.c_str());
        }
        SysFreeString(external);
    } else if (SUCCEEDED(hr)) {
        // The UPnP API itself worked but found no router offering a port-mapping service.
        logf("UPNP no router with UPnP port mapping found (UPnP disabled or unsupported on the router). "
             "Forward UDP %u to %s manually, or use IPv6.",
             port, localIpv4.c_str());
    } else {
        logf("UPNP port mapping failed (hr=0x%08lx). Forward UDP %u to %s manually, or use IPv6.",
             static_cast<unsigned long>(hr), port, localIpv4.c_str());
    }
    if (mapping) mapping->Release();
    if (mappings) mappings->Release();
    if (nat) nat->Release();
    SysFreeString(proto);
    SysFreeString(client);
    SysFreeString(desc);
    CoUninitialize();
}

}  // namespace

void upnpMapUdpAsync(uint16_t port, const std::string& localIpv4) {
    if (localIpv4.empty()) return;
    std::thread([port, localIpv4] { mapPort(port, localIpv4); }).detach();
}

}  // namespace dn
