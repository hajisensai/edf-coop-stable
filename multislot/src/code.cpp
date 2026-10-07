#include "code.h"

#include <cstring>

namespace multislot {
namespace {
// Every stub, hook thunk (about 0x110 bytes each) and compare cave of a full install, with room to spare: 16 KiB ran
// out at the room-size hooks (2026-10-07, "compare cave ... out of reach"). One allocation granule.
constexpr std::size_t kPage = 0x10000;
constexpr std::size_t kStub = 12;
// A rel32 reaches +/-2 GB; stay well inside that on both sides of the anchor.
constexpr std::uintptr_t kReach = 0x60000000;
}  // namespace

bool WriteCode(unsigned char* at, const unsigned char* bytes, std::size_t size) {
    DWORD previous = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(at, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(at, size, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

bool ThunkPage::Allocate(const unsigned char* anchor) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t granularity = info.dwAllocationGranularity ? info.dwAllocationGranularity : 0x10000;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(anchor) & ~(granularity - 1);
    for (std::uintptr_t step = granularity; step < kReach && !page_; step += granularity) {
        if (base > step)
            page_ = static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(base - step), kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!page_)
            page_ = static_cast<unsigned char*>(VirtualAlloc(reinterpret_cast<void*>(base + step), kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    used_ = 0;
    return page_ != nullptr;
}

unsigned char* ThunkPage::Add(void* target) {
    unsigned char code[kStub] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(code + 2, &address, sizeof(address));
    unsigned char* stub = Emit(code, kStub);
    if (!stub) return nullptr;
    // One UNWIND_INFO with no codes serves every stub: version 1, no handler, no prolog, no frame register.
    constexpr std::uint8_t kLeaf[4] = {1, 0, 0, 0};
    if (!leafUnwind_) {
        std::uint32_t at = 0;
        if (!Describe(stub, kStub, kLeaf, sizeof(kLeaf), at)) return nullptr;
        leafUnwind_ = at;
        return stub;
    }
    functions_.push_back({static_cast<DWORD>(stub - page_), static_cast<DWORD>(stub - page_ + kStub), leafUnwind_});
    return stub;
}

unsigned char* ThunkPage::EmitFunction(const unsigned char* code, std::size_t size, std::size_t covered,
                                       const std::uint8_t* unwind, std::size_t unwindSize) {
    if (covered > size) return nullptr;
    unsigned char* at = Emit(code, size);
    std::uint32_t unwindAt = 0;
    return at && Describe(at, covered, unwind, unwindSize, unwindAt) ? at : nullptr;
}

// Copies `unwind` into the page, 4-byte aligned as the unwinder reads it, and records `code`'s first `covered`
// bytes as a function that unwinds by it.
bool ThunkPage::Describe(const unsigned char* code, std::size_t covered, const std::uint8_t* unwind,
                         std::size_t unwindSize, std::uint32_t& unwindAt) {
    const std::size_t padding = (4 - used_ % 4) % 4;
    if (padding + unwindSize > kPage - used_) return false;
    std::memset(page_ + used_, 0xCC, padding);
    used_ += padding;
    unwindAt = static_cast<std::uint32_t>(used_);
    Emit(unwind, unwindSize);
    functions_.push_back({static_cast<DWORD>(code - page_), static_cast<DWORD>(code - page_ + covered), unwindAt});
    return true;
}

unsigned char* ThunkPage::Emit(const unsigned char* code, std::size_t size) {
    if (!page_ || size > kPage - used_) return nullptr;
    unsigned char* at = page_ + used_;
    std::memcpy(at, code, size);
    used_ += size;
    return at;
}

bool ThunkPage::Seal() {
    DWORD previous = 0;
    if (!page_ || !VirtualProtect(page_, kPage, PAGE_EXECUTE_READ, &previous)) return false;
    FlushInstructionCache(GetCurrentProcess(), page_, kPage);
    registered_ = !functions_.empty() &&
                  RtlAddFunctionTable(functions_.data(), static_cast<DWORD>(functions_.size()),
                                      static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(page_)));
    return true;
}

void ThunkPage::Release() {
    // The table goes before the memory it describes.
    if (registered_) RtlDeleteFunctionTable(functions_.data());
    registered_ = false;
    functions_.clear();
    leafUnwind_ = 0;
    if (page_) VirtualFree(page_, 0, MEM_RELEASE);
    page_ = nullptr;
    used_ = 0;
}

bool ThunkPage::Contains(const unsigned char* address) const {
    return page_ && address >= page_ && address < page_ + kPage;
}

std::vector<std::uint8_t> JumpBytes(const unsigned char* site, const unsigned char* destination, std::size_t length) {
    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(destination) - reinterpret_cast<std::intptr_t>(site + 5));
    if (length < 5 || delta < INT32_MIN || delta > INT32_MAX) return {};
    const auto relative = static_cast<std::int32_t>(delta);
    std::vector<std::uint8_t> bytes(length, 0x90);
    bytes[0] = 0xE9;
    std::memcpy(bytes.data() + 1, &relative, sizeof(relative));
    return bytes;
}

std::vector<std::uint8_t> CallBytes(const unsigned char* site, const unsigned char* destination) {
    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(destination) - reinterpret_cast<std::intptr_t>(site + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) return {};
    const auto relative = static_cast<std::int32_t>(delta);
    std::vector<std::uint8_t> bytes(5);
    bytes[0] = 0xE8;
    std::memcpy(bytes.data() + 1, &relative, sizeof(relative));
    return bytes;
}

}  // namespace multislot
