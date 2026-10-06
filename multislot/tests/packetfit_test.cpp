// The mission start message against EOS's packet size (packetfit.h), without the game: a stand-in record
// writer/reader shaped like 773840/773740 (a record starts with a number, never with a byte array), a byte stream
// laid out like the game's, and a fake EOS that records what is sent and hands back what is queued.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../src/log.h"
#include "../src/packetfit.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

// --- a byte stream like the game's (0x5F8 bytes: position at +8, data at +0x10, size at +0x5F0) ---
struct Stream {
    alignas(16) std::uint8_t raw[kStreamObjectSize]{};
    std::size_t Size() const {
        std::size_t size = 0;
        std::memcpy(&size, raw + kStreamSize, sizeof(size));
        return size;
    }
    std::size_t Position() const {
        std::size_t position = 0;
        std::memcpy(&position, raw + kStreamPosition, sizeof(position));
        return position;
    }
    void SetPosition(std::size_t position) { std::memcpy(raw + kStreamPosition, &position, sizeof(position)); }
    void Put(std::uint8_t byte) {
        std::size_t size = Size();
        raw[kStreamData + size++] = byte;
        std::memcpy(raw + kStreamSize, &size, sizeof(size));
    }
    std::uint8_t Get() {
        const std::size_t position = Position();
        SetPosition(position + 1);
        return raw[kStreamData + position];
    }
    std::vector<std::uint8_t> Bytes() const { return {raw + kStreamData, raw + kStreamData + Size()}; }
};

// --- a loadout record and its writer/reader, shaped like the game's ---
struct Record {
    std::int32_t index = -2;  // +0 like the game's record
    std::uint8_t length = 0;
    std::uint8_t payload[220]{};
};
struct Context {
    std::int32_t first = 0;
};

int writes = 0;
bool __fastcall FakeWrite(void* context, const void* record, void* stream, std::int32_t armorLimit) {
    ++writes;
    auto* s = static_cast<Stream*>(stream);
    const auto* r = static_cast<const Record*>(record);
    s->Put(static_cast<std::uint8_t>(static_cast<const Context*>(context)->first & 0x0F));  // a small int: 1 byte
    s->Put(static_cast<std::uint8_t>(r->index & 0x0F));
    s->Put(static_cast<std::uint8_t>(armorLimit & 0x0F));
    s->Put(r->length);
    for (std::size_t i = 0; i < r->length; ++i) s->Put(r->payload[i]);
    return true;
}

bool __fastcall FakeRead(void* context, void* record, void* stream) {
    auto* s = static_cast<Stream*>(stream);
    auto* r = static_cast<Record*>(record);
    static_cast<Context*>(context)->first = s->Get();
    r->index = s->Get();
    s->Get();  // armor limit
    r->length = s->Get();
    for (std::size_t i = 0; i < r->length; ++i) r->payload[i] = s->Get();
    return true;
}

Record MakeRecord(int index, std::size_t size) {
    Record record;
    record.index = index;
    record.length = static_cast<std::uint8_t>(size - 4);  // four bytes of header in FakeWrite
    for (std::size_t i = 0; i < record.length; ++i) record.payload[i] = static_cast<std::uint8_t>(index * 31 + i * 7 + 1);
    return record;
}

bool SameRecord(const Record& a, const Record& b) {
    return a.index == b.index && a.length == b.length && std::memcmp(a.payload, b.payload, a.length) == 0;
}

// The host side of one sync: header, then each record through the redirected write, then the hook after the loop.
std::vector<std::uint8_t> HostMessage(const std::vector<Record>& records, Stream& stream) {
    for (int header : {1, 2, 3}) stream.Put(static_cast<std::uint8_t>(header));           // three ints
    stream.Put(static_cast<std::uint8_t>(records.size()));                         // the player count
    Context context{5};
    for (const auto& record : records) RecordWriteHook(&context, &record, &stream, 9);
    FlushRecords(&stream);
    return stream.Bytes();
}

// What the game itself would write. Record by record: twelve of them run past a stream, as they would in the game.
std::vector<std::uint8_t> DirectMessage(const std::vector<Record>& records) {
    std::vector<std::uint8_t> message = {1, 2, 3, static_cast<std::uint8_t>(records.size())};
    Context context{5};
    for (const auto& record : records) {
        Stream stream;
        FakeWrite(&context, &record, &stream, 9);
        const auto bytes = stream.Bytes();
        message.insert(message.end(), bytes.begin(), bytes.end());
    }
    return message;
}

// --- fake EOS ---
struct Sent {
    std::uint8_t channel;
    std::int32_t reliability;
    std::int32_t delayed;
    const void* remote;
    std::vector<std::uint8_t> bytes;
};
std::vector<Sent> sent;
EosResult FakeSend(void*, const EosSendOptions* options) {
    const auto* data = static_cast<const std::uint8_t*>(options->Data);
    sent.push_back({options->Channel, options->Reliability, options->AllowDelayedDelivery, options->RemoteUserId,
                    {data, data + options->DataLengthBytes}});
    return 0;
}

