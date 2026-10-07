#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "patches.h"  // kMaxPlayers

#include <cstddef>
#include <cstdint>
#include <vector>

#include "midhook.h"

namespace multislot {

// The mission start sync (EDF.dll 678CCB46) and what it takes to fit EOS's packet size.
//
// When everyone has answered, the host's MissionSync_Res (78D0E0) writes one message for the whole room:
// three ints, the player count, then every player's loadout record (773840: two ints, the index, two ints,
// eight colours as 24 floats, six ints and a 48-byte block - about 143 bytes with the game's compact number
// encoding, never more than 222). The message goes to every member wrapped by 74D3E0 and 750380 and framed by
// eos::packet::Controller (12D0AC0, 12 bytes) and the per-peer datagram (12CFFD0, 8 bytes). EOS refuses anything
// above 1170 bytes (EOS_P2P_MAX_PACKET_SIZE); the game only refuses above 1400 (12D0BE6), so a refused sync is
// resent eight times over ~26 s and then every member is dropped. Eight players made 1180/1181 bytes. The game's
// byte streams are 0x5E0 bytes and written without a bound (12B5200 and the other writers), so twelve records
// (~1720 bytes) would also run past the host's stream.
//
// So the host writes records inline only while the message stays within kMissionSyncBudget; the rest (players 5+
// first, from the highest index down) are replaced by a 21-byte stub and sent beside the sync as packets of their
// own. Everyone's MissionSync_Update (790600) reads a stub back as the record it stands for. A message that fits
// is written byte for byte as before, so rooms of four (and larger rooms whose sync fits) are unchanged.

// Records kept for the side packets: four syncs of a full room, and never fewer than the 64 the rooms of up to
// 16 were sized for. The oldest makes room for a new one.
constexpr std::size_t kRecordStoreEntries = 4 * kMaxPlayers > 64 ? 4 * kMaxPlayers : 64;

constexpr std::size_t kEosMaxPacket = 1170;      // EOS_P2P_MAX_PACKET_SIZE
constexpr std::size_t kSessionHeader = 8;        // 12CFFD0: header of each datagram to a peer
constexpr std::size_t kControllerHeader = 12;    // 12D0AC0: eos::packet::Controller header of each packet
constexpr std::size_t kMessageHeaders = 26;      // 750380/7504A0 (at most 12) + 74D3E0 (at most 14)
// The sync is appended to a per-peer builder that only sends once it holds more than 250 bytes (761E60, 0xFA
// from 74ED6D): a few small messages queued in the same frame can share the packet.
constexpr std::size_t kBatchedAllowance = 24;
constexpr std::size_t kMissionSyncBudget =
    kEosMaxPacket - kSessionHeader - kControllerHeader - kMessageHeaders - kBatchedAllowance;  // 1100
static_assert(kMissionSyncBudget == 1100, "sync budget");

// The game's byte stream (0x5F8-byte object, vtable 1AF41D0): read position, data, size.
constexpr std::size_t kStreamPosition = 0x8;
constexpr std::size_t kStreamData = 0x10;
constexpr std::size_t kStreamCapacity = 0x5E0;
constexpr std::size_t kStreamSize = 0x5F0;
constexpr std::size_t kStreamObjectSize = 0x5F8;
static_assert(kMissionSyncBudget < kStreamCapacity, "the sync always fits the game's stream");

constexpr std::size_t kMaxRecordBytes = 256;  // one serialised loadout record is at most 222
constexpr std::size_t kStubBytes = 21;        // a byte array (0xA0, 19) no record ever starts with
constexpr std::size_t kSideHeader = 19;
constexpr std::uint8_t kSideChannel = 0x4D;   // the game ignores the channel on receive; it sends on 0

struct RecordRef {
    int index;
    std::size_t size;
};
// Which records stay inline. All of them when `header` plus their sizes fit `budget`; otherwise records are
// moved out, players 5+ first and each group from the highest index down, until the message fits.
std::vector<bool> PlanInline(std::size_t header, const std::vector<RecordRef>& records, std::size_t budget);

struct StubInfo {
    int index = -1;
    std::size_t size = 0;
    std::uint64_t hash = 0;
};
std::uint64_t RecordHash(const std::uint8_t* data, std::size_t size);  // FNV-1a
void WriteStub(std::uint8_t* out, const StubInfo& stub);                // kStubBytes
bool ParseStub(const std::uint8_t* at, std::size_t available, StubInfo& stub);
// Stubs inside a raw packet; returns how many were found (only the first `max` are stored).
std::size_t FindStubs(const std::uint8_t* data, std::size_t size, StubInfo* out, std::size_t max);

std::size_t BuildSidePacket(const StubInfo& stub, const std::uint8_t* record, std::uint8_t* out, std::size_t capacity);
bool IsSidePacket(const std::uint8_t* data, std::size_t size);
// A side packet whose size and hash match its record.
bool ParseSidePacket(const std::uint8_t* data, std::size_t size, StubInfo& stub, const std::uint8_t*& record);

// Records sent beside a sync, on the host and on every member (a few syncs' worth, oldest replaced first).
bool StoreRecord(const StubInfo& stub, const std::uint8_t* record);  // false when it was there already
bool FindRecord(const StubInfo& stub, std::uint8_t* out);           // copies stub.size bytes
// Forgets the records and the packets held for them (leaving a room).
void ClearRecords();

// Game glue. The record writer/reader of the game (773840 / 773740), or stand-ins in the tests.
using RecordWriteFn = bool(__fastcall*)(void* context, const void* record, void* stream, std::int32_t armorLimit);
using RecordReadFn = bool(__fastcall*)(void* context, void* record, void* stream);
void InitPacketFit(const unsigned char* gameBase);
void SetRecordFunctions(RecordWriteFn write, RecordReadFn read);
bool __fastcall RecordWriteHook(void* context, const void* record, void* stream, std::int32_t armorLimit);  // 78D6FA
bool __fastcall RecordReadHook(void* context, void* record, void* stream);                                   // 790873
void FlushRecords(void* stream);  // after MissionSync_Res's record loop (78D756)
void* PacketFitCallHandler(std::uint32_t rva);
MidHandler PacketFitHookHandler(std::uint32_t rva);

// EOS P2P transport. The game encrypts every datagram (AES-CTR with a CRC32C, 12CEA10, before EOS_P2P_SendPacket;
// decrypted in 12CDF20 after EOS_P2P_ReceivePacket), so a stub is never visible in what passes these wrappers: it
// only exists in the plaintext message, which FlushRecords and RecordReadHook see. (Until 2026-10-05 the wrappers
// looked for stubs in the ciphertext, never found one, and no record was ever sent: every split sync left its
// players out on the members' machines.)
//
// So the host remembers the records its last sync moved out (FlushRecords), and the first packet that goes to a
// member after that carries them ahead of itself as side packets of our own (reliable, kSideChannel, not
// encrypted by the game): the sync is written before it is sent, so the records go before it. Every member gets
// them, marker or not (SendRecordsAhead says why). A member takes side packets out of what its game receives. When
// its game reads a stub whose record is not here yet (they travel apart), RecordReadHook waits for it up to
// kRecordWaitMs, receiving from EOS itself: side packets are stored, anything else is held and handed to the game,
// in order, before anything newer. A held packet is never dropped (EOS acknowledged it and will not send it again):
// it waits only for as long as the game's frame waits.
using EosResult = std::int32_t;
struct EosSendOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const void* RemoteUserId;
    const void* Socket;
    std::uint8_t Channel;
    std::uint32_t DataLengthBytes;
    const void* Data;
    std::int32_t AllowDelayedDelivery;
    std::int32_t Reliability;
    std::int32_t DisableAutoAccept;
};
static_assert(offsetof(EosSendOptions, Channel) == 0x20 && offsetof(EosSendOptions, Data) == 0x28 &&
                  offsetof(EosSendOptions, Reliability) == 0x34,
              "EOS_P2P_SendPacketOptions");
