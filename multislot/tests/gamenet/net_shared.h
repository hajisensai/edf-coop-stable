#pragma once
// The network of the game-code tests: one named shared memory section that every machine's fake EOS
// (fake_eos_net.cpp) and the test driver (gamenet_test.cpp) map. It holds the room (one EOS lobby) and an inbox per
// machine. A packet is written straight into its receiver's inbox, so nothing has to route it, and the driver can
// read every packet that crossed the wire afterwards (the wire log).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace gamenet {

constexpr std::uint32_t kMagic = 0x4E364445;  // "ED6N"
constexpr int kMaxMachines = 8;
constexpr int kMaxAttributes = 16;
constexpr std::size_t kUserText = 33;
constexpr std::size_t kSocketText = 33;
constexpr std::size_t kKeyText = 64;
constexpr std::size_t kValueText = 256;
constexpr std::size_t kInboxBytes = 1 << 20;
constexpr std::size_t kWireLogBytes = 8 << 20;
constexpr std::uint32_t kMaxPacket = 1170;  // EOS_P2P_MAX_PACKET_SIZE: EOS refuses anything larger

// Environment variables the driver hands each machine.
constexpr const char* kSectionVariable = "EDF6NET_SECTION";  // the section's name
constexpr const char* kUserVariable = "EDF6NET_USER";        // this machine's EOS ProductUserId text
// Imperfections of the network, as a scenario asks for them:
// "<channel>:<ms>" - packets of that channel reach their receiver that much later than the others (EOS keeps the
// order of packets only within a channel);
constexpr const char* kDelayVariable = "EDF6NET_DELAY";
// "<bytes>:<count>" - each machine loses its first <count> unreliable packets of at least <bytes> (EOS delivers
// unreliable packets at most once; the game resends what it needs).
constexpr const char* kDropVariable = "EDF6NET_DROP";
// "<ms>" - a member's attribute reaches the other members that much later (Epic's lobby service relays them).
constexpr const char* kLobbyDelayVariable = "EDF6NET_LOBBY_DELAY";

struct Attribute {
    char key[kKeyText];
    std::int32_t type;  // 0 none, 1 int64, 4 UTF-8 (EOS_ELobbyAttributeType: 0 bool, 1 int64, 2 double, 3 string)
    std::int64_t number;
    char text[kValueText];
    std::uint64_t visibleAt;  // GetTickCount64 from which the other members see it (EDF6NET_LOBBY_DELAY)
};

struct Member {
    char user[kUserText];
    Attribute attributes[kMaxAttributes];
};

struct Lobby {
    char id[kKeyText];
    char owner[kUserText];
    std::uint32_t maxMembers;
    std::uint32_t count;
    Member members[kMaxMachines];
    std::uint32_t version;  // bumped on every change
};

// One packet as it travels: a header, then `size` bytes.
struct PacketHeader {
    char from[kUserText];
    char to[kUserText];
    char socket[kSocketText];
    std::uint8_t channel;
    std::uint8_t reliability;
    std::uint32_t size;
};

struct Ring {
    std::uint64_t head;  // read
    std::uint64_t tail;  // written
    std::uint8_t bytes[kInboxBytes];
};

struct Station {
    char user[kUserText];
    std::uint32_t present;
    Ring inbox;
};

struct WireLog {
    std::uint64_t used;
    std::uint64_t dropped;  // packets that no longer fit the log
    std::uint8_t bytes[kWireLogBytes];
};

struct Network {
    std::uint32_t magic;
    std::uint32_t refused;   // packets EOS refused for their size (above kMaxPacket)
    std::uint32_t overflowed;  // packets refused because their receiver's inbox was full
    std::uint32_t dropped;   // unreliable packets the network lost on purpose (EDF6NET_DROP)
    std::uint32_t finished;  // machines done with their part (FakeNet_Finish)
    Lobby lobby;
    Station machines[kMaxMachines];
    WireLog wire;
};

inline std::wstring SectionName(const char* name) {
    std::wstring wide(L"Local\\");
    for (const char* c = name; *c; ++c) wide += static_cast<wchar_t>(*c);
    return wide;
}
inline std::wstring LockName(const char* name) { return SectionName(name) + L"-lock"; }

// Opens (or creates, for the driver) the section; null on failure.
inline Network* MapNetwork(const char* name, bool create, HANDLE* section, HANDLE* lock) {
    const std::wstring sectionName = SectionName(name), lockName = LockName(name);
    *section = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Network),
                                           sectionName.c_str())
                      : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sectionName.c_str());
    if (!*section) return nullptr;
    *lock = create ? CreateMutexW(nullptr, FALSE, lockName.c_str()) : OpenMutexW(SYNCHRONIZE, FALSE, lockName.c_str());
    if (!*lock) return nullptr;
    auto* network = static_cast<Network*>(MapViewOfFile(*section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Network)));
    if (network && create) {
        std::memset(network, 0, sizeof(Network));
        network->magic = kMagic;
    }
    return network && network->magic == kMagic ? network : nullptr;
}

struct Locked {
    explicit Locked(HANDLE lock) : lock_(lock) { WaitForSingleObject(lock_, INFINITE); }
    ~Locked() { ReleaseMutex(lock_); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;

private:
    HANDLE lock_;
};

inline std::uint64_t RingFree(const Ring& ring) { return kInboxBytes - (ring.tail - ring.head); }

inline void RingWrite(Ring& ring, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) ring.bytes[(ring.tail + i) % kInboxBytes] = bytes[i];
    ring.tail += size;
}

inline void RingPeek(const Ring& ring, std::uint64_t at, void* out, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(out);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = ring.bytes[(at + i) % kInboxBytes];
}

// Appends a packet to the wire log (the driver reads it after the machines are done).
inline void LogWire(WireLog& wire, const PacketHeader& header, const void* data) {
    const std::size_t size = sizeof(header) + header.size;
    if (wire.used + size > kWireLogBytes) {
        ++wire.dropped;
        return;
    }
    std::memcpy(wire.bytes + wire.used, &header, sizeof(header));
    std::memcpy(wire.bytes + wire.used + sizeof(header), data, header.size);
    wire.used += size;
}

}  // namespace gamenet