struct Incoming {
    Incoming(std::vector<std::uint8_t> b, void* p = nullptr, std::uint8_t c = 0) : bytes(std::move(b)), peer(p), channel(c) {}
    std::vector<std::uint8_t> bytes;
    void* peer;
    std::uint8_t channel;
};
std::deque<Incoming> incoming;
constexpr EosResult kNotFound = 18;  // EOS_NotFound
struct SocketId {  // EOS_P2P_SocketId
    std::int32_t ApiVersion;
    char SocketName[33];
};
struct ReceiveOptions {  // EOS_P2P_ReceivePacketOptions
    std::int32_t ApiVersion;
    const void* LocalUserId;
    std::uint32_t MaxDataSizeBytes;
    const std::uint8_t* RequestedChannel;
};
EosResult FakeReceive(void*, const void*, void** peer, void* socket, std::uint8_t* channel, void* data, std::uint32_t* size) {
    if (incoming.empty()) return kNotFound;
    const auto packet = incoming.front();
    incoming.pop_front();
    std::memcpy(data, packet.bytes.data(), packet.bytes.size());
    *size = static_cast<std::uint32_t>(packet.bytes.size());
    if (peer) *peer = packet.peer;
    if (socket) {
        auto* id = static_cast<SocketId*>(socket);
        id->ApiVersion = 1;
        std::snprintf(id->SocketName, sizeof(id->SocketName), "GAME%d", packet.channel);
    }
    if (channel) *channel = packet.channel;
    return 0;
}

// Who reads a split sync (syncmarker.cpp in the plugin).
std::vector<const void*> readers;
bool FakeReaders(const void* remote) { return std::find(readers.begin(), readers.end(), remote) != readers.end(); }

unsigned long long fakeNow = 1000;
unsigned long long FakeClock() { return fakeNow; }

// The game's datagram around a message: 8 + 12 header bytes and a few bytes of message headers, the payload
// encrypted (AES-CTR, 12CEA10): what passes the EOS wrappers never shows a stub. Here a XOR stands for the cipher.
std::vector<std::uint8_t> GamePacket(const std::vector<std::uint8_t>& message) {
    std::vector<std::uint8_t> packet = {0x5A, 0xA5, 0, 0, 0, 0, 0, 0, 0x00, 0x12, 0x40, 0x00, 7, 0, 0, 0, 0, 0, 0, 0,
                                        0x21, 0x04, 0x02, 0x00, 0xA4, 0x40};
    for (const std::uint8_t byte : message) packet.push_back(static_cast<std::uint8_t>(byte ^ 0x5A));
    return packet;
}

// MissionSync_Update reading every record of `message` back; `left` counts the players left out.
bool ReadsBack(const std::vector<std::uint8_t>& message, const std::vector<Record>& records, int* left = nullptr) {
    Stream reader;
    std::memcpy(reader.raw + kStreamData, message.data(), message.size());
    const std::size_t messageSize = message.size();
    std::memcpy(reader.raw + kStreamSize, &messageSize, sizeof(messageSize));
    reader.SetPosition(4);
    bool same = true;
    for (const auto& original : records) {
        Record back;
        Context context{};
        RecordReadHook(&context, &back, &reader);
        if (left && back.index == -1)
            ++*left;
        else
            same = same && SameRecord(back, original) && context.first == 5;
    }
    return same && reader.Position() == message.size();
}

const ReceiveOptions kAnyChannel{2, nullptr, 0x1000, nullptr};

// The game's receive, once: what RecordReadHook then receives with.
void GameReceivesNothing() {
    std::vector<std::uint8_t> buffer(0x1000);
    std::uint32_t size = 0;
    std::uint8_t channel = 0;
    void* from = nullptr;
    SocketId socket{};
    PacketFitReceive(nullptr, &kAnyChannel, &from, &socket, &channel, buffer.data(), &size);
}

