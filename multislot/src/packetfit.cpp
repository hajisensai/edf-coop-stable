#include "packetfit.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <numeric>

#include "log.h"

namespace multislot {
namespace {

constexpr std::uint8_t kStubMagic[8] = {'M', 'S', 'l', 'o', 't', 'R', 'e', 'c'};
constexpr std::uint8_t kSideMagic[8] = {'M', 'S', 'l', 'o', 't', 'S', 'i', 'd'};
constexpr std::uint8_t kBulkMagic[8] = {'M', 'S', 'l', 'o', 't', 'B', 'l', 'k'};
constexpr std::uint8_t kBulkPayloadMagic[8] = {'M', 'S', 'l', 'o', 't', 'R', 'B', 'k'};
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
std::size_t syncBudget = kMissionSyncBudget;  // [Test] SplitSyncBudget lowers it
unsigned long long recordWaitMs = kRecordWaitMs;

// The records the last sync this machine wrote moved out, and the members that got them.
struct Outgoing {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<std::pair<StubInfo, std::vector<std::uint8_t>>> records;
    std::vector<const void*> sentTo;
    unsigned long long since = 0;
    std::vector<std::uint8_t> bulk;  // every record in one bulk message (empty: none)
    std::vector<const void*> bulkSentTo;
} outgoing;

BulkReady bulkReady = nullptr;
BulkSend bulkSend = nullptr;
BulkIncoming bulkIncoming = nullptr;

// Bulks received (or written), by id: a few syncs' worth, oldest replaced first.
constexpr std::size_t kBulksKept = 4;
struct Bulks {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<std::pair<std::uint64_t, std::vector<BulkRecord>>> held;  // newest last
    std::vector<std::uint64_t> abandoned;  // bulks waited for once in vain: their records are not waited for again
} bulks;

bool BulkAbandoned(std::uint64_t id) {
    AcquireSRWLockShared(&bulks.lock);
    const bool gone = std::find(bulks.abandoned.begin(), bulks.abandoned.end(), id) != bulks.abandoned.end();
    ReleaseSRWLockShared(&bulks.lock);
    return gone;
}

void AbandonBulk(std::uint64_t id) {
    AcquireSRWLockExclusive(&bulks.lock);
    if (bulks.abandoned.size() >= kBulksKept) bulks.abandoned.erase(bulks.abandoned.begin());
    bulks.abandoned.push_back(id);
    ReleaseSRWLockExclusive(&bulks.lock);
}

void KeepBulk(std::uint64_t id, std::vector<BulkRecord> records) {
    AcquireSRWLockExclusive(&bulks.lock);
    for (auto& [have, list] : bulks.held)
        if (have == id) {
            ReleaseSRWLockExclusive(&bulks.lock);
            return;
        }
    if (bulks.held.size() >= kBulksKept) bulks.held.erase(bulks.held.begin());
    bulks.held.emplace_back(id, std::move(records));
    ReleaseSRWLockExclusive(&bulks.lock);
}

// Record `k` of bulk `id` into `out` (and its size); false when that bulk is not here (yet).
bool BulkRecordAt(std::uint64_t id, std::size_t k, std::uint8_t* out, std::size_t& size) {
    AcquireSRWLockShared(&bulks.lock);
    bool found = false;
    for (const auto& [have, list] : bulks.held)
        if (have == id && k < list.size()) {
            size = list[k].bytes.size();
            if (out) std::memcpy(out, list[k].bytes.data(), size);
            found = true;
            break;
        }
    ReleaseSRWLockShared(&bulks.lock);
    return found;
}

// Which record of a bulk marker the game's reader is at (it stays on the marker until the last).
struct BulkReader {
    const void* stream = nullptr;
    std::uint64_t id = 0;
    std::size_t next = 0;
} bulkReader;

// What the game's last receive asked EOS: what RecordReadHook receives with while it waits for a record.
struct LastReceive {
    bool known = false;
    void* handle = nullptr;
    const void* localUser = nullptr;
    std::int32_t apiVersion = 2;
} lastReceive;

unsigned long long SystemClock() { return GetTickCount64(); }
PacketFitClock packetClock = &SystemClock;

constexpr std::size_t kGameReceiveBuffer = 0x1000;  // what the game receives into (12C8D1D)
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

// A received game packet the game has not read yet. EOS acknowledged it to its sender long ago and never sends it
// again, and the game's own resends stop after ~26 s: a held packet that is thrown away is gone for good, so none
// ever is. A packet leaves only by reaching the game (or with the room, ClearRecords).
struct HeldPacket {
    std::uint64_t order = 0;       // arrival, oldest first
    unsigned long long since = 0;  // packetClock
    void* peer = nullptr;          // EOS_ProductUserId: EOS keeps these for as long as it runs
    std::uint64_t bulk = 0;        // the records bulk (its fragment id) it waits behind; 0: it waits for nothing
    std::size_t bulkTotal = 0;     // that bulk's size
    EosSocketId socket{};
    std::uint8_t channel = 0;
    std::vector<std::uint8_t> bytes;
};
std::deque<HeldPacket> heldStore;  // arrival order
std::uint64_t heldOrder = 0;
std::atomic<std::size_t> heldCount{0};
std::uint64_t heldEver = 0, deliveredEver = 0;  // since start: every held packet is delivered, and the log says so
// Bulks whose packets were let go (it arrived, it was given up, or it outgrew what holding may cost): a resend that
// starts one over at the receiver does not hold that member's packets a second time.
constexpr std::size_t kReleasedKept = 64;
std::deque<std::pair<void*, std::uint64_t>> released;
SRWLOCK heldLock = SRWLOCK_INIT;  // taken before storeLock (and before the bulk query's own lock), never inside it

bool Wanted(const HeldPacket& packet, const EosReceiveOptions* options) {
    if (!options) return true;
    if (options->ApiVersion >= 2 && options->RequestedChannel && *options->RequestedChannel != packet.channel) return false;
    return packet.bytes.size() <= options->MaxDataSizeBytes;
}

bool ReleasedLocked(void* peer, std::uint64_t bulk) {
    return std::find(released.begin(), released.end(), std::make_pair(peer, bulk)) != released.end();
}

// Every packet held behind `bulk` of `peer` may go to the game now (in order); `why` goes to the log.
void ReleaseLocked(void* peer, std::uint64_t bulk, const char* why) {
    if (ReleasedLocked(peer, bulk)) return;
    if (released.size() >= kReleasedKept) released.pop_front();
    released.emplace_back(peer, bulk);
    std::size_t count = 0;
    for (HeldPacket& packet : heldStore)
        if (packet.peer == peer && packet.bulk == bulk) {
            packet.bulk = 0;
            ++count;
        }
    Log("MISSION sync: %zu game packet(s) held behind a member's loadout records bulk go to the game in order: %s",
        count, why);
}

// The records bulk of `peer` that its packets wait behind now (0: none), its size in `total`.
std::uint64_t HoldingForLocked(void* peer, std::size_t& total) {
    total = 0;
    if (!bulkIncoming || !peer) return 0;
    const std::uint64_t bulk = bulkIncoming(peer, &total);
    return bulk && !ReleasedLocked(peer, bulk) ? bulk : 0;
}

// Lets go every bulk whose packets need not wait any more: it arrived or its receiver gave it up (no longer on its
// way), or the packets waited as long as the bulk takes at the slowest rate it is planned for (BulkHoldMs): past
// that the game is better off reading on and waiting for the records once, inside the start message (ReadBulkRecord),
// than having its sender resend into a silence that ends the room.
void RefreshHeldLocked() {
    const unsigned long long now = packetClock();
    std::vector<std::pair<void*, std::uint64_t>> checked;
    for (const HeldPacket& packet : heldStore) {
        if (!packet.bulk) continue;
        const auto key = std::make_pair(packet.peer, packet.bulk);
        if (std::find(checked.begin(), checked.end(), key) != checked.end()) continue;
        checked.push_back(key);  // the oldest packet of each bulk comes first: it has waited longest
        std::size_t total = 0;
        if (!bulkIncoming || bulkIncoming(packet.peer, &total) != packet.bulk)
            ReleaseLocked(packet.peer, packet.bulk, "the bulk arrived, or its sender gave it up");
        else if (now - packet.since >= BulkHoldMs(packet.bulkTotal))
            ReleaseLocked(packet.peer, packet.bulk, "they waited as long as the bulk takes at 32 KiB/s; it is not waited for here");
    }
}

// The oldest held packet the game may have now: it waits for no bulk, nothing older from its sender still waits (a
// member's packets reach the game in the order they came), and it fits what the caller asks for.
std::deque<HeldPacket>::iterator NextDeliverableLocked(const EosReceiveOptions* options) {
    RefreshHeldLocked();
    std::vector<void*> waiting;
    for (auto it = heldStore.begin(); it != heldStore.end(); ++it) {
        const bool behind = std::find(waiting.begin(), waiting.end(), it->peer) != waiting.end();
        if (it->bulk || behind) {
            if (!behind) waiting.push_back(it->peer);
            continue;
        }
        if (Wanted(*it, options)) return it;
    }
    return heldStore.end();
}

bool PeerHeldLocked(void* peer) {
    return std::any_of(heldStore.begin(), heldStore.end(), [peer](const HeldPacket& packet) { return packet.peer == peer; });
}

// Hands the oldest deliverable held packet to the caller of EOS_P2P_ReceivePacket.
bool DeliverHeld(const void* options, void** peer, void* socket, std::uint8_t* channel, void* data, std::uint32_t* size) {
    if (!heldCount) return false;
    AcquireSRWLockExclusive(&heldLock);
    auto it = NextDeliverableLocked(static_cast<const EosReceiveOptions*>(options));
    const bool found = it != heldStore.end();
    bool drained = false;
    std::uint64_t ever = 0, delivered = 0;
    if (found) {
        if (peer) *peer = it->peer;
        if (socket) std::memcpy(socket, &it->socket, sizeof(it->socket));
        if (channel) *channel = it->channel;
        std::memcpy(data, it->bytes.data(), it->bytes.size());
        *size = static_cast<std::uint32_t>(it->bytes.size());
        heldStore.erase(it);
        heldCount = heldStore.size();
        ++deliveredEver;
        drained = heldStore.empty();
        ever = heldEver;
        delivered = deliveredEver;
    }
    ReleaseSRWLockExclusive(&heldLock);
    if (drained)
        Log("MISSION sync: every held game packet reached the game (%llu held, %llu delivered since start)",
            static_cast<unsigned long long>(ever), static_cast<unsigned long long>(delivered));
    return found;
}

// Holds a received game packet (caller holds heldLock): behind its sender's records bulk if one is on its way. When
// that member's packets outgrow what the bulk may cost (BulkHoldCapacity), the bulk is let go and they all go to the
// game in order - never dropped.
void HoldLocked(void* peer, const void* socket, std::uint8_t channel, const std::uint8_t* data, std::uint32_t size,
                std::uint64_t bulk, std::size_t bulkTotal) {
    HeldPacket packet;
    packet.order = ++heldOrder;
    packet.since = packetClock();
    packet.peer = peer;
    packet.bulk = bulk;
    packet.bulkTotal = bulkTotal;
    if (socket) std::memcpy(&packet.socket, socket, sizeof(packet.socket));
    packet.channel = channel;
    packet.bytes.assign(data, data + size);
    heldStore.push_back(std::move(packet));
    heldCount = heldStore.size();
    ++heldEver;
    if (!bulk) return;
    const std::size_t behind = static_cast<std::size_t>(std::count_if(
        heldStore.begin(), heldStore.end(), [&](const HeldPacket& p) { return p.peer == peer && p.bulk == bulk; }));
    if (behind == 1)
        Log("MISSION sync: a member's loadout records bulk (%zu bytes) is on its way: its game packets wait behind it "
            "(for up to %llu ms or %zu packets)", bulkTotal, BulkHoldMs(bulkTotal), BulkHoldCapacity(bulkTotal));
    if (behind > BulkHoldCapacity(bulkTotal))
        ReleaseLocked(peer, bulk, "more of them came than the bulk may hold back; it is not waited for here");
}

// A packet the game's receive got from EOS: held (true) when its sender's records bulk is on its way, or when older
// packets of that sender are held still (they go first). `always`: held either way (the game's frame waits inside
// RecordReadHook, and what arrives meanwhile is the game's to read afterwards).
bool HoldReceived(void* peer, const void* socket, std::uint8_t channel, const std::uint8_t* data, std::uint32_t size,
                  bool always) {
    AcquireSRWLockExclusive(&heldLock);
    std::size_t total = 0;
    const std::uint64_t bulk = HoldingForLocked(peer, total);
    const bool hold = always || bulk || PeerHeldLocked(peer);
    if (hold) HoldLocked(peer, socket, channel, data, size, bulk, total);
    ReleaseSRWLockExclusive(&heldLock);
    return hold;
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
    heldStore.clear();  // the room is gone: its packets with it
    released.clear();
    heldCount = 0;
    ReleaseSRWLockExclusive(&heldLock);
    AcquireSRWLockExclusive(&storeLock);
    for (auto& entry : entries) entry.used = false;
    nextEntry = 0;
    ReleaseSRWLockExclusive(&storeLock);
    AcquireSRWLockExclusive(&outgoing.lock);
    outgoing.records.clear();
    outgoing.sentTo.clear();
    outgoing.bulk.clear();
    outgoing.bulkSentTo.clear();
    ReleaseSRWLockExclusive(&outgoing.lock);
    AcquireSRWLockExclusive(&bulks.lock);
    bulks.held.clear();
    bulks.abandoned.clear();
    ReleaseSRWLockExclusive(&bulks.lock);
    bulkReader = {};
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
    // Too large even with stubs to spare, and the room reads fragments: every record goes in bulk (see packetfit.h).
    if (full > syncBudget && bulkReady && bulkSend && bulkReady()) {
        std::vector<BulkRecord> all;
        for (std::size_t i = 0; i < batch.count; ++i)
            all.push_back({batch.records[i].index,
                           std::vector<std::uint8_t>(batch.records[i].bytes, batch.records[i].bytes + batch.records[i].size)});
        std::uint64_t id = RecordHash(reinterpret_cast<const std::uint8_t*>(&full), sizeof(full));
        for (const auto& r : all) id = RecordHash(r.bytes.data(), r.bytes.size()) ^ (id * 0x100000001B3ull);
        std::vector<std::uint8_t> payload = BuildRecordsBulk(id, all);
        std::uint8_t marker[kStubBytes];
        WriteBulkMarker(marker, batch.count, id);
        Append(stream, marker, sizeof(marker));
        Log("MISSION sync: %zu loadout records would make the start message %zu bytes (it may hold %zu); all of them go "
            "in one bulk message of %zu bytes, the message is %zu bytes",
            batch.count, full, syncBudget, payload.size(), StreamSize(stream));
        KeepBulk(id, std::move(all));
        AcquireSRWLockExclusive(&outgoing.lock);
        outgoing.records.clear();
        outgoing.sentTo.clear();
        outgoing.bulk = std::move(payload);
        outgoing.bulkSentTo.clear();
        outgoing.since = packetClock();
        ReleaseSRWLockExclusive(&outgoing.lock);
        batch.stream = nullptr;
        batch.count = 0;
        return;
    }
    const auto inlined = PlanInline(header, refs, syncBudget);
    std::size_t moved = 0;
    std::vector<std::pair<StubInfo, std::vector<std::uint8_t>>> moving;
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
        moving.push_back({stub, std::vector<std::uint8_t>(pending.bytes, pending.bytes + pending.size)});
        std::uint8_t bytes[kStubBytes];
        WriteStub(bytes, stub);
        Append(stream, bytes, sizeof(bytes));
        ++moved;
    }
    if (moved)
        Log("MISSION sync: %zu loadout records would make the start message %zu bytes (EOS takes %zu per packet, so "
            "the message may hold %zu); %zu of them are sent beside it, the message is %zu bytes",
            batch.count, full, kEosMaxPacket, syncBudget, moved, StreamSize(stream));
    if (moved) {
        AcquireSRWLockExclusive(&outgoing.lock);
        outgoing.records = std::move(moving);
        outgoing.sentTo.clear();
        outgoing.since = packetClock();
        ReleaseSRWLockExclusive(&outgoing.lock);
    }
    batch.stream = nullptr;
    batch.count = 0;
}

// Side packets: what a received one holds goes to the store. True when it was one.
bool TakeSidePacket(const std::uint8_t* bytes, std::uint32_t size) {
    if (!IsSidePacket(bytes, size)) return false;
    StubInfo stub;
    const std::uint8_t* record = nullptr;
    if (!ParseSidePacket(bytes, size, stub, record))
        Log("MISSION sync: a malformed side packet (%u bytes) was dropped", size);
    else if (StoreRecord(stub, record))
        Log("MISSION sync: loadout record of player index %d arrived beside the start message", stub.index);
    return true;
}

// Waits for the record of `stub` (RecordReadHook, on the game thread): receives from EOS itself, keeps side
// packets and holds the rest for the game, until the record is here or kRecordWaitMs passed.
bool WaitForRecord(const StubInfo& stub) {
    if (!lastReceive.known || !eosReceive) return false;
    const EosReceiveOptions ask{lastReceive.apiVersion, lastReceive.localUser, static_cast<std::uint32_t>(kGameReceiveBuffer),
                                nullptr};
    std::vector<std::uint8_t> buffer(kGameReceiveBuffer);
    const unsigned long long start = packetClock();
    std::size_t held = 0;
    for (;;) {
        for (;;) {
            void* peer = nullptr;
            EosSocketId socket{};
            std::uint8_t channel = 0;
            std::uint32_t size = 0;
            if (eosReceive(lastReceive.handle, &ask, &peer, &socket, &channel, buffer.data(), &size) != 0) break;
            if (TakeSidePacket(buffer.data(), size)) continue;
            HoldReceived(peer, &socket, channel, buffer.data(), size, true);
            ++held;
        }
        if (FindRecord(stub, nullptr) || packetClock() - start >= recordWaitMs) break;
        Sleep(1);
    }
    const bool here = FindRecord(stub, nullptr);
    Log("MISSION sync: waited %llu ms for the loadout record of player index %d: %s (%zu game packet(s) held for the "
        "game meanwhile)", packetClock() - start, stub.index, here ? "here" : "still missing", held);
    return here;
}

// Waits for bulk `id` as WaitForRecord waits for a record (receiving from EOS itself, holding the game's packets).
bool WaitForBulk(std::uint64_t id, std::size_t count) {
    std::size_t size = 0;
    if (BulkRecordAt(id, 0, nullptr, size)) return true;
    if (!lastReceive.known || !eosReceive) return false;
    const unsigned long long wait = recordWaitMs + kBulkWaitMsPerKiB * (count * kMaxRecordBytes / 1024);
    const EosReceiveOptions ask{lastReceive.apiVersion, lastReceive.localUser, static_cast<std::uint32_t>(kGameReceiveBuffer),
                                nullptr};
    std::vector<std::uint8_t> buffer(kGameReceiveBuffer);
    const unsigned long long start = packetClock();
    std::size_t held = 0;
    bool here = false;
    for (;;) {
        for (;;) {
            void* peer = nullptr;
            EosSocketId socket{};
            std::uint8_t channel = 0;
            std::uint32_t got = 0;
            if (eosReceive(lastReceive.handle, &ask, &peer, &socket, &channel, buffer.data(), &got) != 0) break;
            if (TakeSidePacket(buffer.data(), got)) continue;
            HoldReceived(peer, &socket, channel, buffer.data(), got, true);
            ++held;
        }
        here = BulkRecordAt(id, 0, nullptr, size);
        if (here || packetClock() - start >= wait) break;
        Sleep(1);
    }
    Log("MISSION sync: waited %llu ms (of %llu) for the %zu loadout records in bulk: %s (%zu game packet(s) held for the "
        "game meanwhile)", packetClock() - start, wait, count, here ? "here" : "still missing", held);
    return here;
}

// The start message's bulk marker read as record `bulkReader.next` of the bulk; the stream stays on the marker until
// the last record was read.
bool ReadBulkRecord(void* context, void* record, void* stream, std::size_t position, std::size_t count, std::uint64_t id) {
    if (bulkReader.stream != stream || bulkReader.id != id || bulkReader.next >= count) bulkReader = {stream, id, 0};
    const std::size_t k = bulkReader.next++;
    if (bulkReader.next >= count) {
        position += kStubBytes;  // the last one: the game reads on after the marker
        std::memcpy(Bytes(stream) + kStreamPosition, &position, sizeof(position));
        bulkReader = {};
    }
    alignas(16) std::uint8_t scratch[kStreamObjectSize] = {};
    std::size_t size = 0;
    // Waited for once already in vain: every later record of it is left out at once (a frame of the game must not
    // wait count times over).
    const bool here = BulkRecordAt(id, k, scratch + kStreamData, size) ||
                      (!BulkAbandoned(id) && WaitForBulk(id, count) && BulkRecordAt(id, k, scratch + kStreamData, size));
    if (!here) AbandonBulk(id);
    if (!here) {
        Log("MISSION sync: the loadout records in bulk never arrived; record %zu of %zu is left out and that player will "
            "look wrong on this machine", k, count);
        const std::int32_t none = -1;
        std::memcpy(record, &none, sizeof(none));
        return true;
    }
    std::memcpy(scratch + kStreamSize, &size, sizeof(size));
    return readRecord(context, record, scratch);
}

bool __fastcall RecordReadHook(void* context, void* record, void* stream) {
    std::size_t position = 0;
    std::memcpy(&position, Bytes(stream) + kStreamPosition, sizeof(position));
    const std::size_t size = std::min(StreamSize(stream), kStreamCapacity);
    std::size_t bulkCount = 0;
    std::uint64_t bulkId = 0;
    if (position < size && ParseBulkMarker(Bytes(stream) + kStreamData + position, size - position, bulkCount, bulkId))
        return ReadBulkRecord(context, record, stream, position, bulkCount, bulkId);
    StubInfo stub;
    if (position >= size || !ParseStub(Bytes(stream) + kStreamData + position, size - position, stub))
        return readRecord(context, record, stream);
    position += kStubBytes;
    std::memcpy(Bytes(stream) + kStreamPosition, &position, sizeof(position));
    alignas(16) std::uint8_t scratch[kStreamObjectSize] = {};
    if (!FindRecord(stub, scratch + kStreamData) && !(WaitForRecord(stub) && FindRecord(stub, scratch + kStreamData))) {
        // The player is left out (a negative index skips the copy; the game still builds that player, from whatever
        // its record slot held - so this must not happen, and the log says so loudly).
        Log("MISSION sync: the loadout record of player index %d (%zu bytes) never arrived; that player is left out "
            "and will look wrong on this machine", stub.index, stub.size);
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

void SetRecordWait(unsigned long long ms) { recordWaitMs = ms; }

void SetSyncBudget(std::size_t budget) { syncBudget = budget && budget < kMissionSyncBudget ? budget : kMissionSyncBudget; }

void SetPacketFitClock(PacketFitClock clock) { packetClock = clock ? clock : &SystemClock; }

std::size_t HeldPacketCount() { return heldCount; }

unsigned long long BulkHoldMs(std::size_t total) {
    return kRecordWaitMs + kBulkWaitMsPerKiB * ((total + 1023) / 1024);
}

std::size_t BulkHoldCapacity(std::size_t total) {
    return kBulkHoldMinPackets + static_cast<std::size_t>(kBulkHoldPacketsPerSecond * BulkHoldMs(total) / 1000);
}

// The records of the last sync go to `options`' member ahead of the first packet to it since: each once, whoever
// it is. The sync itself reaches every member whatever this plugin does (it is encrypted), so holding the records
// back from a member whose marker has not shown up yet (Epic relays lobby attributes in their own time) only made
// it wait for them, and past kRecordWaitMs it built that player from an empty record (GameNet_mission8rushed). A
// member without EDF6Coop cannot read a split sync either way; its game drops the side packets as datagrams that
// do not decrypt (GameNet_mission2plain).
void SendRecordsAhead(void* handle, const EosSendOptions& options) {
    std::vector<std::pair<StubInfo, std::vector<std::uint8_t>>> records;
    std::vector<std::uint8_t> bulk;
    AcquireSRWLockExclusive(&outgoing.lock);
    if (!outgoing.bulk.empty() && packetClock() - outgoing.since < kRecordsAheadMs &&
        std::find(outgoing.bulkSentTo.begin(), outgoing.bulkSentTo.end(), options.RemoteUserId) == outgoing.bulkSentTo.end())
        bulk = outgoing.bulk;  // marked sent once it went (a send that fails now goes again with the next packet)
    const bool due = !outgoing.records.empty() && packetClock() - outgoing.since < kRecordsAheadMs &&
                     std::find(outgoing.sentTo.begin(), outgoing.sentTo.end(), options.RemoteUserId) == outgoing.sentTo.end();
    if (due) {
        outgoing.sentTo.push_back(options.RemoteUserId);
        records = outgoing.records;
    }
    ReleaseSRWLockExclusive(&outgoing.lock);
    if (!bulk.empty()) {
        const bool sent = bulkSend && bulkSend(options.RemoteUserId, kRecordsBulkTag, bulk.data(), bulk.size());
        if (sent) {
            AcquireSRWLockExclusive(&outgoing.lock);
            outgoing.bulkSentTo.push_back(options.RemoteUserId);
            ReleaseSRWLockExclusive(&outgoing.lock);
        }
        Log("MISSION sync: every loadout record (%zu bytes) sent in bulk ahead of the start message: %s", bulk.size(),
            sent ? "sent" : "NOT sent");
    }
    if (due && splitSyncReaders) splitSyncReaders(options.RemoteUserId);  // says so if it shows no marker
    for (const auto& [stub, bytes] : records) {
        std::uint8_t packet[kSideHeader + kMaxRecordBytes];
        EosSendOptions side = options;
        side.Channel = kSideChannel;
        side.Data = packet;
        side.DataLengthBytes = static_cast<std::uint32_t>(BuildSidePacket(stub, bytes.data(), packet, sizeof(packet)));
        side.Reliability = kReliableOrdered;
        side.AllowDelayedDelivery = 1;
        const EosResult sent = eosSend(handle, &side);
        Log("MISSION sync: loadout record of player index %d sent beside the start message: result %d", stub.index, sent);
    }
}

EosResult PacketFitSend(void* handle, const EosSendOptions* options) {
    if (options && options->RemoteUserId) SendRecordsAhead(handle, *options);
    return eosSend(handle, options);
}

EosResult PacketFitReceive(void* handle, const void* options, void** peer, void* socket, std::uint8_t* channel, void* data,
                           std::uint32_t* size) {
    if (const auto* ask = static_cast<const EosReceiveOptions*>(options)) {
        lastReceive.handle = handle;
        lastReceive.localUser = ask->LocalUserId;
        lastReceive.apiVersion = ask->ApiVersion;
        lastReceive.known = true;
    }
    for (;;) {
        if (data && size && DeliverHeld(options, peer, socket, channel, data, size)) return 0;
        const EosResult result = eosReceive(handle, options, peer, socket, channel, data, size);
        if (result != 0 || !data || !size) return result;
        if (TakeSidePacket(static_cast<const std::uint8_t*>(data), *size)) continue;  // never the game's
        // Behind its sender's records bulk (or behind older packets of that sender still held): the game reads it
        // once the bulk is here, or let go (HoldReceived).
        if (peer && HoldReceived(*peer, socket, channel ? *channel : 0, static_cast<const std::uint8_t*>(data), *size,
                                 false))
            continue;
        return result;
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


void SetBulkRecords(BulkReady ready, BulkSend send, BulkIncoming incoming) {
    bulkReady = ready;
    bulkSend = send;
    bulkIncoming = incoming;
}

std::vector<std::uint8_t> BuildRecordsBulk(std::uint64_t id, const std::vector<BulkRecord>& records) {
    std::vector<std::uint8_t> out(sizeof(kBulkPayloadMagic) + 8 + 2);
    std::memcpy(out.data(), kBulkPayloadMagic, sizeof(kBulkPayloadMagic));
    Put64(out.data() + 8, id);
    Put16(out.data() + 16, records.size());
    for (const BulkRecord& r : records) {
        const std::size_t at = out.size();
        out.resize(at + 4 + r.bytes.size());
        Put16(out.data() + at, static_cast<std::size_t>(r.index));
        Put16(out.data() + at + 2, r.bytes.size());
        std::memcpy(out.data() + at + 4, r.bytes.data(), r.bytes.size());
    }
    return out;
}

bool ParseRecordsBulk(const std::uint8_t* data, std::size_t size, std::uint64_t& id, std::vector<BulkRecord>& records) {
    records.clear();
    if (!data || size < 18 || std::memcmp(data, kBulkPayloadMagic, sizeof(kBulkPayloadMagic)) != 0) return false;
    id = Get64(data + 8);
    const std::size_t count = Get16(data + 16);
    std::size_t at = 18;
    for (std::size_t i = 0; i < count; ++i) {
        if (size - at < 4) return false;
        const std::size_t length = Get16(data + at + 2);
        if (length == 0 || length > kMaxRecordBytes || size - at - 4 < length) return false;
        records.push_back({static_cast<int>(Get16(data + at)), std::vector<std::uint8_t>(data + at + 4, data + at + 4 + length)});
        at += 4 + length;
    }
    return at == size;
}

void WriteBulkMarker(std::uint8_t* out, std::size_t count, std::uint64_t id) {
    out[0] = kByteArrayTag;
    out[1] = kStubLength;
    std::memcpy(out + 2, kBulkMagic, sizeof(kBulkMagic));
    out[10] = 0xFF;
    Put16(out + 11, count);
    Put64(out + 13, id);
}

bool ParseBulkMarker(const std::uint8_t* at, std::size_t available, std::size_t& count, std::uint64_t& id) {
    if (!at || available < kStubBytes || at[0] != kByteArrayTag || at[1] != kStubLength ||
        std::memcmp(at + 2, kBulkMagic, sizeof(kBulkMagic)) != 0 || at[10] != 0xFF)
        return false;
    count = Get16(at + 11);
    id = Get64(at + 13);
    return count > 0;
}

void TakeRecordsBulk(const std::uint8_t* data, std::size_t size) {
    std::uint64_t id = 0;
    std::vector<BulkRecord> records;
    if (!ParseRecordsBulk(data, size, id, records)) {
        Log("MISSION sync: a malformed bulk of loadout records (%zu bytes) was dropped", size);
        return;
    }
    Log("MISSION sync: %zu loadout records arrived in bulk (%zu bytes)", records.size(), size);
    KeepBulk(id, std::move(records));
}

}  // namespace multislot