using EosSendFn = EosResult (*)(void* handle, const EosSendOptions* options);
using EosReceiveFn = EosResult (*)(void* handle, const void* options, void** peer, void* socket, std::uint8_t* channel,
                                   void* data, std::uint32_t* size);
EosResult PacketFitSend(void* handle, const EosSendOptions* options);
EosResult PacketFitReceive(void* handle, const void* options, void** peer, void* socket, std::uint8_t* channel, void* data,
                           std::uint32_t* size);
void SetEosFunctions(EosSendFn send, EosReceiveFn receive);
constexpr EosResult kEosLimitExceeded = 22;  // EOS_LimitExceeded
constexpr unsigned long long kRecordWaitMs = 1500;
// How long RecordReadHook waits for a missing record (tests: 0, only what has arrived).
void SetRecordWait(unsigned long long ms);
// How long the records of the last sync still go ahead of packets to members that have not had them.
constexpr unsigned long long kRecordsAheadMs = 30000;
// Asked about every member the records of a split sync go to (syncmarker.h: PeerReadsSplitSync), which logs the
// members that show no marker that they read one. It decides nothing: they get the records all the same.
using SplitSyncReaders = bool (*)(const void* remote);
void SetSplitSyncReaders(SplitSyncReaders readers);
// Test: the start message holds at most `budget` bytes of records inline (0 or kMissionSyncBudget and above: the
// normal budget), so a small room exercises the side packets ([Test] SplitSyncBudget).
void SetSyncBudget(std::size_t budget);
// The clock held packets age by (GetTickCount64 unless a test sets its own).
using PacketFitClock = unsigned long long (*)();
void SetPacketFitClock(PacketFitClock clock);
std::size_t HeldPacketCount();
using ImportRedirect = bool (*)(HMODULE game, const char* dll, const char* function, void* replacement, void** original);
// Redirects EOS_P2P_SendPacket and EOS_P2P_ReceivePacket; returns how many. Install it before the net log so the
// net log's wrappers (which need the game as their caller) sit in front.
int InstallPacketFit(HMODULE game, ImportRedirect redirect);