void TestPlan() {
    std::vector<RecordRef> seven;
    for (int i = 0; i < 7; ++i) seven.push_back({i, 143});
    const auto sevenPlan = PlanInline(6, seven, kMissionSyncBudget);
    Check(std::all_of(sevenPlan.begin(), sevenPlan.end(), [](bool b) { return b; }), "seven players' records all stay inline");

    std::vector<RecordRef> eight = seven;
    eight.push_back({7, 143});
    const auto eightPlan = PlanInline(6, eight, kMissionSyncBudget);
    Check(std::count(eightPlan.begin(), eightPlan.end(), false) == 1 && !eightPlan[7],
          "eight players (1150 bytes): only player index 7 goes beside the message");

    // Written in the order the replies came: the highest indices still go first.
    std::vector<RecordRef> twelve;
    for (int i : {3, 11, 0, 8, 5, 1, 10, 7, 2, 9, 4, 6}) twelve.push_back({i, 143});
    const auto twelvePlan = PlanInline(6, twelve, kMissionSyncBudget);
    std::size_t total = 6;
    std::vector<int> moved;
    for (std::size_t i = 0; i < twelve.size(); ++i) {
        total += twelvePlan[i] ? twelve[i].size : kStubBytes;
        if (!twelvePlan[i]) moved.push_back(twelve[i].index);
    }
    std::sort(moved.begin(), moved.end());
    Check(total <= kMissionSyncBudget && moved == std::vector<int>({6, 7, 8, 9, 10, 11}),
          "twelve players: indices 6-11 go beside the message and it fits");

    // A room of four never changes, whatever its records hold (222 bytes is the most one can take).
    std::vector<RecordRef> four;
    for (int i = 0; i < 4; ++i) four.push_back({i, 222});
    const auto fourPlan = PlanInline(20, four, kMissionSyncBudget);
    Check(std::all_of(fourPlan.begin(), fourPlan.end(), [](bool b) { return b; }), "four players always stay inline");

    // Every room up to sixteen fits the budget, whatever the record sizes; players 1-4 only move when 5+ are not enough.
    unsigned seed = 12345;
    for (int round = 0; round < 2000; ++round) {
        const int players = 1 + static_cast<int>((seed = seed * 1103515245u + 12345u) >> 16) % 16;
        std::vector<RecordRef> records;
        for (int i = 0; i < players; ++i) records.push_back({i, 22 + ((seed = seed * 1103515245u + 12345u) >> 16) % 201});
        std::reverse(records.begin(), records.end());
        const std::size_t header = 4 + ((seed = seed * 1103515245u + 12345u) >> 16) % 20;
        const auto plan = PlanInline(header, records, kMissionSyncBudget);
        std::size_t size = header, extrasInline = 0;
        bool lowMoved = false;
        for (std::size_t i = 0; i < records.size(); ++i) {
            size += plan[i] ? records[i].size : kStubBytes;
            if (plan[i] && records[i].index >= 4) ++extrasInline;
            if (!plan[i] && records[i].index < 4) lowMoved = true;
        }
        if (size > kMissionSyncBudget || (lowMoved && extrasInline)) {
            Check(false, "every plan fits the budget and moves players 5+ first");
            break;
        }
    }
}

void TestStubsAndSidePackets() {
    const Record record = MakeRecord(9, 143);
    Stream serialised;
    Context context{5};
    FakeWrite(&context, &record, &serialised, 9);
    const auto bytes = serialised.Bytes();
    StubInfo stub;
    stub.index = 9;
    stub.size = bytes.size();
    stub.hash = RecordHash(bytes.data(), bytes.size());
    std::uint8_t written[kStubBytes];
    WriteStub(written, stub);
    StubInfo back;
    Check(written[0] == 0xA0 && written[1] == kStubBytes - 2 && ParseStub(written, sizeof(written), back) &&
              back.index == 9 && back.size == stub.size && back.hash == stub.hash,
          "a stub is a 19-byte byte array that reads back");
    Check(!ParseStub(written, sizeof(written) - 1, back), "a cut stub is not one");
    // A record starts with a number (tags 0x00-0x9F); none of them reads as a stub.
    bool anyNumber = false;
    for (int tag = 0; tag < 0xA0; ++tag) {
        std::uint8_t probe[kStubBytes];
        std::memcpy(probe, written, sizeof(probe));
        probe[0] = static_cast<std::uint8_t>(tag);
        anyNumber = anyNumber || ParseStub(probe, sizeof(probe), back);
    }
    Check(!anyNumber, "no number tag reads as a stub");

    std::vector<std::uint8_t> noise(300);
    for (std::size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<std::uint8_t>(i * 37 + 11);
    noise.insert(noise.begin() + 100, written, written + kStubBytes);
    noise.insert(noise.begin() + 200, written, written + kStubBytes);
    StubInfo found[4];
    Check(FindStubs(noise.data(), noise.size(), found, 4) == 2 && found[1].hash == stub.hash, "stubs are found inside a packet");
    Check(FindStubs(noise.data(), 100 + kStubBytes - 1, found, 4) == 0, "a stub cut by the packet end is not found");

    std::uint8_t packet[kSideHeader + kMaxRecordBytes];
    const std::size_t length = BuildSidePacket(stub, bytes.data(), packet, sizeof(packet));
    StubInfo side;
    const std::uint8_t* sideRecord = nullptr;
    Check(length == kSideHeader + bytes.size() && IsSidePacket(packet, length) &&
              ParseSidePacket(packet, length, side, sideRecord) && side.hash == stub.hash &&
              std::memcmp(sideRecord, bytes.data(), bytes.size()) == 0,
          "a side packet carries the record and its hash");
    packet[kSideHeader + 5] ^= 0x40;
    Check(!ParseSidePacket(packet, length, side, sideRecord), "a side packet whose bytes do not match its hash is refused");
    packet[kSideHeader + 5] ^= 0x40;
    Check(!ParseSidePacket(packet, length - 1, side, sideRecord), "a cut side packet is refused");
    Check(!IsSidePacket(written, sizeof(written)), "a stub is no side packet");

    ClearRecords();
    Check(StoreRecord(stub, bytes.data()) && !StoreRecord(stub, bytes.data()), "a record is stored once");
    std::vector<std::uint8_t> copy(bytes.size());
    Check(FindRecord(stub, copy.data()) && copy == bytes, "a stored record is found by its stub");
    for (std::size_t i = 0; i < kRecordStoreEntries; ++i) {
        std::uint8_t other[8] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i >> 8), 2, 3, 4, 5, 6, 7};
        StubInfo filler{static_cast<int>(i & 0x0F), sizeof(other), RecordHash(other, sizeof(other))};
        StoreRecord(filler, other);
    }
    Check(!FindRecord(stub, nullptr), "the oldest record makes room after a full store of others");
    ClearRecords();
}

