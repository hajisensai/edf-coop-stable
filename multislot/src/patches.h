#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "widecmp.h"

namespace multislot {

// EDF.dll build this table was taken from (Steam, 2025-01 update).
constexpr std::uint32_t kImageTimeDateStamp = 0x678CCB46;
constexpr std::uint32_t kImageSize = 0x22CE000;

constexpr int kVanillaPlayers = 4;
// What every machine is built for: user slots, packet sessions, voice chat HUD records and mission arrays for 1024
// players. The size of a room is the host's choice when creating it (hostmode.h, 2..1024); EOS keeps at most 64 of
// its members in the lobby (rooms.h, lobbystate.h), the rest are the plugin's (docs/net-re/roomsize.md).
// The game's own bounds of four were rewritten in place while they fit their operands (imm8: 0x7F at most); a
// room of 1024 does not, so those compares are widened through caves (widecmp.h, SessionCompares and
// MissionCompares), and the two other byte operands derived from the count were rewritten without it (patches.cpp).
constexpr int kMaxPlayers = 1024;
static_assert(kMaxPlayers > kVanillaPlayers && kMaxPlayers <= 0x7FFFFFFF / 0x50,
              "the widened bounds and the imm32 allocations derived from the room size");
// What Epic's services hold: an EOS lobby at most 64 members, a Steam lobby (the step a room is created and joined
// through before its EOS lobby) at most 250. A larger room keeps its size in the lobby attribute kRoomSizeKey
// (lobbystate.h) and its lobbies at these capacities; members past them are the plugin's (docs/net-re/roomsize.md).
constexpr int kEosLobbyMembers = 64;
constexpr int kSteamLobbyMembers = 250;
// The lobby attribute (int64) a MultiSlot room's size is published in.
constexpr const char* kRoomSizeKey = "MS_ROOMSIZE";
// The lobby attribute (int64) a room larger than an EOS lobby publishes its members in: the room's game has them all
// (Epic's lobby members, then those that came in over the direct link), Epic's lobby counts only its own.
constexpr const char* kRoomMembersKey = "MS_MEMBERS";

// The online HUD's per-player colour tables (HudColourPatches): one entry per colour of hudcolours.h, and the index
// they are read with wraps around them, so player 33 shares player 1's colour. Their operands are imm8, so this
// count stays below 0x80 (a lamp texture of 1024 colours is no use to anyone either).
constexpr int kHudTablePlayers = 32;
static_assert(kHudTablePlayers > kVanillaPlayers && kHudTablePlayers < 0x80, "HUD tables: imm8 operands");

// Lobby SEARCH_TYPE. Vanilla rooms publish 0x90+k (k = 1..4) and a search asks for the range
// [0x91, 0x90+m]; joining checks (v & ~0xF) == 0x90. MultiSlot rooms publish the mirror of the vanilla
// value around kSearchTypeCenter: 2*centre - v (0x18..0x1B). A modded search asks for
// [2*centre-0x90-m, 0x90+m], both families at once, and leaves out the rooms of the values between them
// (hostmode.h, lobbystate.h). Vanilla searches never reach below 0x91 and vanilla's join check
// refuses every mirrored value. Earlier MultiSlot versions cannot share a
// room of five or more with this one (0.2-0.4.1: mirror 0x8C..0x8F, enemy counts and strength not adjusted;
// 0.4.2-0.4.3: 0x7C..0x7F; 0.5.0-1.0.0: 0x74..0x77, the fifth player also raised enemy durability;
// 1.1.0-1.1.1: 0x6C..0x6F, four user slots and packet sessions, so a fifth member never got a P2P link;
// 1.2.0: 0x64..0x67, a four-record voice chat HUD that crashed everyone when the fifth member joined;
// 1.2.1-1.2.5: 0x5C..0x5F, four-entry HUD colour tables that crashed every machine in a mission of five or
// more on its first frame), so they cannot join these rooms, and this version lists their rooms but refuses
// to join them: everyone in a room has to link to everyone and simulate missions the same way. Everything
// derived from the centre is computed in patches.cpp, so a new family is this constant plus new tests.
// Raising kMaxPlayers means a new family too: a room of nine needs nine user slots on every machine in it.
// The decode above can only take centres 0x53 and 0x54 below the last one (0x90 - low mirror must stay below 0x80),
// and 0x54 lies on upstream's grid, so 0x53 is the last family this scheme has.
//
// Families so far (published values 2*centre-0x94 .. 2*centre-0x91):
//   0x74 -> 0x54..0x57  upstream 1.2.6+ and EDF6Coop 8p up to 2.2.x (eight slots)
//   0x6E -> 0x48..0x4B  EDF6Coop 10p up to 2.2.x
//   0x6A -> 0x40..0x43  EDF6Coop 12p up to 2.2.x
//   0x66 -> 0x38..0x3B  EDF6Coop 16p up to 2.2.x
//   0x5E -> 0x28..0x2B  EDF6Coop 24p up to 2.2.x
//   0x5A -> 0x20..0x23  EDF6Coop 32p up to 2.2.x
//   0x56 -> 0x18..0x1B  EDF6Coop 2.3.0 .. 2.4.x: one build with 32 slots, rooms of any size up to 32
//   0x53 -> 0x12..0x15  EDF6Coop with 1024 slots: rooms of 2..1024
// Each build's join check accepts its own family only, so a 2.2.x machine with eight slots is never let into
// a room of nine, and no family shares a value with another. The EDF6Coop families sit off upstream's grid of
// fours (0x70, 0x6C, ... are its next ones), so a later upstream family can never share a value with them.
constexpr std::uint32_t kSearchTypeCenter = 0x53;

// Bytes replaced at a fixed RVA. `original` is verified before anything is written.
struct Patch {
    const char* name;
    std::uint32_t rva;
    std::vector<std::uint8_t> original;
    std::vector<std::uint8_t> replacement;
};

// A `call rel32` whose target is verified, then pointed at a plugin function.
struct CallSite {
    const char* name;
    std::uint32_t rva;
    std::uint32_t target;
};

// An absolute function pointer (a vtable slot) whose value is verified, then replaced.
struct PointerSlot {
    const char* name;
    std::uint32_t rva;
    std::uint32_t target;
};

// Always applied: find and join both vanilla and MultiSlot rooms.
std::vector<Patch> GuestPatches();
// Always applied: every member of a room keeps a user slot and a packet session for each member, and the
// voice chat HUD keeps a record for each. eos::Users (constructor 12B77E0), eos::packet::Controller
// (constructor 12CB5F0) and UiVoiceChat_Notify (constructor 9605E0) size theirs to four, the local user
// included: a user without a slot gets no P2P link, and the HUD writes a fifth record past its vector. In
// rooms of four or fewer the extra entries stay empty, as the unused ones of smaller rooms already do.
std::vector<Patch> SessionPatches();
// The bounds of four in the same three constructors that only an imm8 held: Users' slot vector size and capacity
// checks, and the voice chat HUD's record reserve check and move limit, widened to kMaxPlayers (widecmp.h).
std::vector<WideCompare> SessionCompares();
// The voice chat HUD's record allocation (`mov ecx, 0x140; call operator new`, 9606A9/9606AE), redirected to
// VectorOperatorNew (vectoralloc.h). SessionPatches grow its size to 0x50 * kMaxPlayers, past the 4096 bytes from
// which the HUD's destructor (961930, called when a mission's HUD goes) frees it as an aligned STL block: a plain
// operator new block of that size made every mission end, retreat or win, fail fast in 961930 (2.4.1, tester
// reports of 2026-10-07).
std::vector<CallSite> SessionCalls();
// Redirected to RoomCountAndCapacity and RoomFullCount (rooms.h), in this order.
std::vector<CallSite> GuestCalls();
// Room screen member pages (roomview.h): two calls to BuildPanelsHook, then UpdateVoiceIconsHook.
std::vector<CallSite> RoomViewCalls();
// HUiRoom::OnUpdate, replaced by RoomOnUpdateHook.
PointerSlot RoomViewSlot();
// Every call to the room member list builder 7468C0, redirected to MemberListHook (fakemembers.h): the list is
// cut to kMaxPlayers members for every consumer (the voice chat HUD writes one record per member), and in test
// mode ([RoomScreen] DummyMembers=1) fake members reach every consumer of the list, not just the room screen.
std::vector<CallSite> MemberListCalls();

// Instructions at `rva` (`original`, at least 5 bytes, verified) replaced by a jump to a thunk that
// calls a handler (mission.h) and then runs original[displacedOffset, +displacedSize) before
// returning to rva + original.size(). Displaced bytes must be position-independent.
struct MidSite {
    const char* name;
    std::uint32_t rva;
    std::vector<std::uint8_t> original;
    std::size_t displacedOffset;
    std::size_t displacedSize;
};

// Rooms you host (hostmode.h): lobby capacity and published SEARCH_TYPE follow the 8Player MOD setting
// the room was created with (OFF: 4 and the vanilla value, ON: 8 and the mirrored value). Rooms you search
// for are both kinds, whatever the setting.
std::vector<MidSite> HostModeHooks();
// HUiMainFrame::OnUpdate (the menu frame), replaced by MainFrameOnUpdateHook.
PointerSlot MainFrameSlot();

// Mission phase for players 5-8 (applied with [Mission] Extend=1). With four or fewer players every
// changed instruction computes what the game computed: records and array entries 1-4 stay where the
// game reads them, only indices 4..7 reach the new storage.
std::vector<Patch> MissionPatches();
// The mission phase's index bounds of four (record replies, record stores, player objects, FindPlayerIndex, the
// player loops of two script modes, result items), widened to kMaxPlayers (widecmp.h).
std::vector<WideCompare> MissionCompares();
std::vector<MidSite> MissionHooks();
std::vector<CallSite> MissionCalls();
// The online HUD (status lamps, chat balloons, radar markers) keeps one colour per player in tables of four that
// 7FFBD0's player index reads; players 5+ read past their end, which crashed every machine on a mission's first
// frame (1.2.1-1.2.5). With Extend=1 one of two fixes is applied:
// - HudColourPatches and HudColourHooks (hud.h), when Mods\HUD\ONLINEHUDTEXTURE.RAB is ours (modfile.h): the
//   tables are built for kMaxPlayers from the archive's lamps and balloons and hudcolours.h, one colour each;
// - otherwise HudIndexWrapHooks: the index the tables are read with wraps, so player 5 shares player 1's colour.
std::vector<Patch> HudColourPatches();
std::vector<MidSite> HudColourHooks();
std::vector<MidSite> HudIndexWrapHooks();
// Callback vtable slots replaced by mission handlers (mission.h, MissionSlotHandler).
std::vector<PointerSlot> MissionSlots();

// The mission start message (packetfit.h), applied with [Mission] Extend=1: the host's MissionSync_Res record writes
// and everyone's MissionSync_Update record reads are redirected, and a hook after the host's record loop writes
// the records it held back. A message that fits one EOS packet is written exactly as before.
std::vector<CallSite> PacketFitCalls();
std::vector<MidSite> PacketFitHooks();

// Enemy spawn counts for 5-8 players (spawn.h), applied with [Mission] Extend=1 and ExtraEnemies=1.
// Only online missions with more than four players get different counts.
std::vector<MidSite> SpawnHooks();

// "copy armor" (armor.h): the three reads of this machine's armor pickup count that publish it or create a
// player with it. With the feature off every one of them hands back the count the game holds.
std::vector<MidSite> ArmorHooks();

// Lobby join and room member diagnostics (joinlog.h), installed with NetLog=1. They only log.
std::vector<MidSite> DiagnosticHooks();
std::vector<CallSite> DiagnosticCalls();
// Experimental final-hello recovery, independently switchable with HandshakeRecovery=0.
std::vector<CallSite> RecoveryCalls();
// KeepRoomOnPeerTimeout=1 (peertimeout.h): Users::Add records when each user joined, and the room update's
// "a P2P handshake timed out, so leave the room" check is redirected to PeerTimeoutLeaveCheck.
std::vector<MidSite> PeerTimeoutHooks();
// Member slots (userslots.h): Users::Add takes the slot the room's host's game has the member in; Users::Add and
// Users::Remove keep this machine's slot table. Always on.
std::vector<MidSite> UserSlotHooks();
std::vector<CallSite> PeerTimeoutCalls();

// The mission script VM's own player table (MissionScriptBVMImplement+0x168: four 0x18-byte entries, the last 16
// bytes of each a weak_ptr), filled by its CreatePlayers (22B1C0) for the split-screen players (GameStatus+0x14FF4,
// one or two). Four of its readers take a player index from the script or loop up to the online player count
// (225290: GameStatus+0x14FF8 online) and read entry index without a bound: from player 5 on they read the object's
// later fields as a weak_ptr control block and increment it (21F3C2's loop runs every player). Applied with
// [Mission] Extend=1: each read of an entry past the four - or below zero - finds it empty, as the game finds an
// unused one. Each site is `mov rdx, [entry+0x178]`, replaced by the handler (mission.h BvmPlayerEntryHandler).
std::vector<MidSite> BvmPlayerTableHooks();

// Solo test harness ([Test] GhostPlayers=N, needs Extend=1): when the online player count written by
// the mission sync is 1 (host alone), it becomes 1+N; players 2..N+1 are created as remote copies of
// the host (CreateOnlinePlayerObject looks up player 1) that nobody controls.
std::vector<MidSite> GhostHooks();
std::vector<CallSite> GhostCalls();

// Host data (hostdataopen.h): the game's file open (741C0, the function EDFModLoader wraps to read Mods\), at the
// point where rdi is the path's characters and is about to be handed to the open (`lea r8, [rbp-0x28];
// mov rdx, rdi`). The handler may point rdi at another path; both moves run after it.
std::vector<MidSite> HostDataHooks();

// Other players' weapon ids this machine's table does not have (weaponguard.h). [0]: the room info parser 744AB0
// right after it stored a member's six ids (`lea r12, [r13+0x38]`, r13 the PlayerInfo; the nop after it is not
// kept). [1]: the mission start message 790600 at 790887 (`imul r8, rcx, 0xD4`), the decoded record at rbp+0x20,
// before it is copied anywhere - the same site as MissionHooks' "loadout record MissionSync header", so it is
// installed only when the mission phase is not (plugin.cpp runs the guard inside the mission handler otherwise).
std::vector<MidSite> WeaponGuardHooks();

bool Matches(const std::uint8_t* at, const Patch& patch);
bool CallTargets(const std::uint8_t* at, std::uint32_t siteRva, std::uint32_t targetRva);
bool SlotTargets(const std::uint8_t* at, std::uint64_t imageBase, std::uint32_t targetRva);

}  // namespace multislot
