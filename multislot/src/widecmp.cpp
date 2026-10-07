#include "widecmp.h"

#include <cstring>

namespace multislot {
namespace {

void Append64(std::vector<std::uint8_t>& code, std::uint64_t value) {
    const std::size_t at = code.size();
    code.resize(at + sizeof(value));
    std::memcpy(code.data() + at, &value, sizeof(value));
}

// ModRM (with SIB and displacement) bytes from `at`, 0 when the operand is RIP-relative or runs past `end`.
std::size_t OperandBytes(const std::uint8_t* at, const std::uint8_t* end) {
    if (at >= end) return 0;
    const std::uint8_t modrm = at[0];
    const std::uint8_t mod = modrm >> 6, rm = modrm & 7;
    std::size_t size = 1;
    if (mod != 3 && rm == 4) {
        if (at + 1 >= end) return 0;
        const std::uint8_t base = at[1] & 7;
        size += 1;
        if (mod == 0 && base == 5) size += 4;  // [index*scale + disp32]
    } else if (mod == 0 && rm == 5) {
        return 0;  // [rip + disp32]
    }
    if (mod == 1) size += 1;
    if (mod == 2) size += 4;
    return at + size <= end ? size : 0;
}

}  // namespace

WideCompareShape DecodeWideCompare(const WideCompare& site) {
    WideCompareShape shape;
    const std::uint8_t* begin = site.original.data();
    const std::uint8_t* end = begin + site.original.size();
    if (site.compareSize < 3 || site.compareSize >= site.original.size()) return shape;
    const std::uint8_t* at = begin;
    if ((*at & 0xF0) == 0x40) {
        shape.prefix = 1;
        ++at;
    }
    if (at >= end || *at != 0x83) return shape;
    ++at;
    if (at >= end || ((*at >> 3) & 7) != 7) return shape;  // /7: cmp
    shape.operandBytes = OperandBytes(at, end);
    if (!shape.operandBytes) return shape;
    at += shape.operandBytes;
    if (static_cast<std::size_t>(at + 1 - begin) != site.compareSize) return shape;  // the imm8 ends the cmp
    at += 1;
    const std::size_t jump = static_cast<std::size_t>(end - at);
    if (jump == 2 && (at[0] & 0xF0) == 0x70) {
        shape.condition = at[0] & 0x0F;
        shape.displacement = static_cast<std::int8_t>(at[1]);
    } else if (jump == 6 && at[0] == 0x0F && (at[1] & 0xF0) == 0x80) {
        shape.condition = at[1] & 0x0F;
        std::int32_t relative = 0;
        std::memcpy(&relative, at + 2, sizeof(relative));
        shape.displacement = relative;
    } else {
        return shape;
    }
    // A bound the sign-extended imm32 would read as negative is not a bound.
    shape.valid = site.bound <= 0x7FFFFFFFu;
    return shape;
}

std::vector<std::uint8_t> WideCompareCode(const WideCompare& site, std::uint64_t siteAddress) {
    const WideCompareShape shape = DecodeWideCompare(site);
    if (!shape.valid) return {};
    const std::uint64_t resume = siteAddress + site.original.size();
    const std::uint64_t target = resume + static_cast<std::uint64_t>(shape.displacement);
    std::vector<std::uint8_t> code;
    // [REX] 81 /7 id: the same operand against the 32-bit bound.
    if (shape.prefix) code.push_back(site.original[0]);
    code.push_back(0x81);
    const std::uint8_t* operand = site.original.data() + shape.prefix + 1;
    code.insert(code.end(), operand, operand + shape.operandBytes);
    const std::size_t at = code.size();
    code.resize(at + 4);
    std::memcpy(code.data() + at, &site.bound, sizeof(site.bound));
    // j!cc over the taken exit; the taken exit; the fall-through exit. Each exit is `jmp [rip+0]` and its address.
    constexpr std::uint8_t kExitBytes = 6 + 8;
    code.push_back(static_cast<std::uint8_t>(0x70 | (shape.condition ^ 1)));
    code.push_back(kExitBytes);
    code.insert(code.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    Append64(code, target);
    code.insert(code.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    Append64(code, resume);
    return code;
}

unsigned char* EmitWideCompare(ThunkPage& page, const WideCompare& site, const unsigned char* at) {
    const auto code = WideCompareCode(site, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at)));
    return code.empty() ? nullptr : page.Emit(code.data(), code.size());
}

}  // namespace multislot
