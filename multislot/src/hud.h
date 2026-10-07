#pragma once
#include <cstdint>

#include "midhook.h"

namespace multislot {

// The texture HudColourHooks points the status lamps at: one lamp per colour of hudcolours.h, side by side, in
// Mods\HUD\ONLINEHUDTEXTURE.RAB (tools/make_hud_colours.py).
inline constexpr wchar_t kHudLampTexture[] = L"player_lamps.dds";

// The radar marker colour of player `index` (RGBA, 0..1), as HudColourHooks hands it to the radar.
const float* RadarColour(int index);

// What the radar's colour lookup (82A6CD / 82A743) gets for the index 7FFBD0 left at rsp+0x44 (`rbp` the radar's
// frame): the colour of the index, wrapped around the table, for a player; for a negative index (-1, a unit that is
// no player of the mission) the in-frame address the game computes itself (rbp + 0x150 + index*16), as without the
// plugin. 7FFBD0 already hands out a wrapped index online (PlayerTagIndexHandler, 7FFD95) and the split-screen
// number offline (7FFDF4), so the wrap here is not what keeps the read bounded today; it keeps it bounded whatever
// index reaches the radar, instead of falling back to rbp+0x150+index*16 - past the frame - for one >= the table.
std::uint64_t RadarColourAddress(std::int32_t index, std::uint64_t rbp);

// Handlers of HudColourHooks (patches.h).
MidHandler HudColourHookHandler(std::uint32_t rva);

}  // namespace multislot
