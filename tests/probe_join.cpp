// Real-machine probe: joins a running EDF6DirectNet host and reports what it sees.
// Usage: probe_join.exe [host:port] [key]
#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "../src/direct_net.h"

int main(int argc, char** argv) {
    dn::DirectOptions o;
    o.mode = dn::Mode::Join;
    o.listenPort = 0;
    o.hostAddress = argc > 1 ? argv[1] : "127.0.0.1:27015";
    o.key = argc > 2 ? argv[2] : "";
    dn::DirectNet net;
    if (!net.start(o)) {
        printf("PROBE could not open a UDP socket\n");
        return 2;
    }
    net.setLocalUser("probe000000000000000000000000000");
    for (int i = 0; i < 100; ++i) {  // up to 10 s
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto members = net.directMembers();
        if (!members.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // let a few pings measure RTT
            printf("PROBE connected: %s\n", net.statusLine().c_str());
            for (const auto& m : net.directMembers()) printf("PROBE roster member %s\n", m.c_str());
            net.stop();
            return 0;
        }
    }
    printf("PROBE no welcome from %s within 10 s: %s\n", o.hostAddress.c_str(), net.statusLine().c_str());
    net.stop();
    return 1;
}
