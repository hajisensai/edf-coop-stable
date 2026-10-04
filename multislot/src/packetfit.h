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

// EOS P2P transport: side packets go out ahead of every packet that carries a stub (so a resent sync resends
// them too) and are taken out of what the game receives. A packet whose stubs are not all known yet is held
// until their records arrive and then handed to the game: the sync goes out reliably (EDF6DirectNet), so it is
// acknowledged on arrival and dropping it would cost the game a resend about three seconds later. Held packets
// are bounded (kHeldPackets, oldest dropped) and given up after kHeldPacketMs, longer than the ~26 s over which
// the game resends a sync (see the top of this file). A held packet reaches the game after packets that arrived
// behind it; dropping it, as before, reordered the same way.
//
// A packet with a stub only goes to a member that reads it (SetSplitSyncReaders). Anyone else gets neither it nor
// its side packets and the game is told EOS_LimitExceeded - what EOS answers for a packet above its limit (the
// 2026-09-30 DirectNet logs: "EOS SendPacket ... failed: EOS_LimitExceeded"), i.e. what that member got before the
// split - since a machine without the split reads a stub as garbage (syncmarker.h).
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
constexpr std::size_t kHeldPackets = 8;
constexpr unsigned long long kHeldPacketMs = 30000;
// Who reads a split sync (syncmarker.h: PeerReadsSplitSync). Unset: nobody, so no split sync is ever sent.
using SplitSyncReaders = bool (*)(const void* remote);
void SetSplitSyncReaders(SplitSyncReaders readers);
// --- packed packets (bandwidth) ---
// A member that publishes kPackedFormat (syncmarker.h) reads packed packets: kPackHeader bytes (a magic and the
// original size), then the original bytes compressed with XPRESS (ntdll RtlCompressBuffer: fast, part of Windows).
// Every game packet of kPackMinimum bytes and more goes to such a member packed when that saves at least
// kPackSavingMin bytes and kPackSavingPercent of it; the receiver unpacks it before anything else looks at it. The
// game reads packets into a 4096-byte buffer (12C8D1D) and sends up to kGameMaxPacket bytes (12D0BE6), so a start
// message of up to kPackedSyncLimit bytes keeps all its records once it fits one EOS packet packed.
constexpr std::size_t kPackHeader = 10;
constexpr std::size_t kPackMinimum = 200;
constexpr std::size_t kPackSavingMin = 16;
constexpr std::size_t kPackSavingPercent = 5;
constexpr std::size_t kMaxUnpacked = 4096;
constexpr std::size_t kGameMaxPacket = 1400;
constexpr std::size_t kPackedSyncLimit =
    kGameMaxPacket - kSessionHeader - kControllerHeader - kMessageHeaders - kBatchedAllowance;  // 1330
// XPRESS is there (ntdll); without it nothing is packed and kSplitSyncFormat is published.
bool PackingAvailable();
// `data` packed into `out` (capacity bytes); 0 when it is too small to pack, packing saves too little, or the
// result does not fit.
std::size_t PackPacket(const std::uint8_t* data, std::size_t size, std::uint8_t* out, std::size_t capacity);
bool IsPackedPacket(const std::uint8_t* data, std::size_t size);
// A packed packet's original bytes into `out`; 0 when it is not one, is damaged, or its original does not fit.
std::size_t UnpackPacket(const std::uint8_t* data, std::size_t size, std::uint8_t* out, std::size_t capacity);
// Who reads packed packets (syncmarker.h PeerReadsPacked), and whether every member of the room does now. Unset:
// nobody, and nothing is packed.
using PackedReaders = bool (*)(const void* remote);
using RoomReadsPacked = bool (*)();
void SetPackedReaders(PackedReaders readers, RoomReadsPacked room);

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

// Diagnostic: one line per kind (caller, channel, packet type) of game packet above kEosMaxPacket.
bool FirstOversize(std::uintptr_t caller, std::uint8_t channel, const std::uint8_t* data, std::size_t size);
void DescribeBytes(const std::uint8_t* data, std::size_t size, char* out, std::size_t capacity);
void LogOversizePacket(std::uintptr_t caller, std::uint8_t channel, std::int32_t reliability, const void* data,
                       std::uint32_t size, EosResult result);

}  // namespace multislot
