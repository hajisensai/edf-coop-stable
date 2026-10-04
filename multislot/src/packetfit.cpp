#include "packetfit.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <numeric>

#include "log.h"

namespace multislot {
namespace {

constexpr std::uint8_t kStubMagic[8] = {'M', 'S', 'l', 'o', 't', 'R', 'e', 'c'};
constexpr std::uint8_t kSideMagic[8] = {'M', 'S', 'l', 'o', 't', 'S', 'i', 'd'};
constexpr std::uint8_t kByteArrayTag = 0xA0;  // 12B5200: 0xA0 | length >> 8, then the low byte of the length
constexpr std::uint8_t kStubLength = static_cast<std::uint8_t>(kStubBytes - 2);
static_assert(kStubBytes == 2 + sizeof(kStubMagic) + 1 + 2 + 8, "stub layout");
static_assert(kSideHeader == sizeof(kSideMagic) + 1 + 2 + 8, "side packet layout");

void Put16(std::uint8_t* out, std::size_t value) {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
}
std::size_t Get16(const std::uint8_t* in) { return static_cast<std::size_t>(in[0]) | static_cast<std::size_t>(in[1]) << 8; }
void Put64(std::uint8_t* out, std::uint64_t value) { std::memcpy(out, &value, sizeof(value)); }
std::uint64_t Get64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    std::memcpy(&value, in, sizeof(value));
    return value;
}

// index, size, hash after a magic
void PutInfo(std::uint8_t* out, const StubInfo& stub) {
    out[0] = static_cast<std::uint8_t>(stub.index);
    Put16(out + 1, stub.size);
    Put64(out + 3, stub.hash);
}
StubInfo GetInfo(const std::uint8_t* in) {
    StubInfo stub;
    stub.index = in[0];
    stub.size = Get16(in + 1);
    stub.hash = Get64(in + 3);
    return stub;
}

// --- records sent beside a sync ---
struct Entry {
    bool used = false;
    StubInfo stub;
    std::uint8_t bytes[kMaxRecordBytes];
};
Entry entries[kRecordStoreEntries];
std::size_t nextEntry = 0;
SRWLOCK storeLock = SRWLOCK_INIT;

Entry* Lookup(const StubInfo& stub) {
    for (auto& entry : entries)
        if (entry.used && entry.stub.hash == stub.hash && entry.stub.size == stub.size && entry.stub.index == stub.index)
            return &entry;
    return nullptr;
}

// --- game glue ---
RecordWriteFn writeRecord = nullptr;
RecordReadFn readRecord = nullptr;

struct Pending {
    std::int32_t index;
    std::size_t size;
    std::uint8_t bytes[kMaxRecordBytes];
};
// One record per player of a sync: the whole record loop runs within one MissionSync_Res call.
constexpr std::size_t kBatchRecords = std::max<std::size_t>(32, kMaxPlayers);
// MissionSync_Res runs on the game thread, and the whole record loop runs within one call of it.
struct Batch {
    void* stream = nullptr;
    std::size_t count = 0;
    Pending records[kBatchRecords];
} batch;

std::uint8_t* Bytes(void* p) { return static_cast<std::uint8_t*>(p); }
std::size_t StreamSize(const void* stream) {
    std::size_t size = 0;
    std::memcpy(&size, static_cast<const std::uint8_t*>(stream) + kStreamSize, sizeof(size));
    return size;
}

// The game's writers append without a bound; this one refuses to run past the buffer.
bool Append(void* stream, const std::uint8_t* data, std::size_t size) {
    const std::size_t used = StreamSize(stream);
    if (used > kStreamCapacity || size > kStreamCapacity - used) {
        Log("MISSION sync: %zu more bytes would run past the game's %zu-byte stream (holds %zu); not written", size,
            kStreamCapacity, used);
        return false;
    }
    std::memcpy(Bytes(stream) + kStreamData + used, data, size);
    const std::size_t grown = used + size;
    std::memcpy(Bytes(stream) + kStreamSize, &grown, sizeof(grown));
    return true;
}

// Serialises one record with the game's writer into a stream of our own.
std::size_t Serialise(void* context, const void* record, std::int32_t armorLimit, std::uint8_t (&scratch)[kStreamObjectSize]) {
    std::memset(scratch, 0, sizeof(scratch));
    writeRecord(context, record, scratch, armorLimit);
    return StreamSize(scratch);
}

void FlushHandler(CpuContext* context) {
    void* stream = nullptr;
    std::memcpy(&stream, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(SiteRsp(context) + 0x60)), sizeof(stream));
    FlushRecords(stream);
}

