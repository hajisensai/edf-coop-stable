#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

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
template <typename Allocate>
std::byte* AllocateVectorBlock(std::size_t bytes, Allocate&& allocate) {
    if (bytes < kBigAllocationThreshold) return static_cast<std::byte*>(allocate(bytes));
    if (bytes > std::numeric_limits<std::size_t>::max() - kBigAllocationExtra) return nullptr;
    auto* block = static_cast<std::byte*>(allocate(bytes + kBigAllocationExtra));
    if (!block) return nullptr;
    // Place padding, then the backlink, then the aligned payload in the original allocation. Derive padding
    // from the address remainder so no address addition can overflow and all writes use non-negative offsets.
    const auto remainder = std::bit_cast<std::uintptr_t>(block) % kBigAllocationAlignment;
    const auto padding = (kBigAllocationAlignment - (remainder + sizeof(block)) % kBigAllocationAlignment) %
                         kBigAllocationAlignment;
    std::memcpy(block + padding, &block, sizeof(block));
    return block + padding + sizeof(block);
}

// The game's operator new (EDF+12D85B0), set by the plugin before it redirects a call to VectorOperatorNew.
void SetGameOperatorNew(std::uintptr_t address);
// In place of the game's `call operator new` for a vector buffer whose size a patch grew (patches.h SessionCalls).
std::byte* VectorOperatorNew(std::size_t bytes);

}  // namespace multislot
