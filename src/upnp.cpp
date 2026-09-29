#include "upnp.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <natupnp.h>
#include <oleauto.h>

#include <algorithm>
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

// Exactly four decimal octets ("1.2.3.4": no leading zeros, spaces or trailing text).
bool parseIpv4(const std::string& text, unsigned octet[4]) {
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        if (i > 0 && (at >= text.size() || text[at++] != '.')) return false;
        size_t start = at;
        unsigned v = 0;
        while (at < text.size() && at - start < 3 && text[at] >= '0' && text[at] <= '9') v = v * 10 + (text[at++] - '0');
        if (at == start || v > 255 || (at - start > 1 && text[start] == '0')) return false;
        octet[i] = v;
    }
    return at == text.size();
}

// Every IPv4 address this PC has on any adapter.
std::vector<std::string> localIpv4Addresses() {
    std::vector<std::string> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char text[INET_ADDRSTRLEN] = {};
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) out.push_back(text);
        }
    }
    return out;
}

// The port a previous mapping of ours was recorded with; 0 when there is none.
uint16_t recordedPort(const std::wstring& record) {
    FILE* f = _wfopen(record.c_str(), L"rb");
    if (!f) return 0;
    unsigned port = 0;
    if (fscanf_s(f, "UDP %u", &port) != 1 || port > 65535) port = 0;
    fclose(f);
    return static_cast<uint16_t>(port);
}

void writeRecord(const std::wstring& record, uint16_t port) {
    FILE* f = _wfopen(record.c_str(), L"wb");
    if (!f) return;
    fprintf(f, "UDP %u\r\n", port);
    fclose(f);
}

// COM, the router's mapping list and our BSTRs for one piece of work on a background thread.
class Router {
public:
    Router() {
        com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (!com_) return;
        hr_ = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat_));
        if (SUCCEEDED(hr_)) hr_ = nat_->get_StaticPortMappingCollection(&mappings_);
    }
    ~Router() {
        if (mappings_) mappings_->Release();
        if (nat_) nat_->Release();
        SysFreeString(proto_);
        if (com_) CoUninitialize();
    }
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    bool ready() const { return com_; }
    HRESULT hr() const { return hr_; }
    IStaticPortMappingCollection* mappings() const { return mappings_; }
    BSTR proto() const { return proto_; }

    // The mapping of UDP `port`: false when there is none.
    bool find(uint16_t port, std::string* client, std::string* description) const {
        IStaticPortMapping* existing = nullptr;
        if (!mappings_ || FAILED(mappings_->get_Item(port, proto_, &existing)) || !existing) return false;
        BSTR owner = nullptr, what = nullptr;
        existing->get_InternalClient(&owner);
        existing->get_Description(&what);
        *client = narrow(owner);
        *description = narrow(what);
        SysFreeString(owner);
        SysFreeString(what);
        existing->Release();
        return true;
    }

    // Removes our mapping of `port` (forwarding to this PC under our description); leaves others.
    void removeOurs(uint16_t port, const std::vector<std::string>& locals) const {
        std::string client, description;
        if (!find(port, &client, &description)) return;
        if (!upnpMayRemove(client, description, locals)) {
            logf("UPNP UDP %u now forwards to %s (\"%s\"), not to this PC as set up by EDF6DirectNet; left alone", port,
                 client.c_str(), description.c_str());
            return;
        }
        HRESULT hr = mappings_->Remove(port, proto_);
        if (SUCCEEDED(hr))
            logf("UPNP removed the UDP %u forwarding to %s that EDF6DirectNet set up earlier", port, client.c_str());
        else
            logf("UPNP could not remove the UDP %u forwarding (hr=0x%08lx)", port, static_cast<unsigned long>(hr));
    }

private:
    bool com_ = false;
    HRESULT hr_ = E_FAIL;
    IUPnPNAT* nat_ = nullptr;
    IStaticPortMappingCollection* mappings_ = nullptr;
    BSTR proto_ = SysAllocString(L"UDP");
};