// --- EOS ---
EosSendFn eosSend = nullptr;
EosReceiveFn eosReceive = nullptr;
constexpr std::int32_t kReliableOrdered = 2;  // EOS_PR_ReliableOrdered
// Every record of a sync can be a stub: in a room of 32 only about three of them fit the budget inline, and
// a stub past this count would be neither sent beside the sync nor waited for by the receiver.
constexpr std::size_t kMaxStubsPerPacket = kMaxPlayers;

SplitSyncReaders splitSyncReaders = nullptr;

unsigned long long SystemClock() { return GetTickCount64(); }
PacketFitClock packetClock = &SystemClock;

struct EosReceiveOptions {  // EOS_P2P_ReceivePacketOptions
    std::int32_t ApiVersion;
    const void* LocalUserId;
    std::uint32_t MaxDataSizeBytes;
    const std::uint8_t* RequestedChannel;  // ApiVersion 2 and later; null = any
};
static_assert(offsetof(EosReceiveOptions, MaxDataSizeBytes) == 16 && offsetof(EosReceiveOptions, RequestedChannel) == 24,
              "EOS_P2P_ReceivePacketOptions");
struct EosSocketId {  // EOS_P2P_SocketId
    std::int32_t ApiVersion;
    char SocketName[33];
};
static_assert(sizeof(EosSocketId) == 40, "EOS_P2P_SocketId");

// A received packet waiting for the records its stubs stand for.
struct HeldPacket {
    bool used = false;
    std::uint64_t order = 0;       // arrival, oldest first
    unsigned long long since = 0;  // packetClock
    void* peer = nullptr;          // EOS_ProductUserId: EOS keeps these for as long as it runs
    EosSocketId socket{};
    std::uint8_t channel = 0;
    std::uint32_t size = 0;
    std::uint8_t bytes[kEosMaxPacket];
};
HeldPacket heldPackets[kHeldPackets];
std::uint64_t heldOrder = 0;
std::atomic<std::size_t> heldCount{0};
SRWLOCK heldLock = SRWLOCK_INIT;  // taken before storeLock, never inside it

// How many stubs of `data` have no record here (yet); `missing` is the first of them.
std::size_t MissingRecords(const std::uint8_t* data, std::size_t size, StubInfo& missing) {
    StubInfo stubs[kMaxStubsPerPacket];
    const std::size_t found = std::min(FindStubs(data, size, stubs, kMaxStubsPerPacket), kMaxStubsPerPacket);
    std::size_t count = 0;
    for (std::size_t i = 0; i < found; ++i) {
        if (FindRecord(stubs[i], nullptr)) continue;
        if (!count++) missing = stubs[i];
    }
    return count;
}

bool Wanted(const HeldPacket& packet, const EosReceiveOptions* options) {
    if (!options) return true;
    if (options->ApiVersion >= 2 && options->RequestedChannel && *options->RequestedChannel != packet.channel) return false;
    return packet.size <= options->MaxDataSizeBytes;
}

// The oldest held packet that is complete now and fits what the caller asks for; packets held for kHeldPacketMs
// are given up on (counted in `expired`). Caller holds heldLock.
HeldPacket* NextCompleteLocked(const EosReceiveOptions* options, std::size_t& expired) {
    const unsigned long long now = packetClock();
    HeldPacket* next = nullptr;
    for (HeldPacket& packet : heldPackets) {
        if (!packet.used) continue;
        StubInfo missing;
        if (now - packet.since >= kHeldPacketMs) {
            packet.used = false;
            --heldCount;
            ++expired;
        } else if (Wanted(packet, options) && !MissingRecords(packet.bytes, packet.size, missing) &&
                   (!next || packet.order < next->order)) {
            next = &packet;
        }
    }
    return next;
}

