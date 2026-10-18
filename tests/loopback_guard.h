// Every UDP socket of a test process must be bound to a loopback address. Windows Defender Firewall asks for an
// administrator ("allow access") the first time a program listens on an address other machines can reach, and leaves
// a rule per program path behind: a test that binds the wildcard address pops that dialog on every new build folder.
//
// LoopbackGuard watches, while it lives, the UDP endpoints the system lists for the processes it is given (this one by
// default; GameNetTests adds its machines), every few milliseconds, and remembers every one bound elsewhere. A socket
// that lives shorter than one look can slip through; every DirectNet socket of a test lives far longer.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace testguard {

class LoopbackGuard {
public:
    LoopbackGuard() : thread_([this] { run(); }) { watch(GetCurrentProcessId()); }
    // Only stops the watcher: the last look and the result are stop()'s, which can allocate and throw.
    ~LoopbackGuard() { halt(); }
    LoopbackGuard(const LoopbackGuard&) = delete;
    LoopbackGuard& operator=(const LoopbackGuard&) = delete;

    void watch(DWORD pid) {
        std::lock_guard<std::mutex> lock(mu_);
        pids_.insert(pid);
    }
    // Stops watching (after one last look) and returns what was bound to an address that is not loopback.
    std::vector<std::string> stop() {
        if (halt()) look();
        std::lock_guard<std::mutex> lock(mu_);
        return {found_.begin(), found_.end()};
    }
    // Prints what stop() found; false when anything was.
    bool report(const char* what) {
        const std::vector<std::string> bad = stop();
        for (const std::string& b : bad) std::printf("FAIL: %s bound UDP %s, which other machines can reach\n", what, b.c_str());
        if (bad.empty()) std::printf("loopback guard: every UDP socket of %s was bound to loopback\n", what);
        return bad.empty();
    }

private:
    // Ends the watcher thread; true when it was running (a last look is then due). Never throws: a join that fails
    // (std::system_error) leaves the thread detached rather than ending the process from a destructor.
    bool halt() noexcept {
        done_ = true;
        if (!thread_.joinable()) return false;
        try {
            thread_.join();
        } catch (...) {
            thread_.detach();
        }
        return true;
    }
    void run() {
        while (!done_) {
            look();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    template <class Table, class Row>
    static std::vector<Row> rows(ULONG family) {
        std::vector<unsigned char> buffer(16384);
        for (int tries = 0; tries < 4; ++tries) {
            DWORD size = static_cast<DWORD>(buffer.size());
            const DWORD r = GetExtendedUdpTable(buffer.data(), &size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
            if (r == ERROR_INSUFFICIENT_BUFFER) {
                buffer.resize(size + 4096);
                continue;
            }
            if (r != NO_ERROR) return {};
            const auto* table = reinterpret_cast<const Table*>(buffer.data());
            return std::vector<Row>(table->table, table->table + table->dwNumEntries);
        }
        return {};
    }
    void look() {
        std::set<DWORD> pids;
        {
            std::lock_guard<std::mutex> lock(mu_);
            pids = pids_;
        }
        std::vector<std::string> bad;
        for (const MIB_UDPROW_OWNER_PID& row : rows<MIB_UDPTABLE_OWNER_PID, MIB_UDPROW_OWNER_PID>(AF_INET)) {
            if (!pids.count(row.dwOwningPid)) continue;
            in_addr a{};
            a.s_addr = row.dwLocalAddr;
            if ((ntohl(a.s_addr) >> 24) == 127) continue;
            char text[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &a, text, sizeof(text));
            bad.push_back(std::string(text) + ":" + std::to_string(ntohs(static_cast<u_short>(row.dwLocalPort))) +
                          " (process " + std::to_string(row.dwOwningPid) + ")");
        }
        for (const MIB_UDP6ROW_OWNER_PID& row : rows<MIB_UDP6TABLE_OWNER_PID, MIB_UDP6ROW_OWNER_PID>(AF_INET6)) {
            if (!pids.count(row.dwOwningPid)) continue;
            in6_addr a{};
            memcpy(a.s6_addr, row.ucLocalAddr, sizeof(a.s6_addr));
            if (IN6_IS_ADDR_LOOPBACK(&a)) continue;
            char text[INET6_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET6, &a, text, sizeof(text));
            bad.push_back("[" + std::string(text) + "]:" + std::to_string(ntohs(static_cast<u_short>(row.dwLocalPort))) +
                          " (process " + std::to_string(row.dwOwningPid) + ")");
        }
        if (bad.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        found_.insert(bad.begin(), bad.end());
    }

    std::mutex mu_;
    std::set<DWORD> pids_;
    std::set<std::string> found_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

}  // namespace testguard
