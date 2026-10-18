// Where a mission's player `index` starts past the game's four, relative to the spawn point: the one layout of
// EDF6Coop's MultiSlot (AngelScript missions, multislot/src/mission.cpp SpawnOffset) and of All Forces' EDF5 / EDF4.1
// script missions (src/edf5online.cpp, the BVM creation loop). One source, copied verbatim into EDF6Coop as
// multislot/src/spawn_layout.h: both plugins on every machine place a player on the same bits, whichever runtime the
// mission is (tests/edf5_online_room_test.cpp holds the bits: the same golden values belong in Coop's test).
//
// `table` is the creation loop's four finished offsets (float4 each, y 0: normalised, scaled, turned as both loops do,
// 0x1D96B0 / 0x22B490). Players 1-4 keep them; 5..32 stand along players 2-4's directions, 2x, 3x... as far (x and z
// scaled); from the 33rd they fill rings around player 1, one spacing apart (players 1-2's distance, 3 when under 0.5),
// the first ring just past the 32's lines.
//
// Deterministic by construction: + - * / and sqrt only (IEEE 754 exact rounding, SSE2 scalar), no contraction into FMA
// (the pragma; neither plugin builds with /fp:fast or /fp:contract), no CRT sin / cos (the x64 CRT picks an FMA3 path by
// the CPU, so two machines could differ in the last bit). The ring's turn: the nearest quarter turn, then Taylor to x^17
// on |x| <= pi/4 (error < 1e-16) in double, rounded once to float.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#pragma fp_contract(off)
#endif

namespace edf6spawn {
constexpr std::uint32_t kStockPlayers=4;   // the game's own offsets: players 1-4
constexpr std::uint32_t kLinePlayers=32;   // players 5..32 on the lines, 33.. on the rings

// sin and cos of `slot` / `seats` of a turn.
inline void TurnSinCos(std::uint32_t slot,std::uint32_t seats,double& s,double& c) noexcept {
    const double quarters=4.0*static_cast<double>(slot)/static_cast<double>(seats);
    const auto q=static_cast<std::uint32_t>(quarters+0.5);
    const double x=(quarters-static_cast<double>(q))*1.5707963267948966;
    const double x2=x*x;
    double sx=x,cx=1.0,term=x;
    for(int k=1;k<=8;++k){term*=-x2/static_cast<double>((2*k)*(2*k+1));sx+=term;}
    term=1.0;
    for(int k=1;k<=8;++k){term*=-x2/static_cast<double>((2*k-1)*(2*k));cx+=term;}
    switch(q&3) {
        case 0: s=sx;c=cx;break;
        case 1: s=cx;c=-sx;break;
        case 2: s=-sx;c=-cx;break;
        default: s=-cx;c=sx;break;
    }
}

// out: float4, y and w as the table's (player 1's for the rings).
inline void SpawnOffset(std::uint32_t index,const float* table,float* out) noexcept {
    if(index<kStockPlayers) {std::memcpy(out,table+index*4,4*sizeof(float));return;}
    if(index<kLinePlayers) {
        const std::uint32_t extra=index-kStockPlayers;
        std::memcpy(out,table+(1+extra%3)*4,4*sizeof(float));
        const float scale=2.0f+static_cast<float>(extra/3);
        out[0]*=scale;
        out[2]*=scale;
        return;
    }
    std::memcpy(out,table,4*sizeof(float));
    const float dx=table[4]-table[0],dz=table[6]-table[2];
    float spacing=std::sqrt(dx*dx+dz*dz);
    if(!(spacing>0.5f))spacing=3.0f;
    const float first=spacing*(2.0f+static_cast<float>((kLinePlayers-5)/3))+spacing;
    std::uint32_t slot=index-kLinePlayers;
    for(int ring=0;;++ring) {
        const float radius=first+spacing*static_cast<float>(ring);
        const auto seats=static_cast<std::uint32_t>(6.2831853f*radius/spacing);
        if(slot<seats) {
            double s=0.0,c=1.0;
            TurnSinCos(slot,seats,s,c);
            out[0]=table[0]+static_cast<float>(static_cast<double>(radius)*c);
            out[2]=table[2]+static_cast<float>(static_cast<double>(radius)*s);
            return;
        }
        slot-=seats;
    }
}
}  // namespace edf6spawn