// Hands a held packet that is complete now to the caller of EOS_P2P_ReceivePacket.
bool DeliverHeld(const void* options, void** peer, void* socket, std::uint8_t* channel, void* data, std::uint32_t* size) {
    if (!heldCount) return false;
    std::size_t expired = 0;
    AcquireSRWLockExclusive(&heldLock);
    HeldPacket* packet = NextCompleteLocked(static_cast<const EosReceiveOptions*>(options), expired);
    if (packet) {
        if (peer) *peer = packet->peer;
        if (socket) std::memcpy(socket, &packet->socket, sizeof(packet->socket));
        if (channel) *channel = packet->channel;
        std::memcpy(data, packet->bytes, packet->size);
        *size = packet->size;
        packet->used = false;
        --heldCount;
    }
    ReleaseSRWLockExclusive(&heldLock);
    if (expired)
        Log("MISSION sync: %zu held start message(s) dropped: their loadout records did not arrive within %llu s",
            expired, kHeldPacketMs / 1000);
    if (packet) Log("MISSION sync: held start message (%u bytes) handed to the game, its loadout records are here", *size);
    return packet != nullptr;
}

// A free slot, or else the oldest held packet's.
HeldPacket& SlotLocked() {
    HeldPacket* oldest = &heldPackets[0];
    for (HeldPacket& packet : heldPackets) {
        if (!packet.used) return packet;
        if (packet.order < oldest->order) oldest = &packet;
    }
    return *oldest;
}

void Hold(void* const* peer, const void* socket, const std::uint8_t* channel, const std::uint8_t* data,
          std::uint32_t size) {
    AcquireSRWLockExclusive(&heldLock);
    HeldPacket& slot = SlotLocked();
    const bool evicted = slot.used;
    if (!evicted) ++heldCount;
    slot.used = true;
    slot.order = ++heldOrder;
    slot.since = packetClock();
    slot.peer = peer ? *peer : nullptr;
    slot.socket = {};
    if (socket) std::memcpy(&slot.socket, socket, sizeof(slot.socket));
    slot.channel = channel ? *channel : 0;
    slot.size = size;
    std::memcpy(slot.bytes, data, size);
    ReleaseSRWLockExclusive(&heldLock);
    if (evicted) Log("MISSION sync: %zu start messages already wait for loadout records; the oldest is dropped", kHeldPackets);
}

// --- oversize diagnostic ---
std::uint64_t oversizeKinds[32]{};
std::size_t oversizeCount = 0;
SRWLOCK oversizeLock = SRWLOCK_INIT;

}  // namespace

std::vector<bool> PlanInline(std::size_t header, const std::vector<RecordRef>& records, std::size_t budget) {
    std::vector<bool> inlined(records.size(), true);
    std::size_t total = header;
    for (const auto& record : records) total += record.size;
    std::vector<std::size_t> order(records.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    // Players 5+ first (their records only exist in MultiSlot rooms), each group from the highest index down.
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const bool extraA = records[a].index >= 4, extraB = records[b].index >= 4;
        return extraA != extraB ? extraA : records[a].index > records[b].index;
    });
    for (const std::size_t i : order) {
        if (total <= budget) break;
        if (records[i].size <= kStubBytes || records[i].size > kMaxRecordBytes || records[i].index < 0 ||
            records[i].index > 0xFF)
            continue;
        total = total - records[i].size + kStubBytes;
        inlined[i] = false;
    }
    return inlined;
}

std::uint64_t RecordHash(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001B3ull;
    return hash;
}

void WriteStub(std::uint8_t* out, const StubInfo& stub) {
    out[0] = kByteArrayTag;
    out[1] = kStubLength;
    std::memcpy(out + 2, kStubMagic, sizeof(kStubMagic));
    PutInfo(out + 2 + sizeof(kStubMagic), stub);
}

bool ParseStub(const std::uint8_t* at, std::size_t available, StubInfo& stub) {
    if (!at || available < kStubBytes || at[0] != kByteArrayTag || at[1] != kStubLength ||
        std::memcmp(at + 2, kStubMagic, sizeof(kStubMagic)) != 0)
        return false;
    stub = GetInfo(at + 2 + sizeof(kStubMagic));
    return stub.size > 0 && stub.size <= kMaxRecordBytes;
}

