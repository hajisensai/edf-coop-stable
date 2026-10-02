#pragma once
#include <cstdint>

// One colour per player for the online HUD: the status lamp (player_lamp.dds, one lamp per colour side by side),
// the chat balloon (one chat_<name>.dds each) and the radar marker. The game has four - yellow, green, blue, red -
// and these extend them; tools/make_hud_colours.py reads this table to paint the textures of
// assets/ONLINEHUDTEXTURE.RAB, so the textures and the radar never disagree. Players 5-8 take the colours of the
// 8-player textures FevGrave made (orange, pink, purple, cyan); the rest follow Kelly's colours of maximum contrast.
// Append only: a colour's position is the player it belongs to, in every build and in the archive.
// Keep every line `{L"chat_<name>.dds", 0xRRGGBB},` - the tool parses them.

namespace multislot {

struct HudColour {
    const wchar_t* chatTexture;
    std::uint32_t rgb;
};

inline constexpr HudColour kHudColours[] = {
    {L"chat_Yellow.dds", 0xFFFF00},  // the game's four: textures kept as they are, radar colours as the game's
    {L"chat_Green.dds", 0x00FF00},
    {L"chat_Blue.dds", 0x0000FF},
    {L"chat_Red.dds", 0xFF0000},
    {L"chat_Orange.dds", 0xFF8C00},
    {L"chat_Pink.dds", 0xFF78C8},
    {L"chat_Purple.dds", 0x9B3CE6},
    {L"chat_Cyan.dds", 0x00E6F0},
    {L"chat_White.dds", 0xF2F3F4},
    {L"chat_LightBlue.dds", 0xA1CAF1},
    {L"chat_Buff.dds", 0xC2B280},
    {L"chat_Gray.dds", 0x848482},
    {L"chat_DarkGreen.dds", 0x008856},
    {L"chat_Azure.dds", 0x0067A5},
    {L"chat_Salmon.dds", 0xF99379},
    {L"chat_Violet.dds", 0x604E97},
    {L"chat_Amber.dds", 0xF6A600},
    {L"chat_Wine.dds", 0xB3446C},
    {L"chat_Citron.dds", 0xDCD300},
    {L"chat_Rust.dds", 0x882D17},
    {L"chat_Lime.dds", 0x8DB600},
    {L"chat_Brown.dds", 0x654522},
    {L"chat_Vermilion.dds", 0xE25822},
    {L"chat_Olive.dds", 0x2B3D26},
    {L"chat_Black.dds", 0x222222},
    {L"chat_Magenta.dds", 0xFF00FF},
    {L"chat_Teal.dds", 0x008080},
    {L"chat_Mint.dds", 0x98FF98},
    {L"chat_Lavender.dds", 0xC8A2FF},
    {L"chat_Maroon.dds", 0x800000},
    {L"chat_Sky.dds", 0x87CEEB},
    {L"chat_Indigo.dds", 0x4B0082},
};

inline constexpr int kHudColourCount = static_cast<int>(sizeof(kHudColours) / sizeof(kHudColours[0]));

}  // namespace multislot
