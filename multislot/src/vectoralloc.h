#pragma once
#include <cstddef>

namespace multislot {

// A buffer the game's std::vector code will free. MSVC's std::allocator (_Allocate / _Deallocate) treats blocks of
// 4096 bytes or more differently from smaller ones: it asks operator new for 0x27 more bytes, aligns the buffer to 32
// and keeps the block's own address just below it; _Deallocate of such a size reads that address back and fails fast
// (FAST_FAIL_INVALID_ARG, "invalid parameter") when the buffer is not 8..0x27 bytes past it. Code that called
// operator new itself for a vector the game frees has to do the same once a patch makes the size that large.
using OperatorNew = void* (*)(std::size_t bytes);
inline constexpr std::size_t kBigAllocationThreshold = 0x1000;
inline constexpr std::size_t kBigAllocationAlignment = 32;
inline constexpr std::size_t kBigAllocationExtra = sizeof(void*) + kBigAllocationAlignment - 1;  // 0x27

// `bytes` from `allocate` as std::allocator would hand them out: small blocks as they come, large ones aligned with
// their block address at [buffer - 8].
void* AllocateVectorBlock(std::size_t bytes, OperatorNew allocate);

// The game's operator new (EDF+12D85B0), set by the plugin before it redirects a call to VectorOperatorNew.
void SetGameOperatorNew(OperatorNew allocate);
// In place of the game's `call operator new` for a vector buffer whose size a patch grew (patches.h SessionCalls).
void* VectorOperatorNew(std::size_t bytes);

}  // namespace multislot
