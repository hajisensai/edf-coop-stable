#include "hud.h"

#include <array>
#include <cstring>

#include "hudcolours.h"

namespace multislot {
namespace {

struct alignas(16) Float4 {
    float rgba[4];
};

constexpr Float4 RadarEntry(std::uint32_t rgb) {
    return {{static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
             static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f}};
}

constexpr std::array<Float4, kHudColourCount> RadarTable() {
    std::array<Float4, kHudColourCount> table{};
    for (int i = 0; i < kHudColourCount; ++i) table[i] = RadarEntry(kHudColours[i].rgb);
    return table;
}

// Where the radar's four-entry table on the stack was; read in place of it.
constexpr std::array<Float4, kHudColourCount> kRadar = RadarTable();

std::uint64_t Address(const void* pointer) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)); }

// `lea r8, [player_lamp.dds]` in the lamp loop of HudPlayer_MultiPlayStatus.
void LampTextureHandler(CpuContext* context) { context->r8 = Address(kHudLampTexture); }

// `mov r8, [rbp+rax*8+7]`: rax is the balloon being made, below the table's size (at most kMaxPlayers).
void ChatTextureHandler(CpuContext* context) { context->r8 = Address(kHudColours[context->rax].chatTexture); }

// `movsxd rax, [rsp+0x44]; shl rax, 4; lea r9, [rbp+0x150]; add r9, rax`.
void RadarColourHandler(CpuContext* context) {
    std::int32_t index = 0;
    std::memcpy(&index, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(SiteRsp(context) + 0x44)), sizeof(index));
    context->r9 = RadarColourAddress(index, context->rbp);
}

}  // namespace

const float* RadarColour(int index) { return kRadar[static_cast<std::size_t>(index)].rgba; }

std::uint64_t RadarColourAddress(std::int32_t index, std::uint64_t rbp) {
    if (index < 0) return rbp + 0x150 + static_cast<std::uint64_t>(static_cast<std::int64_t>(index) * 16);
    return Address(RadarColour(index % kHudColourCount));
}

MidHandler HudColourHookHandler(std::uint32_t rva) {
    switch (rva) {
        case 0x8074E7: return &LampTextureHandler;
        case 0x802B9B: return &ChatTextureHandler;
        case 0x82A6CD:
        case 0x82A743: return &RadarColourHandler;
        default: return nullptr;
    }
}

}  // namespace multislot