std::size_t FindStubs(const std::uint8_t* data, std::size_t size, StubInfo* out, std::size_t max) {
    if (!data || size < kStubBytes) return 0;
    std::size_t found = 0;
    // Every packet the game sends or receives passes here: jump from one 0xA0 to the next.
    const std::uint8_t* const last = data + (size - kStubBytes);
    for (const std::uint8_t* at = data; at <= last;) {
        at = static_cast<const std::uint8_t*>(std::memchr(at, kByteArrayTag, static_cast<std::size_t>(last - at) + 1));
        if (!at) break;
        StubInfo stub;
        if (!ParseStub(at, static_cast<std::size_t>(data + size - at), stub)) {
            ++at;
            continue;
        }
        if (found < max) out[found] = stub;
        ++found;
        at += kStubBytes;
    }
    return found;
}

std::size_t BuildSidePacket(const StubInfo& stub, const std::uint8_t* record, std::uint8_t* out, std::size_t capacity) {
    if (!record || stub.size == 0 || stub.size > kMaxRecordBytes || capacity < kSideHeader + stub.size) return 0;
    std::memcpy(out, kSideMagic, sizeof(kSideMagic));
    PutInfo(out + sizeof(kSideMagic), stub);
    std::memcpy(out + kSideHeader, record, stub.size);
    return kSideHeader + stub.size;
}

bool IsSidePacket(const std::uint8_t* data, std::size_t size) {
    return data && size >= sizeof(kSideMagic) && std::memcmp(data, kSideMagic, sizeof(kSideMagic)) == 0;
}

bool ParseSidePacket(const std::uint8_t* data, std::size_t size, StubInfo& stub, const std::uint8_t*& record) {
    if (!IsSidePacket(data, size) || size < kSideHeader) return false;
    stub = GetInfo(data + sizeof(kSideMagic));
    record = data + kSideHeader;
    return stub.size > 0 && stub.size <= kMaxRecordBytes && size == kSideHeader + stub.size &&
           RecordHash(record, stub.size) == stub.hash;
}

bool StoreRecord(const StubInfo& stub, const std::uint8_t* record) {
    if (!record || stub.size == 0 || stub.size > kMaxRecordBytes) return false;
    AcquireSRWLockExclusive(&storeLock);
    const bool fresh = Lookup(stub) == nullptr;
    if (fresh) {
        Entry& entry = entries[nextEntry++ % kRecordStoreEntries];
        entry.used = true;
        entry.stub = stub;
        std::memcpy(entry.bytes, record, stub.size);
    }
    ReleaseSRWLockExclusive(&storeLock);
    return fresh;
}

bool FindRecord(const StubInfo& stub, std::uint8_t* out) {
    AcquireSRWLockShared(&storeLock);
    const Entry* entry = Lookup(stub);
    if (entry && out) std::memcpy(out, entry->bytes, stub.size);
    ReleaseSRWLockShared(&storeLock);
    return entry != nullptr;
}

void ClearRecords() {
    AcquireSRWLockExclusive(&heldLock);
    for (HeldPacket& packet : heldPackets) packet.used = false;
    heldCount = 0;
    ReleaseSRWLockExclusive(&heldLock);
    AcquireSRWLockExclusive(&storeLock);
    for (auto& entry : entries) entry.used = false;
    nextEntry = 0;
    ReleaseSRWLockExclusive(&storeLock);
}

void InitPacketFit(const unsigned char* gameBase) {
    SetRecordFunctions(reinterpret_cast<RecordWriteFn>(const_cast<unsigned char*>(gameBase) + 0x773840),
                       reinterpret_cast<RecordReadFn>(const_cast<unsigned char*>(gameBase) + 0x773740));
}

void SetRecordFunctions(RecordWriteFn write, RecordReadFn read) {
    writeRecord = write;
    readRecord = read;
    batch.stream = nullptr;
    batch.count = 0;
}

