// Runs widened compares in this process: a `cmp r/m, imm8; jcc` pair is patched to jump to its cave, and the patched
// function must take the same exits it did, with the bound the cave compares against instead of the imm8.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/code.h"
#include "../src/patches.h"
#include "../src/widecmp.h"

using namespace multislot;

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const char* what, long long value = 0) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s (%lld)\n", what, value);
    }
}

struct Buffer {
    unsigned char* code = nullptr;
    Buffer() { code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)); }
    ~Buffer() {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
    }
};

using Function = std::uint64_t (*)(std::uint64_t);

// Writes `body` at the buffer, patches the site at offset 0 (the first `site.original.size()` bytes of body) with a
// jump to its cave, and returns the function. False when anything did not work.
bool Build(Buffer& buffer, ThunkPage& page, const std::vector<std::uint8_t>& body, const WideCompare& site) {
    if (!buffer.code || !page.Allocate(buffer.code)) return false;
    std::memcpy(buffer.code, body.data(), body.size());
    const unsigned char* cave = EmitWideCompare(page, site, buffer.code);
    if (!cave) return false;
    const auto jump = JumpBytes(buffer.code, cave, site.original.size());
    if (jump.empty() || !page.Seal()) return false;
    std::memcpy(buffer.code, jump.data(), jump.size());
    FlushInstructionCache(GetCurrentProcess(), buffer.code, 0x1000);
    return true;
}

// f(rcx) = 1 when the branch of `cmp rcx, 4; jae` is taken, else 0; the bound becomes `bound`.
void ShortJumpOnRegister(std::uint32_t bound) {
    const std::vector<std::uint8_t> body = {
        0x48, 0x83, 0xF9, 0x04,        // cmp rcx, 4
        0x73, 0x06,                    // jae +6
        0x31, 0xC0,                    // xor eax, eax
        0xC3,                          // ret
        0x90, 0x90, 0x90,              //
        0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax, 1
        0xC3,                          // ret
    };
    const WideCompare site{"test cmp rcx", 0, {body.begin(), body.begin() + 6}, 4, bound};
    Buffer buffer;
    ThunkPage page;
    Check(Build(buffer, page, body, site), "the register compare site is patched");
    const auto f = reinterpret_cast<Function>(buffer.code);
    for (const std::uint64_t value : {0ull, 3ull, 4ull, 31ull, 32ull, 127ull, 128ull, 1023ull, 1024ull, 5000ull,
                                      static_cast<std::uint64_t>(bound) - 1, static_cast<std::uint64_t>(bound)}) {
        const std::uint64_t expected = value >= bound ? 1 : 0;
        Check(f(value) == expected, "jae against the wide bound", static_cast<long long>(value));
    }
    page.Release();
}

// `cmp dword [rcx+0x10], 4; jne rel32` (0F 85): f(p) = 1 while [p+0x10] != bound.
void NearJumpOnMemory(std::uint32_t bound) {
    const std::vector<std::uint8_t> body = {
        0x83, 0x79, 0x10, 0x04,              // cmp dword [rcx+0x10], 4
        0x0F, 0x85, 0x03, 0x00, 0x00, 0x00,  // jne +3
        0x31, 0xC0,                          // xor eax, eax
        0xC3,                                // ret
        0xB8, 0x01, 0x00, 0x00, 0x00,        // mov eax, 1
        0xC3,                                // ret
    };
    const WideCompare site{"test cmp mem", 0, {body.begin(), body.begin() + 10}, 4, bound};
    Buffer buffer;
    ThunkPage page;
    Check(Build(buffer, page, body, site), "the memory compare site is patched");
    const auto f = reinterpret_cast<Function>(buffer.code);
    std::uint32_t record[8]{};
    for (const std::uint32_t value : {0u, 4u, bound - 1, bound, bound + 1}) {
        record[4] = value;
        Check(f(reinterpret_cast<std::uintptr_t>(record)) == (value != bound ? 1u : 0u), "jne against the wide bound", value);
    }
    page.Release();
}

