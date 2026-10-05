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

// Every packet that crossed the fake EOS, in order.
struct WirePacket {
    gamenet::PacketHeader header;
    std::string data;
};
inline std::vector<WirePacket> Wire(const gamenet::Network& network) {
    std::vector<WirePacket> packets;
    for (std::uint64_t at = 0; at + sizeof(gamenet::PacketHeader) <= network.wire.used;) {
        WirePacket packet;
        std::memcpy(&packet.header, network.wire.bytes + at, sizeof(packet.header));
        packet.data.assign(reinterpret_cast<const char*>(network.wire.bytes + at + sizeof(packet.header)), packet.header.size);
        at += sizeof(packet.header) + packet.header.size;
        packets.push_back(std::move(packet));
    }
    return packets;
}

inline void CheckLink(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    for (const auto& machine : machines) {
        int received = 0;
        for (auto it = machine.results.equal_range("received"); it.first != it.second; ++it.first)
            received += it.first->second.find("PROBE-PLAINTEXT-FROM-") != std::string::npos ? 1 : 0;
        Check(received == static_cast<int>(machines.size()) - 1,
              machine.user + " got every other machine's record through the game's controller");
    }
    const auto wire = Wire(network);
    bool plaintext = false, oversize = false;
    for (const auto& packet : wire) {
        plaintext = plaintext || packet.data.find("PROBE-PLAINTEXT") != std::string::npos;
        oversize = oversize || packet.header.size > gamenet::kMaxPacket;
    }
    Check(!wire.empty(), std::to_string(wire.size()) + " packets crossed EOS");
    Check(!plaintext, "no record crossed EOS in plaintext (the game encrypts every datagram)");
    Check(!oversize && network.refused == 0, "no packet above EOS's 1170 bytes was sent or refused");
    Check(network.wire.dropped == 0, "the wire log kept every packet");
}

// The mission start sync: everyone ends up with every player's record, byte for byte the same.
inline void CheckMission(const std::vector<Spawned>& machines, const gamenet::Network& network, bool split) {
    CheckRoom(machines, network);
    const int members = static_cast<int>(machines.size());
    for (const auto& machine : machines) {
        Check(Result(machine, "sync").rfind("done", 0) == 0, machine.user + " finished the sync: " + Result(machine, "sync"));
        Check(Result(machine, "players") == std::to_string(members),
              machine.user + " has " + std::to_string(members) + " players (has " + Result(machine, "players") + ")");
        Check(Result(machine, "mission") == "7 3", machine.user + " starts the host's mission 7 on difficulty 3 (has " +
                                                      Result(machine, "mission") + ")");
    }
    for (int slot = 0; slot < members; ++slot) {
        const std::string expected = std::to_string(slot) + " class=" + std::to_string(slot % 4) +
                                     " marker=" + std::to_string(500 + slot) + " armor=" + std::to_string(1000 + 37 * slot);
        std::string first;
        bool same = true;
        for (const auto& machine : machines) {
            std::string record;
            for (auto it = machine.results.equal_range("record"); it.first != it.second; ++it.first)
                if (it.first->second.rfind(std::to_string(slot) + " ", 0) == 0) record = it.first->second;
            Check(record.rfind(expected + " ", 0) == 0,
                  machine.user + " has player " + std::to_string(slot + 1) + "'s own loadout (" + record + ")");
            if (first.empty()) first = record;
            same = same && record == first;
        }
        Check(same, "every machine holds the same bytes for player " + std::to_string(slot + 1));
    }
    bool oversize = false;
    for (const auto& packet : Wire(network)) oversize = oversize || packet.header.size > gamenet::kMaxPacket;
    Check(!oversize && network.refused == 0, "no packet above EOS's 1170 bytes was sent or refused");
    const auto& host = machines.front();
    const bool sentBeside = host.text.find("sent beside the start message: result 0") != std::string::npos;
    Check(sentBeside == split, split ? "the host sent loadout records beside the start message"
                                     : "the start message fit: nothing was sent beside it");
    for (std::size_t i = 1; i < machines.size(); ++i) {
        const bool arrived = machines[i].text.find("arrived beside the start message") != std::string::npos;
        Check(arrived == split, machines[i].user + (split ? " got records beside the start message" : " needed no records beside it"));
        Check(machines[i].text.find("never arrived") == std::string::npos, machines[i].user + " missed no record");
    }
}
inline void CheckSplitMission(const std::vector<Spawned>& m, const gamenet::Network& n) { CheckMission(m, n, true); }
inline void CheckWholeMission(const std::vector<Spawned>& m, const gamenet::Network& n) { CheckMission(m, n, false); }

inline std::vector<Seat> Seats(int count, const std::string& step, const std::string& ini) {
    std::vector<Seat> seats;
    for (int i = 0; i < count; ++i) {
        char user[33];
        std::snprintf(user, sizeof(user), "%s%027d", i ? "guest" : "host0", i + 1);
        seats.push_back({user, std::string(i ? "guest-" : "host-") + step, ini});
    }
    return seats;
}

inline const std::vector<Scenario>& Scenarios() {
    static const std::vector<Scenario> all = {
        {"room",
         {{"host0000000000000000000000000001", "host-room", BaseIni()},
          {"guest000000000000000000000000002", "guest-room", BaseIni()}},
         60000,
         &CheckRoom},
        {"link",
         {{"host0000000000000000000000000001", "host-link", BaseIni()},
          {"guest000000000000000000000000002", "guest-link", BaseIni()}},
         60000,
         &CheckLink},
        // Two players, a test budget small enough that the second record goes beside the start message: what two
        // people testing the split by hand ran (SplitSyncBudget).
        {"mission2", Seats(2, "mission", BaseIni("[Test]\r\nSplitSyncBudget=200\r\n")), 90000, &CheckSplitMission},
        // Four players: the start message fits and is the game's own.
        {"mission4", Seats(4, "mission", BaseIni()), 90000, &CheckWholeMission},
        // Eight players: 1180 bytes would not fit EOS's 1170, so records go beside it.
        {"mission8", Seats(8, "mission", BaseIni()), 120000, &CheckSplitMission},
    };
    return all;
}

inline const Scenario* FindScenario(const std::string& name) {
    for (const auto& scenario : Scenarios())
        if (scenario.name == name) return &scenario;
    return nullptr;
}
