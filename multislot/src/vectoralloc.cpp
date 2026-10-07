#include "vectoralloc.h"

#include <atomic>
#include <bit>
#include <cstdint>

namespace multislot {
namespace {

std::atomic<std::uintptr_t>& GameAllocatorAddress() {
    static std::atomic<std::uintptr_t> address{0};
    return address;
}

}  // namespace

void SetGameOperatorNew(std::uintptr_t address) { GameAllocatorAddress().store(address); }

std::byte* VectorOperatorNew(std::size_t bytes) {
    return AllocateVectorBlock(bytes, std::bit_cast<OperatorNew>(GameAllocatorAddress().load()));
}

}  // namespace multislot
