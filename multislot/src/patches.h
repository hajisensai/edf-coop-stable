#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace multislot {

// EDF.dll build this table was taken from (Steam, 2025-01 update).
constexpr std::uint32_t kImageTimeDateStamp = 0x678CCB46;
constexpr std::uint32_t kImageSize = 0x22CE000;

constexpr int kVanillaPlayers = 4;
// What every machine is built for: user slots, packet sessions, HUD records and mission arrays for 32 players.
// The size of a room is the host's choice when creating it (hostmode.h, 5..32) and lives in its lobby's
// MaxMembers, so every room any of us hosts can be joined by any of us (2.3.0; before, each size was a build).
constexpr int kMaxPlayers = 32;
// The signed imm8/disp8 operands written from the room size (patches.cpp: `cmp r, N`, the destructor's
// N-0x10, FindPlayerIndex's -(N+1)) hold up to 0x7E players; EOS lobbies hold 64 members.
static_assert(kMaxPlayers > kVanillaPlayers && kMaxPlayers <= 32,
              "5..32 players: the room-size operands patched into the game and the loadout log bits (mission.cpp)");

// Lobby SEARCH_TYPE. Vanilla rooms publish 0x90+k (k = 1..4) and a search asks for the range
// [0x91, 0x90+m]; joining checks (v & ~0xF) == 0x90. MultiSlot rooms publish the mirror of the vanilla
// value around kSearchTypeCenter: 2*centre - v (0x18..0x1B). With the Player MOD ON a modded search asks for
// [2*centre-0x90-m, 2*centre-0x91], MultiSlot rooms of the kinds asked for only; OFF it asks for what the game
// asks, normal rooms only (hostmode.h). Vanilla searches never reach below 0x91 and vanilla's join check
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
//
// Families so far (published values 2*centre-0x94 .. 2*centre-0x91):
//   0x74 -> 0x54..0x57  upstream 1.2.6+ and EDF6Coop 8p up to 2.2.x (eight slots)
//   0x6E -> 0x48..0x4B  EDF6Coop 10p up to 2.2.x
//   0x6A -> 0x40..0x43  EDF6Coop 12p up to 2.2.x
//   0x66 -> 0x38..0x3B  EDF6Coop 16p up to 2.2.x
//   0x5E -> 0x28..0x2B  EDF6Coop 24p up to 2.2.x
//   0x5A -> 0x20..0x23  EDF6Coop 32p up to 2.2.x
//   0x56 -> 0x18..0x1B  EDF6Coop 2.3.0+: one build with 32 slots, rooms of any size
// Each build's join check accepts its own family only, so a 2.2.x machine with eight slots is never let into
// a room of nine, and no family shares a value with another. The EDF6Coop families sit off upstream's grid of
// fours (0x70, 0x6C, ... are its next ones), so a later upstream family can never share a value with them.
constexpr std::uint32_t kSearchTypeCenter = 0x56;

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
// for follow the current setting (OFF: normal and MultiSlot rooms, ON: MultiSlot rooms only).
std::vector<MidSite> HostModeHooks();
// HUiMainFrame::OnUpdate (the menu frame), replaced by MainFrameOnUpdateHook.
PointerSlot MainFrameSlot();
// HUiLobby::OnUpdate (the room list screen), replaced by LobbyOnUpdateHook: F2 there searches again.
PointerSlot LobbySlot();

// Mission phase for players 5-8 (applied with [Mission] Extend=1). With four or fewer players every
// changed instruction computes what the game computed: records and array entries 1-4 stay where the
// game reads them, only indices 4..7 reach the new storage.
std::vector<Patch> MissionPatches();
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
std::vector<CallSite> PeerTimeoutCalls();

// Solo test harness ([Test] GhostPlayers=N, needs Extend=1): when the online player count written by
// the mission sync is 1 (host alone), it becomes 1+N; players 2..N+1 are created as remote copies of
// the host (CreateOnlinePlayerObject looks up player 1) that nobody controls.
std::vector<MidSite> GhostHooks();
std::vector<CallSite> GhostCalls();

// Host data (hostdataopen.h): the game's file open (741C0, the function EDFModLoader wraps to read Mods\), at the
// point where rdi is the path's characters and is about to be handed to the open (`lea r8, [rbp-0x28];
// mov rdx, rdi`). The handler may point rdi at another path; both moves run after it.
std::vector<MidSite> HostDataHooks();

bool Matches(const std::uint8_t* at, const Patch& patch);
bool CallTargets(const std::uint8_t* at, std::uint32_t siteRva, std::uint32_t targetRva);
bool SlotTargets(const std::uint8_t* at, std::uint64_t imageBase, std::uint32_t targetRva);

}  // namespace multislot