bool __fastcall RecordWriteHook(void* context, const void* record, void* stream, std::int32_t armorLimit) {
    if (batch.stream != stream) {
        if (batch.count) Log("MISSION sync: %zu records of an unfinished sync were dropped", batch.count);
        batch.stream = stream;
        batch.count = 0;
    }
    alignas(16) std::uint8_t scratch[kStreamObjectSize];
    const std::size_t size = Serialise(context, record, armorLimit, scratch);
    if (size > kMaxRecordBytes || batch.count == kBatchRecords) {
        // Cannot be held: what came before goes out first, then this one inline, in the game's order.
        FlushRecords(stream);
        Append(stream, scratch + kStreamData, size);
        return true;
    }
    Pending& pending = batch.records[batch.count++];
    std::memcpy(&pending.index, record, sizeof(pending.index));
    pending.size = size;
    std::memcpy(pending.bytes, scratch + kStreamData, size);
    return true;
}

void FlushRecords(void* stream) {
    if (!stream || batch.stream != stream || !batch.count) {
        if (batch.count) Log("MISSION sync: %zu records of another sync were dropped", batch.count);
        batch.stream = nullptr;
        batch.count = 0;
        return;
    }
    const std::size_t header = StreamSize(stream);
    std::vector<RecordRef> refs;
    std::size_t full = header;
    for (std::size_t i = 0; i < batch.count; ++i) {
        refs.push_back({batch.records[i].index, batch.records[i].size});
        full += batch.records[i].size;
    }
    const auto inlined = PlanInline(header, refs, kMissionSyncBudget);
    std::size_t moved = 0;
    for (std::size_t i = 0; i < batch.count; ++i) {
        const Pending& pending = batch.records[i];
        if (inlined[i]) {
            Append(stream, pending.bytes, pending.size);
            continue;
        }
        StubInfo stub;
        stub.index = pending.index;
        stub.size = pending.size;
        stub.hash = RecordHash(pending.bytes, pending.size);
        StoreRecord(stub, pending.bytes);
        std::uint8_t bytes[kStubBytes];
        WriteStub(bytes, stub);
        Append(stream, bytes, sizeof(bytes));
        ++moved;
    }
    if (moved)
        Log("MISSION sync: %zu loadout records would make the start message %zu bytes (EOS takes %zu per packet, so "
            "the message may hold %zu); %zu of them are sent beside it, the message is %zu bytes",
            batch.count, full, kEosMaxPacket, kMissionSyncBudget, moved, StreamSize(stream));
    batch.stream = nullptr;
    batch.count = 0;
}

bool __fastcall RecordReadHook(void* context, void* record, void* stream) {
    std::size_t position = 0;
    std::memcpy(&position, Bytes(stream) + kStreamPosition, sizeof(position));
    const std::size_t size = std::min(StreamSize(stream), kStreamCapacity);
    StubInfo stub;
    if (position >= size || !ParseStub(Bytes(stream) + kStreamData + position, size - position, stub))
        return readRecord(context, record, stream);
    position += kStubBytes;
    std::memcpy(Bytes(stream) + kStreamPosition, &position, sizeof(position));
    alignas(16) std::uint8_t scratch[kStreamObjectSize] = {};
    if (!FindRecord(stub, scratch + kStreamData)) {
        // The packet gate keeps this from happening; the player is left out (a negative index is skipped).
        std::size_t stored = 0;
        AcquireSRWLockShared(&storeLock);
        for (const auto& entry : entries) stored += entry.used ? 1 : 0;
        ReleaseSRWLockShared(&storeLock);
        Log("MISSION sync: the loadout record of player index %d (%zu bytes) never arrived; that player is left out "
            "(DIAG %zu record(s) here, %zu start message(s) held)", stub.index, stub.size, stored, heldCount.load());
        const std::int32_t none = -1;
        std::memcpy(record, &none, sizeof(none));
        return true;
    }
    std::memcpy(scratch + kStreamSize, &stub.size, sizeof(stub.size));
    return readRecord(context, record, scratch);
}

void* PacketFitCallHandler(std::uint32_t rva) {
    switch (rva) {
        case 0x78D6FA: return reinterpret_cast<void*>(&RecordWriteHook);
        case 0x790873: return reinterpret_cast<void*>(&RecordReadHook);
        default: return nullptr;
    }
}

