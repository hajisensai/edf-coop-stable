#include "vectoralloc.h"

#include <atomic>
#include <cstdint>

namespace multislot {
namespace {

std::atomic<OperatorNew> gameOperatorNew{nullptr};

}  // namespace

void* AllocateVectorBlock(std::size_t bytes, OperatorNew allocate) {
    if (bytes < kBigAllocationThreshold) return allocate(bytes);
    const auto block = reinterpret_cast<std::uintptr_t>(allocate(bytes + kBigAllocationExtra));
    if (!block) return nullptr;
    const std::uintptr_t buffer = (block + kBigAllocationExtra) & ~(std::uintptr_t{kBigAllocationAlignment} - 1);
    reinterpret_cast<std::uintptr_t*>(buffer)[-1] = block;
    return reinterpret_cast<void*>(buffer);
}

void SetGameOperatorNew(OperatorNew allocate) { gameOperatorNew.store(allocate); }

void* VectorOperatorNew(std::size_t bytes) { return AllocateVectorBlock(bytes, gameOperatorNew.load()); }

}  // namespace multislot
