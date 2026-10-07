#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "code.h"

namespace multislot {

// A `cmp r/m, imm8` (83 /7 ib, optionally behind a REX prefix) and the conditional jump right after it, in the game's
// code, where the bound the plugin needs no longer fits the sign-extended imm8 (0x7F at most: a room of 1024 does
// not). The pair is replaced by a `jmp rel32` to a cave that compares against a 32-bit bound (81 /7 id, same operand)
// and leaves through the same two exits the jcc had: its target, or the instruction after the pair. Flags are what
// the wide compare set, exactly as the game's jcc would have read them from its own compare.
//
// `original` is the cmp followed by the jcc (70..7F rel8 or 0F 80..8F rel32); `compareSize` is the cmp's length. A
// cmp with a RIP-relative operand is refused (the cave is elsewhere), as is anything that is not one cmp and one jcc.
struct WideCompare {
    const char* name;
    std::uint32_t rva;
    std::vector<std::uint8_t> original;
    std::size_t compareSize;
    std::uint32_t bound;
};

// What a site decodes to; `valid` false when it is not the shape described above.
struct WideCompareShape {
    bool valid = false;
    std::size_t prefix = 0;        // 0 or 1 (REX)
    std::size_t operandBytes = 0;  // ModRM, SIB and displacement, copied as they are
    std::uint8_t condition = 0;    // the jcc's condition code (0..15)
    std::int64_t displacement = 0; // the jcc's, relative to the end of `original`
};
WideCompareShape DecodeWideCompare(const WideCompare& site);

// The cave for `site` as it sits at `siteAddress` (the game's address of `original`); empty when the site is not valid.
// Position-independent: it reaches both exits through absolute indirect jumps, so it may lie anywhere.
std::vector<std::uint8_t> WideCompareCode(const WideCompare& site, std::uint64_t siteAddress);

// Emits the cave into `page` for the site at `at`; nullptr when the site is not valid or the page is full. The cave
// keeps the stack as it is, so it unwinds like the game function it was taken from would at that point - except that
// the system does not know it is part of that function; only the wide compare in it can fault, on the very operand the
// game's own compare read.
unsigned char* EmitWideCompare(ThunkPage& page, const WideCompare& site, const unsigned char* at);

}  // namespace multislot