EosSendOptions SendTo(const void* remote, const std::vector<std::uint8_t>& packet);

void TestRoundTrip(int players, bool expectMoved) {
    std::vector<Record> records;
    for (int i : {0, 3, 1, 2, 4, 6, 5, 7, 9, 8, 11, 10}) {
        if (i < players) records.push_back(MakeRecord(i, 143));
    }
    const std::string label = std::to_string(players) + " players: ";
    ClearRecords();
    sent.clear();
    writes = 0;
    Stream host;
    const auto message = HostMessage(records, host);
    const auto direct = DirectMessage(records);
    Check(writes == 2 * players, (label + "every record goes through the game's writer once").c_str());
    StubInfo stubs[16];
    const std::size_t stubCount = FindStubs(message.data(), message.size(), stubs, 16);
    if (!expectMoved) {
        Check(message == direct && stubCount == 0, (label + "the message is byte for byte the game's").c_str());
    } else {
        Check(message.size() <= kMissionSyncBudget && stubCount > 0,
              (label + "the message fits and names the records sent beside it").c_str());
        Check(direct.size() > kMissionSyncBudget, (label + "the game's own message would not have fit").c_str());
    }
    const auto packet = GamePacket(message);
    Check(packet.size() <= kEosMaxPacket || !expectMoved, (label + "the packet fits EOS").c_str());
    Check(FindStubs(packet.data(), packet.size(), stubs, 16) == 0, (label + "no stub shows in the encrypted packet").c_str());

    // Host sends: the records the sync moved out go first, reliable, on their own channel; then the game's packet as
    // it was. Each member gets them once.
    const int peer = 42;
    readers = {&peer};
    auto options = SendTo(&peer, packet);
    Check(PacketFitSend(nullptr, &options) == 0, (label + "send passes the result on").c_str());
    Check(sent.size() == stubCount + 1 && sent.back().bytes == packet && sent.back().channel == 0,
          (label + "one side packet per record moved, then the game's packet unchanged").c_str());
    for (std::size_t i = 0; i + 1 < sent.size(); ++i)
        Check(sent[i].channel == kSideChannel && sent[i].reliability == 2 && sent[i].delayed == 1 && sent[i].remote == &peer &&
                  IsSidePacket(sent[i].bytes.data(), sent[i].bytes.size()),
              (label + "side packets are reliable, to the same peer, on the side channel").c_str());
    const auto sides = std::vector<Sent>(sent.begin(), sent.end() - 1);
    sent.clear();
    PacketFitSend(nullptr, &options);
    Check(sent.size() == 1 && sent[0].bytes == packet, (label + "a member gets them once").c_str());

    // A member: nothing stored yet. The sync arrives before its side packets (they travel apart) and goes to the game
    // as it came - nothing here can read it. The game decrypts it and reads a stub: the record is waited for,
    // received from EOS meanwhile.
    ClearRecords();
    incoming.clear();
    int sender = 7;
    incoming.push_back({packet, &sender, 0});
    for (const auto& side : sides) incoming.push_back({side.bytes, &sender, kSideChannel});
    std::vector<std::uint8_t> buffer(0x1000);
    std::uint32_t size = 0;
    std::uint8_t channel = 0xFF;
    void* from = nullptr;
    SocketId socket{};
    EosResult result = PacketFitReceive(nullptr, &kAnyChannel, &from, &socket, &channel, buffer.data(), &size);
    Check(result == 0 && size == packet.size() && std::memcmp(buffer.data(), packet.data(), size) == 0 && from == &sender &&
              channel == 0 && std::strcmp(socket.SocketName, "GAME0") == 0,
          (label + "the game receives the sync as it came, from its sender").c_str());
    Check(ReadsBack(message, records), (label + "every record reads back as written, the moved ones waited for").c_str());
    Check(incoming.empty() && HeldPacketCount() == 0 &&
              PacketFitReceive(nullptr, &kAnyChannel, &from, &socket, &channel, buffer.data(), &size) == kNotFound,
          (label + "and side packets never reach the game").c_str());

    if (stubCount) {
        // Were a record never to arrive, that player is left out and the rest still line up.
        ClearRecords();
        int left = 0;
        Check(ReadsBack(message, records, &left) && left == static_cast<int>(stubCount),
              (label + "a missing record leaves only its player out").c_str());
    }
    sent.clear();
}