MidHandler PacketFitHookHandler(std::uint32_t rva) { return rva == 0x78D756 ? &FlushHandler : nullptr; }

void SetEosFunctions(EosSendFn send, EosReceiveFn receive) {
    eosSend = send;
    eosReceive = receive;
}

void SetSplitSyncReaders(SplitSyncReaders readers) { splitSyncReaders = readers; }

void SetPacketFitClock(PacketFitClock clock) { packetClock = clock ? clock : &SystemClock; }

std::size_t HeldPacketCount() { return heldCount; }

EosResult PacketFitSend(void* handle, const EosSendOptions* options) {
    if (options && options->Data && options->DataLengthBytes >= kStubBytes) {
        StubInfo stubs[kMaxStubsPerPacket];
        const std::size_t found = std::min(
            FindStubs(static_cast<const std::uint8_t*>(options->Data), options->DataLengthBytes, stubs, kMaxStubsPerPacket),
            kMaxStubsPerPacket);
        if (found && !(splitSyncReaders && splitSyncReaders(options->RemoteUserId))) {
            static std::atomic<bool> logged{false};
            if (!splitSyncReaders && !logged.exchange(true))
                Log("MISSION sync: nobody is known to read a split start message (the lobby marker is unavailable); "
                    "a start message too large for one packet is not sent");
            return kEosLimitExceeded;
        }
        for (std::size_t i = 0; i < found; ++i) {
            std::uint8_t record[kMaxRecordBytes];
            std::uint8_t packet[kSideHeader + kMaxRecordBytes];
            if (!FindRecord(stubs[i], record)) continue;  // not ours to send (a member relaying cannot happen)
            EosSendOptions side = *options;
            side.Channel = kSideChannel;
            side.Data = packet;
            side.DataLengthBytes = static_cast<std::uint32_t>(BuildSidePacket(stubs[i], record, packet, sizeof(packet)));
            side.Reliability = kReliableOrdered;
            side.AllowDelayedDelivery = 1;
            const EosResult sent = eosSend(handle, &side);
            Log("MISSION sync: DIAG side packet of player index %d (%u bytes, channel %u) sent: result %d", stubs[i].index,
                side.DataLengthBytes, static_cast<unsigned>(side.Channel), sent);
        }
    }
    const EosResult result = eosSend(handle, options);
    if (options && options->Data && options->DataLengthBytes >= kStubBytes) {
        StubInfo stubs[kMaxStubsPerPacket];
        if (const std::size_t found = FindStubs(static_cast<const std::uint8_t*>(options->Data), options->DataLengthBytes,
                                                stubs, kMaxStubsPerPacket))
            Log("MISSION sync: DIAG start message (%u bytes, channel %u, %zu stub(s)) sent: result %d",
                options->DataLengthBytes, static_cast<unsigned>(options->Channel), found, result);
    }
    return result;
}

EosResult PacketFitReceive(void* handle, const void* options, void** peer, void* socket, std::uint8_t* channel, void* data,
                           std::uint32_t* size) {
    static std::atomic<bool> calledOnce{false};
    if (!calledOnce.exchange(true)) {
        const auto* ask = static_cast<const EosReceiveOptions*>(options);
        Log("MISSION sync: DIAG the game reads packets through the split sync gate (options v%d, channel %s)",
            ask ? ask->ApiVersion : -1, ask && ask->ApiVersion >= 2 && ask->RequestedChannel ? "asked" : "any");
    }
    for (;;) {
        if (data && size && DeliverHeld(options, peer, socket, channel, data, size)) return 0;
        const EosResult result = eosReceive(handle, options, peer, socket, channel, data, size);
        if (result != 0 || !data || !size) return result;
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        if (IsSidePacket(bytes, *size)) {
            StubInfo stub;
            const std::uint8_t* record = nullptr;
            if (!ParseSidePacket(bytes, *size, stub, record))
                Log("MISSION sync: a malformed side packet (%u bytes) was dropped", *size);
            else if (StoreRecord(stub, record))
                Log("MISSION sync: loadout record of player index %d arrived beside the start message", stub.index);
            else
                Log("MISSION sync: DIAG side packet of player index %d again (already here)", stub.index);
            continue;  // never the game's; a held packet it completes goes out next
        }
        StubInfo missing;
        const std::size_t count = MissingRecords(bytes, *size, missing);
        if (StubInfo stubs[kMaxStubsPerPacket]; FindStubs(bytes, *size, stubs, kMaxStubsPerPacket))
            Log("MISSION sync: DIAG start message (%u bytes, channel %u) received, %zu record(s) missing", *size,
                channel ? static_cast<unsigned>(*channel) : 255u, count);
        if (!count) return result;
        if (*size > kEosMaxPacket) {  // cannot happen (EOS sends nothing larger); never hand the game half a sync
            Log("MISSION sync: a %u-byte start message without its loadout records was dropped", *size);
            continue;
        }
        Hold(peer, socket, channel, bytes, *size);
        Log("MISSION sync: start message (%u bytes) held until %zu loadout record(s) arrive, the first for player "
            "index %d", *size, count, missing.index);
    }
}

