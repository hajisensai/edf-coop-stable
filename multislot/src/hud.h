#pragma once
#include <cstdint>

#include "midhook.h"

namespace multislot {

// The texture HudColourHooks points the status lamps at: one lamp per colour of hudcolours.h, side by side, in
// Mods\HUD\ONLINEHUDTEXTURE.RAB (tools/make_hud_colours.py).
inline constexpr wchar_t kHudLampTexture[] = L"player_lamps.dds";

// The radar marker colour of player `index` (RGBA, 0..1), as HudColourHooks hands it to the radar.
const float* RadarColour(int index);

// Handlers of HudColourHooks (patches.h).
MidHandler HudColourHookHandler(std::uint32_t rva);

}  // namespace multislot