// The 8-player sync, split, as the host sends it.
std::vector<std::uint8_t> SplitPacket() {
    std::vector<Record> records;
    for (int i = 0; i < 8; ++i) records.push_back(MakeRecord(i, 143));
    Stream host;
    return GamePacket(HostMessage(records, host));
}

EosSendOptions SendTo(const void* remote, const std::vector<std::uint8_t>& packet) {
    EosSendOptions options{};
    options.ApiVersion = 3;
    options.RemoteUserId = remote;
    options.Data = packet.data();
    options.DataLengthBytes = static_cast<std::uint32_t>(packet.size());
    return options;
}

// The records of a split sync go to every member ahead of the first packet to it, marker or not: the sync reaches
// them anyway (it is encrypted), so holding records back only left a member waiting for them.
int askedReaders = 0;
bool CountingReaders(const void* remote) {
    ++askedReaders;
    return FakeReaders(remote);
}

void TestSendToEveryMember() {
    ClearRecords();
    const auto packet = SplitPacket();
    const int marked = 1, unmarked = 2;
    readers = {&marked};
    SetSplitSyncReaders(&CountingReaders);
    askedReaders = 0;
    sent.clear();
    auto options = SendTo(&unmarked, packet);
    Check(PacketFitSend(nullptr, &options) == 0 && sent.size() == 2 && sent[0].channel == kSideChannel &&
              sent[1].bytes == packet,
          "a member without the marker gets the side packet, then the game's packet");
    options = SendTo(&marked, packet);
    Check(PacketFitSend(nullptr, &options) == 0 && sent.size() == 4 && sent[2].channel == kSideChannel &&
              sent[3].bytes == packet,
          "so does a member with it");
    Check(askedReaders == 2, "the marker is asked about each member, for the log only");
    options = SendTo(&marked, packet);
    Check(PacketFitSend(nullptr, &options) == 0 && sent.size() == 5 && sent[4].bytes == packet,
          "each member gets the records once");
    sent.clear();
    SetSplitSyncReaders(nullptr);
    ClearRecords();
    const auto again = SplitPacket();
    options = SendTo(&marked, again);
    Check(PacketFitSend(nullptr, &options) == 0 && sent.size() == 2 && sent[0].channel == kSideChannel,
          "without the lobby marker the records go all the same");
    SetSplitSyncReaders(&FakeReaders);
    sent.clear();
    ClearRecords();
}

// Side packets of the 8-player sync as the host sends them, and the sync's message.
std::vector<std::vector<std::uint8_t>> Sides(std::vector<std::uint8_t>& message) {
    std::vector<Record> records;
    for (int i = 0; i < 8; ++i) records.push_back(MakeRecord(i, 143));
    Stream host;
    message = HostMessage(records, host);
    const auto packet = GamePacket(message);
    const int peer = 1;
    readers = {&peer};
    sent.clear();
    auto options = SendTo(&peer, packet);
    PacketFitSend(nullptr, &options);
    std::vector<std::vector<std::uint8_t>> sides;
    for (std::size_t i = 0; i + 1 < sent.size(); ++i) sides.push_back(sent[i].bytes);
    sent.clear();
    return sides;
}

// What the game reads one stub with; true when its record was there in the end.
bool ReadStub(const std::vector<std::uint8_t>& message) {
    std::vector<Record> records;
    for (int i = 0; i < 8; ++i) records.push_back(MakeRecord(i, 143));
    int left = 0;
    return ReadsBack(message, records, &left) && left == 0;
}

