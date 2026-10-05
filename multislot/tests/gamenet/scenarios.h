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
    };
    return all;
}

inline const Scenario* FindScenario(const std::string& name) {
    for (const auto& scenario : Scenarios())
        if (scenario.name == name) return &scenario;
    return nullptr;
}