// `cmp ebx, 4; jb` with a signed-looking count: the unsigned exits of the game's loop (78FFAC) stay unsigned.
void ShortJumpBelow(std::uint32_t bound) {
    const std::vector<std::uint8_t> code = {
        0x83, 0xF9, 0x04,              // cmp ecx, 4
        0x72, 0x03,                    // jb +3
        0x31, 0xC0,                    // xor eax, eax
        0xC3,                          // ret
        0xB8, 0x01, 0x00, 0x00, 0x00,  // mov eax, 1
        0xC3,                          // ret
    };
    const WideCompare site{"test cmp ecx", 0, {code.begin(), code.begin() + 5}, 3, bound};
    Buffer buffer;
    ThunkPage page;
    Check(Build(buffer, page, code, site), "the 32-bit compare site is patched");
    const auto f = reinterpret_cast<Function>(buffer.code);
    for (const std::uint64_t value : {0ull, 4ull, 1023ull, 1024ull, 0xFFFFFFFFull, 0x1FFFFFFFFull})
        Check(f(value) == ((value & 0xFFFFFFFF) < bound ? 1u : 0u), "jb on the low 32 bits", static_cast<long long>(value));
    page.Release();
}

void Shapes() {
    // RIP-relative operands, two-byte compares, a missing jcc and an oversized bound are refused.
    Check(!DecodeWideCompare({"rip", 0, {0x83, 0x3D, 0, 0, 0, 0, 0x04, 0x73, 0x00}, 7, 1024}).valid, "rip-relative refused");
    Check(!DecodeWideCompare({"no jcc", 0, {0x83, 0xF9, 0x04, 0x90, 0x90}, 3, 1024}).valid, "no jcc refused");
    Check(!DecodeWideCompare({"add", 0, {0x83, 0xC1, 0x04, 0x73, 0x00}, 3, 1024}).valid, "add refused");
    Check(!DecodeWideCompare({"bound", 0, {0x83, 0xF9, 0x04, 0x73, 0x00}, 3, 0x80000000u}).valid, "negative bound refused");
    Check(!DecodeWideCompare({"size", 0, {0x83, 0xF9, 0x04, 0x73, 0x00}, 4, 1024}).valid, "wrong compare size refused");
    // Every widened site of the plugin's tables decodes (patches.h).
    std::vector<WideCompare> sites = SessionCompares();
    const auto mission = MissionCompares();
    sites.insert(sites.end(), mission.begin(), mission.end());
    Check(sites.size() == 14, "widened sites: four of the room tables, ten of the mission phase");
    for (const auto& site : sites) {
        Check(DecodeWideCompare(site).valid, site.name, site.rva);
        Check(site.bound == static_cast<std::uint32_t>(kMaxPlayers), "widened to the room size", site.rva);
        Check(site.original[site.compareSize - 1] == kVanillaPlayers, "the game's bound was four", site.rva);
    }
    // The cave of a site is position-independent: two addresses give the same compare and different exits only.
    const WideCompare site{"pi", 0, {0x48, 0x83, 0xF9, 0x04, 0x73, 0x10}, 4, 1024};
    const auto a = WideCompareCode(site, 0x1000), b = WideCompareCode(site, 0x7FF600001000ull);
    Check(a.size() == b.size() && a.size() == 7 + 2 + 14 + 14, "cave size");
    Check(std::memcmp(a.data(), b.data(), 9) == 0, "the compare does not depend on the address");
    std::uint64_t target = 0, resume = 0;
    std::memcpy(&target, b.data() + 9 + 6, 8);
    std::memcpy(&resume, b.data() + 9 + 14 + 6, 8);
    Check(target == 0x7FF600001000ull + 6 + 0x10 && resume == 0x7FF600001000ull + 6, "exits of the jcc");
}

}  // namespace

int main() {
    ShortJumpOnRegister(static_cast<std::uint32_t>(kMaxPlayers));
    ShortJumpOnRegister(4);  // the game's own bound: unchanged behaviour
    NearJumpOnMemory(static_cast<std::uint32_t>(kMaxPlayers));
    ShortJumpBelow(static_cast<std::uint32_t>(kMaxPlayers));
    Shapes();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