// Game packets received while a record is waited for are the game's: held, and handed to it in order, as it asks.
void TestHold() {
    SetPacketFitClock(&FakeClock);
    ClearRecords();
    std::vector<std::uint8_t> message;
    const auto sides = Sides(message);
    ClearRecords();  // the member knows nothing yet
    GameReceivesNothing();
    std::vector<std::uint8_t> buffer(0x1000);
    std::uint32_t size = 0;
    std::uint8_t channel = 0;
    void* from = nullptr;
    SocketId socket{};
    const std::vector<std::uint8_t> first(300, 0x11), second(200, 0x22);

    incoming.clear();
    incoming.push_back({first, nullptr, 3});
    incoming.push_back({second, nullptr, 0});
    for (const auto& side : sides) incoming.push_back({side, nullptr, kSideChannel});
    Check(ReadStub(message) && HeldPacketCount() == 2, "the record is waited for; the game's packets meanwhile are held");
    const std::uint8_t other = 4, three = 3;
    const ReceiveOptions wrongChannel{2, nullptr, 0x1000, &other};
    Check(PacketFitReceive(nullptr, &wrongChannel, &from, &socket, &channel, buffer.data(), &size) == kNotFound &&
              HeldPacketCount() == 2,
          "a held packet is not handed to a caller asking for another channel");
    const ReceiveOptions small{2, nullptr, 100, nullptr};
    Check(PacketFitReceive(nullptr, &small, &from, &socket, &channel, buffer.data(), &size) == kNotFound,
          "nor to one without room for it");
    const ReceiveOptions right{2, nullptr, 0x1000, &three};
    Check(PacketFitReceive(nullptr, &right, &from, &socket, &channel, buffer.data(), &size) == 0 && channel == 3 &&
              size == first.size() && buffer[0] == 0x11,
          "but to the next caller that asks for it");
    Check(PacketFitReceive(nullptr, &kAnyChannel, &from, &socket, &channel, buffer.data(), &size) == 0 &&
              size == second.size() && buffer[0] == 0x22 && HeldPacketCount() == 0,
          "and the next one after it");

    // However many come while the game's frame waits, none is dropped (EOS will not send them again): all reach the
    // game, in order.
    ClearRecords();
    GameReceivesNothing();
    incoming.clear();
    constexpr std::size_t kMany = 1500;
    for (std::size_t i = 0; i < kMany; ++i) {
        std::vector<std::uint8_t> numbered(64, 0);
        numbered[0] = static_cast<std::uint8_t>(i);
        numbered[1] = static_cast<std::uint8_t>(i >> 8);
        incoming.push_back({numbered, nullptr, 0});
    }
    Check(!ReadStub(message) && HeldPacketCount() == kMany, "every packet that came while the frame waited is held");
    bool inOrder = true;
    for (std::size_t i = 0; i < kMany; ++i)
        inOrder = inOrder && PacketFitReceive(nullptr, &kAnyChannel, &from, &socket, &channel, buffer.data(), &size) == 0 &&
                  (buffer[0] | buffer[1] << 8) == static_cast<int>(i);
    Check(inOrder && HeldPacketCount() == 0, "and every one reaches the game, oldest first");
    ClearRecords();
    Check(HeldPacketCount() == 0, "leaving the room drops held packets");

    // Not given up however long the game takes to read it.
    GameReceivesNothing();
    incoming.clear();
    incoming.push_back({first, nullptr, 0});
    ReadStub(message);
    fakeNow += 10 * 60 * 1000;
    Check(PacketFitReceive(nullptr, &kAnyChannel, &from, &socket, &channel, buffer.data(), &size) == 0 &&
              size == first.size() && HeldPacketCount() == 0,
          "a packet held for minutes still reaches the game");
    SetPacketFitClock(nullptr);
    ClearRecords();
    incoming.clear();
}

// --- holding a member's packets behind its records bulk (packetfit.h BulkIncoming) ---
int bulkPeer = 0, otherPeer = 0;
std::uint64_t bulkInFlight = 0;  // what the fragment layer says of bulkPeer
std::size_t bulkSize = 0;
std::uint64_t FakeIncoming(const void* peer, std::size_t* total) {
    if (peer != &bulkPeer || !bulkInFlight) return 0;
    if (total) *total = bulkSize;
    return bulkInFlight;
}

std::vector<std::uint8_t> Numbered(std::size_t i) {
    std::vector<std::uint8_t> bytes(40, 0xEE);
    bytes[0] = static_cast<std::uint8_t>(i);
    bytes[1] = static_cast<std::uint8_t>(i >> 8);
    return bytes;
}

// The game's receive, until EOS has nothing: every packet it got, as numbered, with the member it came from.
std::vector<std::pair<int, const void*>> GameReadsAll() {
    std::vector<std::pair<int, const void*>> got;
    std::vector<std::uint8_t> buffer(0x1000);
    std::uint32_t size = 0;
    std::uint8_t channel = 0;
    void* from = nullptr;
    SocketId socket{};
    while (PacketFitReceive(nullptr, &kAnyChannel, &from, &socket, &channel, buffer.data(), &size) == 0)
        got.emplace_back(buffer[0] | buffer[1] << 8, from);
    return got;
}

bool Sequence(const std::vector<std::pair<int, const void*>>& got, const void* peer, int from, int to) {
    int next = from;
    for (const auto& [n, who] : got)
        if (who == peer) {
            if (n != next) return false;
            ++next;
        }
    return next == to;
}

