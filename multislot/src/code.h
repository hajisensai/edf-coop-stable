#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace multislot {

bool WriteCode(unsigned char* at, const unsigned char* bytes, std::size_t size);

// Stubs and mid-function thunks in one block within rel32 reach of `anchor`. The code in it is described to the
// system's unwinder (a function table registered by Seal), so a fault in a handler called through a stub or a
// thunk unwinds through it: to the crash log's call stack, and to the game's own exception handlers above it.
class ThunkPage {
public:
    bool Allocate(const unsigned char* anchor);
    // `mov rax, target; jmp rax`, reached by the game's `call`: it unwinds as a leaf (the return address is on
    // top of the stack all the way through). Returns the stub address, or nullptr when full or not allocated.
    unsigned char* Add(void* target);
    // Copies code or data the unwinder is not told about; returns its address, or nullptr when full or not
    // allocated.
    unsigned char* Emit(const unsigned char* code, std::size_t size);
    // Copies code whose first `covered` bytes unwind by `unwind` (an UNWIND_INFO, copied next to it); returns its
    // address, or nullptr when full or not allocated.
    unsigned char* EmitFunction(const unsigned char* code, std::size_t size, std::size_t covered,
                                const std::uint8_t* unwind, std::size_t unwindSize);
    // Makes the page executable and read-only, and registers its function table (Unwindable() says whether that
    // worked; the stubs run either way).
    bool Seal();
    void Release();  // removes the function table too
    bool Contains(const unsigned char* address) const;
    bool Unwindable() const { return registered_; }
    const unsigned char* Base() const { return page_; }
    std::size_t Used() const { return used_; }

private:
    bool Describe(const unsigned char* code, std::size_t covered, const std::uint8_t* unwind, std::size_t unwindSize,
                  std::uint32_t& unwindAt);

    unsigned char* page_ = nullptr;
    std::size_t used_ = 0;
    std::uint32_t leafUnwind_ = 0;  // where the stubs' shared UNWIND_INFO is, 0 until the first stub
    std::vector<RUNTIME_FUNCTION> functions_;  // in address order, as the code is emitted
    bool registered_ = false;
};

// `call rel32` bytes at `site` aimed at `destination`; empty when out of reach.
std::vector<std::uint8_t> CallBytes(const unsigned char* site, const unsigned char* destination);
// `lea rax, [rip+disp32]` bytes at `site` loading `destination`; empty when out of reach.
// `jmp rel32` at `site` aimed at `destination`, NOP-padded to `length` (>= 5); empty when out of reach.
std::vector<std::uint8_t> JumpBytes(const unsigned char* site, const unsigned char* destination, std::size_t length);

}  // namespace multislot
