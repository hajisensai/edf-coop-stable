#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "code.h"

namespace multislot {

// Registers as a mid-function thunk saved them, lowest address first. A handler may change any of
// them (including xmm0-xmm5); the thunk loads the changed values back before it runs the displaced
// instructions and returns to the game. `returnAddress` is not the game's: the thunk puts the site's resume
// address there so that it unwinds like a function the game called (see MidThunkCode); leave it alone.
struct CpuContext {
    std::uint8_t xmm[6][16];  // xmm0..xmm5
    std::uint64_t rflags;
    std::uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    std::uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    std::uint64_t returnAddress;
};

using MidHandler = void (*)(CpuContext* context);

// The game's rsp at the hooked instruction (its locals are addressed from here or from rbp).
inline std::uint64_t SiteRsp(const CpuContext* context) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&context->returnAddress)) +
           sizeof(context->returnAddress);
}

// Thunk: save every general register, flags and xmm0-5, call `handler` on a 16-byte aligned stack,
// restore, run `displaced` (position-independent instructions copied from the site) and jump to
// `resume`.
//
// The game jumps here, so nothing on the stack says where it came from, and a fault in a handler could not be
// unwound past the thunk: the crash log's call stack stopped there, and the system could not reach the game's own
// exception handlers above it. So the thunk first pushes `resume` as if the game had called it from there, and
// describes its pushes and its frame (rbx) in MidThunkUnwind; EmitMidThunk registers that with the page.
std::vector<std::uint8_t> MidThunkCode(MidHandler handler, const std::uint8_t* displaced, std::size_t displacedSize,
                                       std::uint64_t resume);
// The UNWIND_INFO of every thunk, and how many of its first bytes it describes: everything up to the point where
// the pushed resume address is dropped again. The displaced instructions after that run on the game's own frame,
// which only the game's unwind data could describe; they are copies of the game's position-independent
// instructions, so what faults there would have faulted in the game.
std::vector<std::uint8_t> MidThunkUnwind();
std::size_t MidThunkFrameBytes();

// Emits the thunk into `page`, with its unwind data; nullptr when the page is full.
unsigned char* EmitMidThunk(ThunkPage& page, MidHandler handler, const std::uint8_t* displaced,
                            std::size_t displacedSize, std::uint64_t resume);

}  // namespace multislot