void TestBulkHold() {
    SetPacketFitClock(&FakeClock);
    SetBulkRecords(nullptr, nullptr, &FakeIncoming);
    ClearRecords();
    incoming.clear();
    bulkSize = 1200;
    Check(BulkHoldMs(143 * 1024) <= 6000 && BulkHoldMs(1200) == kRecordWaitMs + 2 * kBulkWaitMsPerKiB &&
              BulkHoldCapacity(1200) >= kBulkHoldMinPackets,
          "a room of 1024's bulk (143 KiB) is waited for at most 6 s: far inside the ~26 s the game resends for");

    // While the bulk is on its way the member's packets wait; another member's reach the game at once.
    bulkInFlight = 0x1111;
    for (int i = 0; i < 5; ++i) incoming.push_back({Numbered(i), &bulkPeer, 0});
    incoming.push_back({Numbered(100), &otherPeer, 0});
    auto got = GameReadsAll();
    Check(got.size() == 1 && got[0].second == &otherPeer && HeldPacketCount() == 5,
          "packets of a member whose bulk is on its way wait; the others' do not");
    // Its sender gave up (the fragment layer lets the bulk go): every held packet reaches the game in order, and
    // the member's later packets are not held at all.
    bulkInFlight = 0;
    for (int i = 5; i < 8; ++i) incoming.push_back({Numbered(i), &bulkPeer, 0});
    got = GameReadsAll();
    Check(Sequence(got, &bulkPeer, 0, 8) && HeldPacketCount() == 0,
          "once the bulk is no longer on its way, every held packet goes to the game in order, then the new ones");
    // The sender's resend starts the same bulk over at the receiver: its member's packets are not held again.
    bulkInFlight = 0x1111;
    incoming.push_back({Numbered(8), &bulkPeer, 0});
    got = GameReadsAll();
    Check(got.size() == 1 && got[0].first == 8 && HeldPacketCount() == 0, "a bulk let go once holds nothing again");

    // A new bulk that arrives: the packets held behind it go once it is here.
    bulkInFlight = 0x2222;
    for (int i = 0; i < 3; ++i) incoming.push_back({Numbered(i), &bulkPeer, 0});
    Check(GameReadsAll().empty() && HeldPacketCount() == 3, "a new bulk holds the member's packets");
    bulkInFlight = 0;
    got = GameReadsAll();
    Check(Sequence(got, &bulkPeer, 0, 3), "and they go once it arrived");

    // More packets than the bulk may hold back: the bulk is let go, not one packet is dropped.
    bulkInFlight = 0x3333;
    const std::size_t cap = BulkHoldCapacity(bulkSize);
    for (std::size_t i = 0; i < cap + 10; ++i) incoming.push_back({Numbered(i), &bulkPeer, 0});
    got = GameReadsAll();
    Check(Sequence(got, &bulkPeer, 0, static_cast<int>(cap + 10)) && HeldPacketCount() == 0,
          "past BulkHoldCapacity the bulk is let go and every packet reaches the game, in order");

    // Held as long as the bulk takes at 32 KiB/s, then let go (the start message waits for the records instead).
    bulkInFlight = 0x4444;
    for (int i = 0; i < 4; ++i) incoming.push_back({Numbered(i), &bulkPeer, 0});
    Check(GameReadsAll().empty(), "held behind the bulk");
    fakeNow += BulkHoldMs(bulkSize) - 1;
    Check(GameReadsAll().empty() && HeldPacketCount() == 4, "still held just before BulkHoldMs");
    fakeNow += 1;
    got = GameReadsAll();
    Check(Sequence(got, &bulkPeer, 0, 4) && HeldPacketCount() == 0, "and let go, every one in order, at BulkHoldMs");

    // A packet that came while the game's frame waited for a record is held behind the bulk as well: nothing of
    // that member overtakes what waits behind its bulk.
    std::vector<std::uint8_t> message;
    Sides(message);  // a start message whose records went beside it
    ClearRecords();  // and never came: reading its stub waits (record wait 0: one pass over what EOS has)
    bulkInFlight = 0x5555;
    incoming.push_back({Numbered(0), &bulkPeer, 0});
    Check(GameReadsAll().empty(), "held behind the bulk");
    incoming.push_back({Numbered(1), &bulkPeer, 0});
    incoming.push_back({Numbered(200), &otherPeer, 0});
    ReadStub(message);
    got = GameReadsAll();
    Check(got.size() == 1 && got[0].second == &otherPeer && HeldPacketCount() == 2,
          "what the frame received meanwhile keeps its member's order behind the bulk");
    bulkInFlight = 0;
    Check(Sequence(GameReadsAll(), &bulkPeer, 0, 2), "and follows it once the bulk is here");

    SetBulkRecords(nullptr, nullptr, nullptr);
    SetPacketFitClock(nullptr);
    ClearRecords();
    incoming.clear();
}

void TestOversizeDiagnostic() {
    std::vector<std::uint8_t> packet(1180, 0x11);
    const std::uint8_t header[] = {0x00, 0x12, 0x40, 0x00};
    std::memcpy(packet.data() + 8, header, 4);
    packet[16] = 0x01;
    Check(FirstOversize(0x12C8C5A, 0, packet.data(), packet.size()), "an oversize packet kind is logged the first time");
    packet[1] = 0x99;  // the datagram header's random bytes do not make a new kind
    Check(!FirstOversize(0x12C8C5A, 0, packet.data(), packet.size()), "and not again");
    packet[16] = 0x02;
    Check(FirstOversize(0x12C8C5A, 0, packet.data(), packet.size()), "another packet type is another kind");
    Check(FirstOversize(0x12C8C5B, 0, packet.data(), packet.size()), "another caller is another kind");
    char text[16];
    const std::uint8_t bytes[] = {0x00, 0xAB, 0x12, 0xFF, 0x77};
    DescribeBytes(bytes, sizeof(bytes), text, sizeof(text));
    Check(std::strcmp(text, "00 AB 12 FF 77") == 0, "first bytes are written as hex");
    DescribeBytes(bytes, sizeof(bytes), text, 8);
    Check(std::strcmp(text, "00 AB") == 0, "and cut to the buffer");
}