void mapPort(uint16_t port, const std::string& localIpv4, const std::wstring& record,
             const std::function<void(const std::string&)>& onPublicIpv4) {
    Router router;
    if (!router.ready()) return;
    std::vector<std::string> locals = localIpv4Addresses();
    IStaticPortMapping* mapping = nullptr;
    HRESULT hr = router.hr();
    bool taken = false;
    if (SUCCEEDED(hr) && router.mappings()) {
        uint16_t previous = recordedPort(record);
        if (previous != 0 && previous != port) router.removeOurs(previous, locals);
        // Replace an older mapping of the same port only when it forwards to this PC (e.g. a stale one of
        // ours). One to another PC, e.g. someone else in the house hosting on the same port, must not be
        // touched, whatever its description says.
        std::string ownerIp, ownerDesc;
        if (router.find(port, &ownerIp, &ownerDesc)) {
            taken = !upnpMayReplace(ownerIp, locals);
            if (taken)
                logf("UPNP UDP %u is already forwarded to %s (\"%s\"); left alone. Set another ListenPort, "
                     "or forward a port manually and set PublicAddress.",
                     port, ownerIp.c_str(), ownerDesc.c_str());
            else
                router.mappings()->Remove(port, router.proto());
        }
        if (!taken) {
            BSTR client = SysAllocString(widen(localIpv4).c_str());
            BSTR desc = SysAllocString(widen(kUpnpDescription).c_str());
            hr = router.mappings()->Add(port, router.proto(), port, client, VARIANT_TRUE, desc, &mapping);
            SysFreeString(client);
            SysFreeString(desc);
        }
    }
    if (SUCCEEDED(hr) && mapping) {
        writeRecord(record, port);
        BSTR external = nullptr;
        mapping->get_ExternalIPAddress(&external);
        std::string ext = narrow(external);
        logf("UPNP router now forwards UDP %u -> %s:%u; router WAN address: %s", port, localIpv4.c_str(), port,
             ext.empty() ? "(not reported)" : ext.c_str());
        if (isBehindNatIpv4(ext)) {
            logf("UPNP WARNING: the router WAN address %s is private (carrier-grade NAT). Friends cannot reach "
                 "your IPv4; use your public IPv6 address instead.",
                 ext.c_str());
        } else if (!ext.empty() && !isPublicIpv4(ext)) {
            logf("UPNP WARNING: the router WAN address %s cannot be reached from the internet; it is not advertised. "
                 "Use your public IPv6 address, or set PublicAddress.",
                 ext.c_str());
        } else if (!ext.empty() && onPublicIpv4) {
            onPublicIpv4(ext);
        }
        SysFreeString(external);
    } else if (taken) {
        // already logged
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
}

void removeRecorded(const std::wstring& record) {
    uint16_t port = recordedPort(record);
    Router router;
    if (!router.ready() || FAILED(router.hr()) || !router.mappings()) return;  // not at that router now: keep it
    if (port != 0) router.removeOurs(port, localIpv4Addresses());
    DeleteFileW(record.c_str());
}

}  // namespace

bool isPublicIpv4(const std::string& text) {
    unsigned o[4];
    if (!parseIpv4(text, o) || isBehindNatIpv4(text)) return false;
    return !(o[0] == 0 || o[0] == 127 || o[0] >= 224 || (o[0] == 169 && o[1] == 254) ||
             (o[0] == 192 && o[1] == 0 && o[2] == 0) || (o[0] == 198 && (o[1] == 18 || o[1] == 19)));
}

bool isBehindNatIpv4(const std::string& text) {
    unsigned o[4];
    if (!parseIpv4(text, o)) return false;
    return o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168) ||
           (o[0] == 100 && o[1] >= 64 && o[1] <= 127);
}

bool upnpMayReplace(const std::string& internalClient, const std::vector<std::string>& localIpv4s) {
    return !internalClient.empty() && std::find(localIpv4s.begin(), localIpv4s.end(), internalClient) != localIpv4s.end();
}

bool upnpMayRemove(const std::string& internalClient, const std::string& description,
                   const std::vector<std::string>& localIpv4s) {
    return description == kUpnpDescription && upnpMayReplace(internalClient, localIpv4s);
}

void upnpMapUdpAsync(uint16_t port, const std::string& localIpv4, const std::wstring& record,
                     std::function<void(const std::string&)> onPublicIpv4) {
    if (localIpv4.empty()) return;
    std::thread([port, localIpv4, record, onPublicIpv4] { mapPort(port, localIpv4, record, onPublicIpv4); }).detach();
}

void upnpRemoveRecordedAsync(const std::wstring& record) {
    if (GetFileAttributesW(record.c_str()) == INVALID_FILE_ATTRIBUTES) return;  // never mapped: no router traffic
    std::thread([record] { removeRecorded(record); }).detach();
}

}  // namespace dn