// --- every record in bulk (I1: rooms of up to 1024) ---
// About 50 players fill the start message with stubs alone (21 bytes each), and the game's stream (0x5E0 bytes) the
// same way. When the whole room reads fragments (src/netcode.h, NetFeature::Fragments) and the records do not fit,
// the message keeps none of them: one bulk marker (kStubBytes, laid out as a stub with kBulkMagic, index 0xFF, then
// the record count and the bulk's id) stands for all, and every record goes to every member as one bulk message
// (SendBulk, tag kRecordsBulkTag: 1024 records are about 140 KiB). Everyone's MissionSync_Update reads the marker
// back as each record in turn (RecordReadHook stays on it until the last), waiting for the bulk up to kRecordWaitMs
// plus kBulkWaitMsPerKiB per KiB. A room that does not read fragments keeps the stubs and side packets above.
constexpr std::uint16_t kRecordsBulkTag = 0x5352;
constexpr unsigned long long kBulkWaitMsPerKiB = 30;  // 32 KiB/s, half the game's own budget for a member
// Whether the room reads bulk messages now, and how one goes to `remote` (an EOS_ProductUserId): the room part wires
// them to netfeature.h. Unset: never bulk.
using BulkReady = bool (*)();
using BulkSend = bool (*)(const void* remote, std::uint16_t tag, const void* data, std::size_t size);
// The bulk of records from `peer` (an EOS_ProductUserId) on its way: its id (0: none; some of its fragments here,
// not all), its size in `total`. One its receiver gave up on (no progress for a while, src/fragment.h) is not.
using BulkIncoming = std::uint64_t (*)(const void* peer, std::size_t* total);
// While one is, the game's packets from that member wait here (in order) instead of reaching the game: its start
// message comes after its records, so the game reads the message once the records are here, and its frame never waits
// for them (RecordReadHook waits only when the message overtook every fragment, at most once per bulk). The game's
// start message is encrypted, so it cannot be told from the member's other packets: they all wait, and all reach the
// game in order - none is ever dropped. They wait until the bulk arrives or stops being on its way (its sender gave
// up), or for at most BulkHoldMs(total) (the bulk at 32 KiB/s, kBulkWaitMsPerKiB, plus kRecordWaitMs: 143 KiB, a room
// of 1024, is 5.8 s, well inside the ~26 s the sender's game resends unacknowledged messages before it ends the
// room) and BulkHoldCapacity(total) packets (kBulkHoldPacketsPerSecond over that time; per member, so the room's
// size scales the whole), whichever comes first: then the bulk is no longer waited for here, the held packets go to
// the game in order, and the start message waits for the records once inside RecordReadHook.
void SetBulkRecords(BulkReady ready, BulkSend send, BulkIncoming incoming = nullptr);
constexpr std::size_t kBulkHoldPacketsPerSecond = 120;  // twice a game sending every 60 Hz frame
constexpr std::size_t kBulkHoldMinPackets = 64;
// What every member's held packets may cost together: past it every bulk that holds packets is let go (they go to the
// game in order, as when one bulk outgrows BulkHoldCapacity), so many members' bulks at once never grow the store
// without a bound.
constexpr std::size_t kHeldBytesCap = 1 << 20;
unsigned long long BulkHoldMs(std::size_t total);
std::size_t BulkHoldCapacity(std::size_t total);
// The receiver's side: a bulk message of tag kRecordsBulkTag arrived.
void TakeRecordsBulk(const std::uint8_t* data, std::size_t size);
struct BulkRecord {
    int index;
    std::vector<std::uint8_t> bytes;
};
std::vector<std::uint8_t> BuildRecordsBulk(std::uint64_t id, const std::vector<BulkRecord>& records);
bool ParseRecordsBulk(const std::uint8_t* data, std::size_t size, std::uint64_t& id, std::vector<BulkRecord>& records);
void WriteBulkMarker(std::uint8_t* out, std::size_t count, std::uint64_t id);  // kStubBytes
bool ParseBulkMarker(const std::uint8_t* at, std::size_t available, std::size_t& count, std::uint64_t& id);

// Diagnostic: one line per kind (caller, channel, packet type) of game packet above kEosMaxPacket.
bool FirstOversize(std::uintptr_t caller, std::uint8_t channel, const std::uint8_t* data, std::size_t size);
void DescribeBytes(const std::uint8_t* data, std::size_t size, char* out, std::size_t capacity);
void LogOversizePacket(std::uintptr_t caller, std::uint8_t channel, std::int32_t reliability, const void* data,
                       std::uint32_t size, EosResult result);

}  // namespace multislot