std::vector<std::string> redirected;
bool FakeRedirect(HMODULE, const char* dll, const char* function, void*, void** original) {
    redirected.push_back(std::string(dll) + "!" + function);
    *original = reinterpret_cast<void*>(std::strcmp(function, "EOS_P2P_SendPacket") == 0 ? reinterpret_cast<void*>(&FakeSend)
                                                                                             : reinterpret_cast<void*>(&FakeReceive));
    return true;
}

std::string ReadLog(const std::wstring& path) {
    LogFlush();
    std::string text;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
        std::fclose(file);
    }
    return text;
}

std::size_t Count(const std::string& text, const char* needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

void TestLogLines(const std::wstring& path) {
    const std::string log = ReadLog(path);
    // Eight players: the round trip, then the send (twice), hold and bulk hold tests build that sync again; records
    // sent in between keep the log from folding any of them.
    Check(Count(log, "MISSION sync: 8 loadout records would make the start message") == 5 &&
              Count(log, "1 of them are sent beside it") == 5 && Count(log, "12 loadout records") == 1,
          "the host logs each sync that sends records beside the message");
    Check(Count(log, "MISSION sync: 7 loadout records") == 0, "a sync that fits logs nothing");
    Check(Count(log, "loadout record of player index 7 sent beside the start message: result 0") > 0,
          "the host logs each record it sends");
    Check(Count(log, "arrived beside the start message") > 0 && Count(log, "for the loadout record of player index 7: here") > 0,
          "a member logs the records that arrive and each wait for one");
    Check(Count(log, "never arrived; that player is left out") > 0 && Count(log, "still missing") > 0,
          "and a record that never came");
    Check(Count(log, "held game packet(s) dropped") == 0 && Count(log, "the oldest is dropped") == 0,
          "no held packet is ever dropped");
    Check(Count(log, "its game packets wait behind it") > 0 && Count(log, "the bulk arrived, or its sender gave it up") > 0 &&
              Count(log, "more of them came than the bulk may hold back") == 1 &&
              Count(log, "they waited as long as the bulk takes at 32 KiB/s") == 1 &&
              Count(log, "every held game packet reached the game") > 0,
          "holding behind a bulk, and each way it ends, is logged");
    std::vector<std::uint8_t> packet(1181, 0x33);
    LogOversizePacket(0x12C8C5A, 0, 0, packet.data(), 1170, 0);
    LogOversizePacket(0x5000, 1, 0, packet.data(), 1181, 1);
    LogOversizePacket(0x5000, 1, 0, packet.data(), 1181, 1);
    const std::string after = ReadLog(path);
    Check(Count(after, "NETLOG OVERSIZE") == 1 &&
              Count(after, "NETLOG OVERSIZE game packet of 1181 bytes (EOS takes 1170): channel=1 reliability=0 "
                           "caller=EDF+5000 result=1 first bytes 33 33") == 1,
          "an oversize packet is logged once with size, channel, caller and first bytes; 1170 is not oversize");
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring logPath;
    if (argc > 1) {
        for (const char* c = argv[1]; *c; ++c) logPath.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
        DeleteFileW(logPath.c_str());
        LogOpen(logPath.c_str());
    }
    SetRecordFunctions(&FakeWrite, &FakeRead);
    SetSplitSyncReaders(&FakeReaders);
    SetRecordWait(0);  // only what has arrived: the fake EOS has everything there at once
    Check(InstallPacketFit(nullptr, &FakeRedirect) == 2 && redirected.size() == 2 &&
              redirected[0] == "EOSSDK-Win64-Shipping.dll!EOS_P2P_ReceivePacket" &&
              redirected[1] == "EOSSDK-Win64-Shipping.dll!EOS_P2P_SendPacket",
          "the two EOS P2P imports are redirected");
    Check(PacketFitCallHandler(0x78D6FA) && PacketFitCallHandler(0x790873) && PacketFitHookHandler(0x78D756) &&
              !PacketFitCallHandler(0x78D6E3),
          "handlers exist for the three sites and only them");
    TestPlan();
    TestStubsAndSidePackets();
    TestRoundTrip(4, false);
    TestRoundTrip(7, false);
    TestRoundTrip(8, true);
    TestRoundTrip(10, true);
    TestRoundTrip(12, true);
    TestSendToEveryMember();
    TestHold();
    TestBulkHold();
    TestOversizeDiagnostic();
    if (!logPath.empty()) TestLogLines(logPath);
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("mission start message fits EOS for up to 12 players\n");
    return 0;
}
