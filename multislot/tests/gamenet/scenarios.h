#pragma once
// The rooms GameNetTests plays (included by gamenet_test.cpp, after Spawned, Check and Result).

struct Seat {
    std::string user;  // EOS ProductUserId text
    std::string role;  // roles.cpp
    std::string ini;   // its EDF6Coop.ini
};

struct Scenario {
    std::string name;
    std::vector<Seat> seats;
    DWORD timeoutMs;
    void (*check)(const std::vector<Spawned>& machines, const gamenet::Network& network);
};

// What every machine runs with: the room part on, no update checks, no direct link (the game's own EOS P2P).
inline std::string BaseIni(const std::string& extra = "") {
    return "[MultiSlot]\r\nEnabled=1\r\nEightPlayerRooms=1\r\nMaxPlayers=8\r\nCrashLog=0\r\nNetLog=1\r\n"
           "[DirectNet]\r\nEnabled=0\r\n"
           "[Update]\r\nAutoUpdate=0\r\nCheckEDF6VR=0\r\n" +
           extra;
}

inline void CheckRoom(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    for (const auto& machine : machines) {
        int marked = 0;
        for (auto it = machine.results.equal_range("member"); it.first != it.second; ++it.first)
            marked += it.first->second.find(" marked") != std::string::npos ? 1 : 0;
        Check(marked == static_cast<int>(machines.size()),
              machine.user + " sees all " + std::to_string(machines.size()) + " members with the split sync marker");
        Check(machine.results.count("unimplemented-eos") == 0,
              machine.user + " reached no EOS function the fake only stubs (first: " + Result(machine, "unimplemented-eos") + ")");
    }
    Check(network.lobby.count == machines.size(), "EOS has every machine in the room");
}

inline const std::vector<Scenario>& Scenarios() {
    static const std::vector<Scenario> all = {
        {"room",
         {{"host0000000000000000000000000001", "host-room", BaseIni()},
          {"guest000000000000000000000000002", "guest-room", BaseIni()}},
         60000,
         &CheckRoom},
    };
    return all;
}

inline const Scenario* FindScenario(const std::string& name) {
    for (const auto& scenario : Scenarios())
        if (scenario.name == name) return &scenario;
    return nullptr;
}
