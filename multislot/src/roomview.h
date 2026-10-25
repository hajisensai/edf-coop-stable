#pragma once
#include <cstddef>
#include <cstdint>

namespace multislot {

// The room screen (ui::HUiRoom). Its PlayersGroup holds one panel per room member: the game builds them itself once
// RoomPanelPatches (patches.h) let its builder take Button_Master for the first and Button_Member for every later one.
// After every build (both calls go through BuildPanelsHook) a room of more than four has its rows laid out to fit the
// group (roomrows.h), and with All Forces' PvP on each row shows the member's faction (EDF6AF_PvpMemberLabel).
//
// Optional test mode: local dummy members (fakemembers.h), drawn only on this screen, so many rows can be tried
// without that many real players. Nothing about them is sent anywhere.

constexpr int kEightPlayerModKey = 0x71;  // VK_F2 belongs to 8Player MOD (hostmode.h), never to the room screen

struct RoomViewSettings {
    bool dummies = false;
    int dummyAddKey = 0x75;     // VK_F6
    int dummyRemoveKey = 0x76;  // VK_F7
};

// A pad button held down now, by channel offset; false for 0. Readable from any thread the game's UI runs on.
bool PadButtonDown(std::uint32_t channel);
// "RightStick", "LeftStick", "Back", "Start", "X", "Y", "LB", "RB", "LT", "RT", "None".
// Returns the channel offset inside a pad input slot, 0 for "None", or -1 if unknown.
int PadButtonOffset(const wchar_t* name);
// Short name for the label: "RS", "LS", "Back", ... ; "" for None or an unknown offset.
const wchar_t* PadButtonShortName(std::uint32_t offset);
// "F1".."F24", "Tab", "None", or a decimal virtual-key code. Returns -1 if unknown.
int VirtualKey(const wchar_t* name);
// "F3", "Tab" or the decimal code, for the label.
std::size_t KeyName(int key, wchar_t* out, std::size_t outChars);
// A dummy key may be neither F2 nor the other dummy key: it falls back to its default (F6 / F7), or to none when
// that is taken too. True when a key was changed.
bool ResolveDummyKeys(RoomViewSettings& settings);

void InitRoomView(const unsigned char* gameBase, const RoomViewSettings& settings);

// Installed as: the two calls to the panel builder EDF+8F8020 (RoomViewCalls) and HUiRoom's OnUpdate vtable slot
// (EDF+180AC48).
std::uint64_t BuildPanelsHook(void* room);
std::uint64_t RoomOnUpdateHook(void* room, void* context);
// What RoomOnUpdateHook calls on: the slot's value when it was written (SlotNext in patches.h), the game's
// HUiRoom::OnUpdate or another plugin's hook chained onto that slot first. InitRoomView sets the game's.
void ChainRoomOnUpdate(std::uint64_t next);

// All Forces' faction label for a room member (its src/room_pvp.cpp): 1 with `out` written and *argb its colour,
// 0 when PvP is off or the member is unknown. Looked up on EDF6VehicleCrew.dll once per room screen.
using MemberLabelFn = int (*)(const char* memberId, wchar_t* out, std::uint32_t outChars, std::uint32_t* argb);

// Test seams: the same logic with the original function supplied by the caller.
using RoomFunction = std::uint64_t (*)(void* room);
std::uint64_t CallBuilder(void* room, RoomFunction original);
// How many fake members the harness adds to the game's own member list (fakemembers.h), F6/F7 on the room screen.
void SetDummyCount(std::size_t count);
std::size_t DummyCount();
// The faction label source for the rows; the plugin's is resolved on the room screen (nullptr: none).
void SetMemberLabelSource(MemberLabelFn label);
// The member's EOS ProductUserId text from a panel's member (panel +0x200, MemberInfo): what EDF6AF_PvpMemberLabel
// is asked for. The plugin's reads it through MemberInfo -> PlayerInfo -> eos::User (identity.h); "" when unknown.
using PanelMemberIdFn = const char* (*)(const void* memberInfo, char* out, std::size_t outChars);
void SetPanelMemberIdSource(PanelMemberIdFn source);
// The game functions the layout calls, replaceable for tests.
struct RoomUiCalls {
    void(__fastcall* setArea)(void* button, const float* area);                 // 7E7FA0 HUiButton::SetArea
    void(__fastcall* setEnabled)(void* component, bool enabled);                // 7EE280
    void*(__fastcall* createByName)(void* screen, void* out, void* parent, const wchar_t* name);  // 8456D0
    void(__fastcall* setText)(void* textField, const wchar_t* text, const void* font);            // 7F8BB0
    void*(__cdecl* textField)(void* component);                                 // the component as ui::TextField
};
void SetRoomUiCalls(const RoomUiCalls& calls);
// Lays out the rows of `room`'s panels as built now (what BuildPanelsHook does after the game's builder), and the
// faction labels' text as of now (what RoomOnUpdateHook does a few times a second).
void LayoutRows(void* room);
void RefreshLabels(void* room);

}  // namespace multislot
