#include "midhook.h"

#include <cstring>

namespace multislot {
namespace {

void Append(std::vector<std::uint8_t>& code, std::initializer_list<std::uint8_t> bytes) {
    code.insert(code.end(), bytes.begin(), bytes.end());
}

void Append64(std::vector<std::uint8_t>& code, std::uint64_t value) {
    const std::size_t at = code.size();
    code.resize(at + sizeof(value));
    std::memcpy(code.data() + at, &value, sizeof(value));
}

// push rax; mov rax, resume; xchg [rsp], rax: [rsp] = resume with every register as it was.
constexpr std::size_t kReturnAddressBytes = 1 + 10 + 4;
// The prolog after that: push rax, rcx, rdx, rbx, rbp, rsi, rdi (1 byte each), r8..r15 (2 each), pushfq,
// sub rsp, 0x60 (4) and mov rbx, rsp (3), which makes rbx the frame the unwinder starts from.
constexpr std::uint8_t kPushedRegisters[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr std::size_t kPrologBytes = kReturnAddressBytes + 7 + 16 + 1 + 4 + 3;
// Everything up to the pops and `lea rsp, [rsp+8]` that drops the pushed resume address.
constexpr std::size_t kFrameBytes = kPrologBytes + 4 + 15 + 10 + 3 + 4 + 4 + 10 + 2 + 3 + 4 + 15 + 10 + 4 + 1 + 16 + 7;

}  // namespace

std::vector<std::uint8_t> MidThunkCode(MidHandler handler, const std::uint8_t* displaced, std::size_t displacedSize,
                                       std::uint64_t resume) {
    std::vector<std::uint8_t> code;
    // push rax; mov rax, resume; xchg [rsp], rax
    Append(code, {0x50, 0x48, 0xB8});
    Append64(code, resume);
    Append(code, {0x48, 0x87, 0x04, 0x24});
    // push rax, rcx, rdx, rbx, rbp, rsi, rdi, r8..r15; pushfq
    Append(code, {0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57});
    Append(code, {0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
    Append(code, {0x9C});
    // sub rsp, 0x60; mov rbx, rsp (the frame, until rbx is popped); movups [rsp+0x10*i], xmm_i
    Append(code, {0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xDC});
    Append(code, {0x0F, 0x11, 0x04, 0x24});
    Append(code, {0x0F, 0x11, 0x4C, 0x24, 0x10, 0x0F, 0x11, 0x54, 0x24, 0x20, 0x0F, 0x11, 0x5C, 0x24, 0x30});
    Append(code, {0x0F, 0x11, 0x64, 0x24, 0x40, 0x0F, 0x11, 0x6C, 0x24, 0x50});
    // mov rcx, rsp (context); and rsp, -16; sub rsp, 0x20; mov rax, handler; call rax; mov rsp, rbx
    Append(code, {0x48, 0x8B, 0xCC, 0x48, 0x83, 0xE4, 0xF0, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xB8});
    Append64(code, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handler)));
    Append(code, {0xFF, 0xD0, 0x48, 0x8B, 0xE3});
    // movups xmm_i, [rsp+0x10*i]; add rsp, 0x60; popfq; pop r15..r8, rdi, rsi, rbp, rbx, rdx, rcx, rax
    Append(code, {0x0F, 0x10, 0x04, 0x24});
    Append(code, {0x0F, 0x10, 0x4C, 0x24, 0x10, 0x0F, 0x10, 0x54, 0x24, 0x20, 0x0F, 0x10, 0x5C, 0x24, 0x30});
    Append(code, {0x0F, 0x10, 0x64, 0x24, 0x40, 0x0F, 0x10, 0x6C, 0x24, 0x50});
    Append(code, {0x48, 0x83, 0xC4, 0x60, 0x9D});
    Append(code, {0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58});
    Append(code, {0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58});
    // lea rsp, [rsp+8]: the pushed resume address goes, the flags stay
    Append(code, {0x48, 0x8D, 0x64, 0x24, 0x08});
    if (displacedSize) code.insert(code.end(), displaced, displaced + displacedSize);
    // jmp qword ptr [rip+0]; resume
    Append(code, {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    Append64(code, resume);
    return code;
}

std::vector<std::uint8_t> MidThunkUnwind() {
    // UNWIND_INFO version 1, no handler; the frame register is rbx (3) at offset 0. The codes are listed last
    // prolog instruction first, each with the offset just past it.
    constexpr std::uint8_t kPushNonvol = 0, kAllocSmall = 2, kSetFrame = 3;
    std::vector<std::uint8_t> codes;
    const auto add = [&](std::size_t offset, std::uint8_t op, std::uint8_t info) {
        codes.insert(codes.begin(), {static_cast<std::uint8_t>(offset), static_cast<std::uint8_t>(op | (info << 4))});
    };
    std::size_t offset = kReturnAddressBytes;
    for (const std::uint8_t reg : kPushedRegisters) {
        offset += reg < 8 ? 1 : 2;
        add(offset, kPushNonvol, reg);
    }
    add(offset += 1, kAllocSmall, 0);            // pushfq: 8 bytes
    add(offset += 4, kAllocSmall, (0x60 - 8) / 8);  // sub rsp, 0x60
    add(offset += 3, kSetFrame, 0);              // mov rbx, rsp
    std::vector<std::uint8_t> info = {1, static_cast<std::uint8_t>(offset), static_cast<std::uint8_t>(codes.size() / 2), 3};
    info.insert(info.end(), codes.begin(), codes.end());
    if (codes.size() / 2 % 2) info.insert(info.end(), {0, 0});  // the code array has an even length
    return info;
}

std::size_t MidThunkFrameBytes() { return kFrameBytes; }

unsigned char* EmitMidThunk(ThunkPage& page, MidHandler handler, const std::uint8_t* displaced,
                            std::size_t displacedSize, std::uint64_t resume) {
    const auto code = MidThunkCode(handler, displaced, displacedSize, resume);
    const auto unwind = MidThunkUnwind();
    return page.EmitFunction(code.data(), code.size(), kFrameBytes, unwind.data(), unwind.size());
}

}  // namespace multislot