int InstallPacketFit(HMODULE game, ImportRedirect redirect) {
    constexpr const char* sdk = "EOSSDK-Win64-Shipping.dll";
    // The redirect stores the previous target before it changes the slot, so a packet sent or received in
    // between already finds the EOS function here.
    int redirected = 0;
    if (redirect(game, sdk, "EOS_P2P_ReceivePacket", reinterpret_cast<void*>(&PacketFitReceive), reinterpret_cast<void**>(&eosReceive)))
        ++redirected;
    if (redirect(game, sdk, "EOS_P2P_SendPacket", reinterpret_cast<void*>(&PacketFitSend), reinterpret_cast<void**>(&eosSend)))
        ++redirected;
    return redirected;
}

bool FirstOversize(std::uintptr_t caller, std::uint8_t channel, const std::uint8_t* data, std::size_t size) {
    // The packet type is the low 20 bits of the controller header's third word (12D0BAB), after the 8-byte
    // datagram header; other packets are told apart by caller and channel only.
    std::uint32_t type = 0xFFFFFFFF;
    const std::uint8_t controllerMagic[4] = {0x00, 0x12, 0x40, 0x00};  // 0x401200
    if (data && size >= kSessionHeader + kControllerHeader && std::memcmp(data + kSessionHeader, controllerMagic, 4) == 0) {
        std::memcpy(&type, data + kSessionHeader + 8, sizeof(type));
        type &= 0xFFFFF;
    }
    std::uint64_t kind = 0xCBF29CE484222325ull;
    const std::uint64_t parts[] = {static_cast<std::uint64_t>(caller), channel, type};
    for (const std::uint64_t part : parts) kind = (kind ^ part) * 0x100000001B3ull;
    AcquireSRWLockExclusive(&oversizeLock);
    const std::size_t known = std::min(oversizeCount, std::size(oversizeKinds));
    bool first = std::find(oversizeKinds, oversizeKinds + known, kind) == oversizeKinds + known;
    if (first) oversizeKinds[oversizeCount++ % std::size(oversizeKinds)] = kind;
    ReleaseSRWLockExclusive(&oversizeLock);
    return first;
}

void DescribeBytes(const std::uint8_t* data, std::size_t size, char* out, std::size_t capacity) {
    if (!out || !capacity) return;
    out[0] = 0;
    std::size_t used = 0;
    for (std::size_t i = 0; data && i < size && used + 3 < capacity; ++i)
        used += static_cast<std::size_t>(std::snprintf(out + used, capacity - used, i ? " %02X" : "%02X", data[i]));
}

void LogOversizePacket(std::uintptr_t caller, std::uint8_t channel, std::int32_t reliability, const void* data,
                       std::uint32_t size, EosResult result) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    if (size <= kEosMaxPacket || !FirstOversize(caller, channel, bytes, size)) return;
    char head[32 * 3 + 1];
    DescribeBytes(bytes, std::min<std::size_t>(size, 32), head, sizeof(head));
    Log("NETLOG OVERSIZE game packet of %u bytes (EOS takes %zu): channel=%u reliability=%d caller=EDF+%llX result=%d "
        "first bytes %s (logged once per kind)",
        size, kEosMaxPacket, channel, reliability, static_cast<unsigned long long>(caller), result, head);
}

}  // namespace multislot
