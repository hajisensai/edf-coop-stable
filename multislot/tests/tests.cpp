// Verifies the patch tables against the EDF.dll on disk: every original byte must be present, no
// two writes may overlap, and the SEARCH_TYPE and capacity values must follow the room rules.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <bit>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../src/hud.h"
#include "../src/hudcolours.h"
#include "../src/mission.h"
#include "../src/patches.h"
#include "../src/smoothing.h"
#include "../src/rooms.h"
#include "../src/userslots.h"
#include "../src/vectoralloc.h"
#include "../src/joinlog.h"
#include "../src/peertimeout.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const char* what, std::uint32_t rva = 0) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s (EDF+%X)\n", what, rva);
    }
}

struct Image {
    std::vector<std::uint8_t> file;
    const IMAGE_NT_HEADERS64* nt = nullptr;

    const std::uint8_t* At(std::uint32_t rva, std::size_t size) const {
        auto section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if (rva >= section->VirtualAddress && rva + size <= section->VirtualAddress + section->SizeOfRawData) {
                const std::size_t offset = section->PointerToRawData + (rva - section->VirtualAddress);
                return offset + size <= file.size() ? file.data() + offset : nullptr;
            }
        }
        return nullptr;
    }
};

std::uint64_t Operand(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t size) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, size);
    return value;
}

using Spans = std::set<std::pair<std::uint32_t, std::uint32_t>>;

Spans WriteSpans(const std::vector<Patch>& patches, const std::vector<CallSite>& calls, const std::vector<MidSite>& hooks) {
    Spans spans;
    for (const auto& patch : patches) spans.insert({patch.rva, patch.rva + static_cast<std::uint32_t>(patch.original.size())});
    for (const auto& call : calls) spans.insert({call.rva, call.rva + 5});
    for (const auto& hook : hooks) spans.insert({hook.rva, hook.rva + static_cast<std::uint32_t>(hook.original.size())});
    const PointerSlot slot = RoomViewSlot();
    spans.insert({slot.rva, slot.rva + 8});
    const PointerSlot frame = MainFrameSlot();
    spans.insert({frame.rva, frame.rva + 8});
    for (const auto& missionSlot : MissionSlots()) spans.insert({missionSlot.rva, missionSlot.rva + 8});
    return spans;
}

void CheckNoOverlap(const Spans& spans) {
    std::uint32_t lastEnd = 0;
    for (const auto& span : spans) {
        Check(span.first >= lastEnd, "writes do not overlap", span.first);
        lastEnd = span.second;
    }
}

// Addresses EDF6VR patches (Mods/Plugins/EDF6VR.log, read-only). Each is assumed to cover 8 bytes.
// Every byte EDF6VR changes in EDF.dll, from Mods/Plugins/EDF6VR.patches.txt (read-only; refreshed
// 2026-09-19 after the VR mod was updated). MultiSlot may not write inside any of them.
struct VrSite {
    std::uint32_t rva;
    std::uint32_t size;
};
constexpr VrSite kVrSites[] = {
    {0x2BA9C0, 5}, {0x2BAD96, 5}, {0x2BAF60, 5}, {0x2BFC02, 5},
    {0x2BFC89, 5}, {0x2C0D32, 5}, {0x2C0DB9, 5}, {0x572FFB, 5},
    {0x681A7F, 5}, {0x683EA0, 5}, {0x684271, 5}, {0x685985, 5},
    {0x6888D9, 5}, {0x688FFD, 5}, {0x6891A5, 5}, {0x6904F7, 5},
    {0x690603, 5}, {0x690B3F, 5}, {0x690D54, 5}, {0x691B10, 5},
    {0x692A24, 5}, {0x69366A, 5}, {0x694894, 5}, {0x6A3D0E, 5},
    {0x6A3DFB, 5}, {0x6AEBB0, 5}, {0x6C06F2, 5}, {0x70573B, 5},
    {0x7ACEE0, 5}, {0x7ACF98, 5}, {0x7AEE60, 5}, {0x80200A, 42},
    {0x805657, 4}, {0x80565F, 4}, {0x805EFE, 4}, {0x805F09, 4},
    {0x806123, 4}, {0x80612E, 4}, {0x82B6DB, 5}, {0x100B07B, 5},
    {0x106C0B6, 5}, {0x10FF556, 6}, {0x10FF5C1, 6}, {0x1131198, 7},
    {0x1131233, 7}, {0x1183250, 16}, {0x1183260, 43}, {0x1183290, 16},
    {0x11832A0, 17}, {0x1183310, 17}, {0x1197A55, 5}, {0x1197F31, 5},
    {0x1197FBD, 5}, {0x119889B, 5}, {0x11A2F65, 6}, {0x11A2F74, 2},
    {0x1755578, 8}, {0x1756728, 8}, {0x1756868, 8}, {0x1756870, 8},
    {0x1756898, 8}, {0x17568A8, 8}, {0x17568E0, 8}, {0x17568E8, 8},
    {0x1768C28, 8}, {0x1768C30, 8}, {0x1768C58, 8}, {0x17A6D70, 8},
    {0x17C4038, 8}, {0x17E1CD8, 8}, {0x17E1F28, 8}, {0x17E2158, 8},
    {0x17E2390, 8}, {0x17E2468, 8}, {0x17E2470, 8}, {0x17E24A8, 8},
    {0x17E24B0, 8}, {0x17E26C0, 8}, {0x17E3350, 8}, {0x17E3400, 8},
    {0x17E35B8, 8}, {0x17E3668, 8}, {0x17E3708, 8}, {0x17E37B8, 8},
    {0x17E3858, 8}, {0x17E3908, 8}, {0x17E3B18, 8}, {0x17E3BC8, 8},
    {0x17E3D78, 8}, {0x17E3E28, 8}, {0x17E3F40, 8}, {0x17E3FF0, 8},
    {0x17E40C8, 8}, {0x17E4178, 8}, {0x17E42C8, 8}, {0x17E4378, 8},
    {0x17E4610, 8}, {0x17E46C0, 8}, {0x17E4778, 8}, {0x17E4828, 8},
    {0x17E4930, 8}, {0x17E49E0, 8}, {0x17E4AA8, 8}, {0x17E4B58, 8},
    {0x17E4DF0, 8}, {0x17E4EA0, 8}, {0x17E4FD8, 8}, {0x17E5088, 8},
    {0x17E51C8, 8}, {0x17E5278, 8}, {0x17E5468, 8}, {0x17E5518, 8},
    {0x17E5800, 8}, {0x17E5978, 8}, {0x17E5A28, 8}, {0x17E5B88, 8},
    {0x17E5C38, 8}, {0x17E5E68, 8}, {0x17E5F18, 8}, {0x17E5FD0, 8},
    {0x17E6080, 8}, {0x17E6148, 8}, {0x17E61F8, 8}, {0x17E6348, 8},
    {0x17E63F8, 8}, {0x17F6C20, 8}, {0x17F6CB0, 8}, {0x17F6E20, 8},
    {0x1AE5290, 8},
    // Sites EDF6VR reported in earlier builds but not in the current list: kept so MultiSlot stays clear of
    // anything the VR mod has ever touched. 16 bytes each, which is more than any of them was.
    {0x18428, 16}, {0x2CBDF0, 16}, {0x56D709, 16}, {0x56DB5B, 16},
    {0x570650, 16}, {0x576E60, 16}, {0x692100, 16}, {0x705B10, 16},
    {0x11001F0, 16}, {0x11883C2, 16}, {0x11883D7, 16}, {0x1194280, 16},
    {0x1197A3A, 16}, {0x2136FF0, 16},
};

void CheckClearOfVr(const Spans& spans) {
    for (const auto& span : spans)
        for (const VrSite& vr : kVrSites)
            Check(span.second <= vr.rva || span.first >= vr.rva + vr.size, "write stays clear of EDF6VR's patches", span.first);
}

// What 961930 (and every std::vector of the game) checks before it frees a buffer of `bytes`: the address it passes to
// operator delete, or 0 where it fails fast.
std::uintptr_t GameFreeAddress(std::span<const std::byte> allocation, std::size_t offset, std::size_t bytes) {
    if (offset > allocation.size() || bytes > allocation.size() - offset) return 0;
    const auto at = std::bit_cast<std::uintptr_t>(allocation.data() + offset);
    if (bytes < 0x1000) return at;
    std::uintptr_t block = 0;
    if (offset < sizeof(block)) return 0;
    std::memcpy(&block, allocation.data() + offset - sizeof(block), sizeof(block));
    return at - block - 8 > 0x1F ? 0 : block;
}

void CheckVectorBlocks() {
    // Storage ownership stays in the fixture while the callback records the allocator's request.
    std::vector<std::byte> storage;
    std::size_t requested = 0;
    const auto recording = [&requested, &storage](std::size_t bytes) {
        requested = bytes;
        storage.resize(bytes);
        return storage.data();
    };
    const auto* small = AllocateVectorBlock(0x140, recording);
    Check(small && requested == 0x140 && GameFreeAddress(storage, 0, 0x140) == std::bit_cast<std::uintptr_t>(small),
          "a small vector block is operator new's own");
    // Cover the exact alignment threshold as well as actual HUD record capacities.
    for (std::size_t bytes : {std::size_t{0x1000}, std::size_t{0x50 * 52}, std::size_t{0x50 * 64},
                              std::size_t{0x50 * kMaxPlayers}}) {
        const auto* buffer = AllocateVectorBlock(bytes, recording);
        const auto offset = static_cast<std::size_t>(buffer - storage.data());
        const auto block = GameFreeAddress(storage, offset, bytes);
        const auto at = std::bit_cast<std::uintptr_t>(buffer);
        Check(buffer && requested == bytes + kBigAllocationExtra &&
                  block == std::bit_cast<std::uintptr_t>(storage.data()) && at % kBigAllocationAlignment == 0 &&
                  at + bytes <= block + requested,
              "a large vector block frees as the game's std::vector frees it");
    }
    requested = 0;
    Check(!AllocateVectorBlock(std::numeric_limits<std::size_t>::max(), recording) && requested == 0,
          "an overflowing request never reaches the backing allocator");
    Check(!AllocateVectorBlock(0x1000, [](std::size_t) -> std::byte* { return nullptr; }),
          "a failed backing allocation propagates without writing a header");
    // Exercise every possible base-address remainder, including the longest 39-byte prefix. The fixture's
    // explicit allocation extent lets the backlink reader verify the same bounds without looking before it.
    constexpr std::size_t payload = 0x1000;
    constexpr std::size_t allocated = payload + kBigAllocationExtra;
    alignas(32) std::array<std::byte, allocated + kBigAllocationAlignment> shifted{};
    for (std::size_t shift = 0; shift < kBigAllocationAlignment; ++shift) {
        auto* base = shifted.data() + shift;
        const auto* buffer = AllocateVectorBlock(payload, [base](std::size_t bytes) {
            Check(bytes == allocated, "every alignment reserves the full payload and prefix");
            return base;
        });
        const auto offset = static_cast<std::size_t>(buffer - base);
        Check(offset >= sizeof(base) && offset <= kBigAllocationExtra &&
                  std::bit_cast<std::uintptr_t>(buffer) % kBigAllocationAlignment == 0 &&
                  GameFreeAddress({base, allocated}, offset, payload) == std::bit_cast<std::uintptr_t>(base),
              "all 32 allocation alignments preserve the native backlink and payload extent");
    }
    // The 2.4.1 crash: an ordinary grown block without a backlink is rejected.
    const std::size_t grown = 0x50 * kMaxPlayers;
    const std::vector<std::byte> plain(grown + 16);
    Check(GameFreeAddress(plain, 16, grown) == 0,
          "a plain operator new block of the grown size fails 961930's check");
    Check(GameFreeAddress(plain, 0, grown) == 0 && GameFreeAddress(plain, plain.size() + 1, grown) == 0 &&
              GameFreeAddress(plain, 16, plain.size()) == 0,
          "backlink inspection rejects missing prefix and out-of-range payloads");
}

}  // namespace

int main(int argc, char** argv) {
    // Needs no game: also runs when the tests against EDF.dll are skipped.
    CheckVectorBlocks();
    if (argc < 2) {
        std::printf("usage: MultiSlotTests EDF.dll\n");
        return 2;
    }
    // CMake registers this test with or without the game; without it the test says so and counts as skipped.
    if (GetFileAttributesA(argv[1]) == INVALID_FILE_ATTRIBUTES) {
        std::printf("SKIPPED: %s is not there (set EDF6_GAME_DIR to the game folder to run this test)\n", argv[1]);
        return failures ? 1 : 77;
    }
    Image image;
    std::ifstream in(argv[1], std::ios::binary);
    image.file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (image.file.size() < 0x1000) {
        std::printf("FAIL: cannot read %s\n", argv[1]);
        return 1;
    }
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.file.data());
    image.nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.file.data() + dos->e_lfanew);
    Check(image.nt->FileHeader.TimeDateStamp == kImageTimeDateStamp, "EDF.dll TimeDateStamp is the supported build");
    Check(image.nt->OptionalHeader.SizeOfImage == kImageSize, "EDF.dll SizeOfImage is the supported build");

    const auto guest = GuestPatches();
    auto calls = GuestCalls();
    Check(calls.size() == 2, "two member count calls are redirected");
    const auto roomCalls = RoomViewCalls();
    Check(roomCalls.size() == 3, "two panel builder calls and the voice icon call are redirected");
    calls.insert(calls.end(), roomCalls.begin(), roomCalls.end());
    // Every call to the member list builder: the list is cut to kMaxPlayers, and fake members reach all of its consumers.
    const auto memberListCalls = MemberListCalls();
    Check(memberListCalls.size() == 3, "all three member list calls are redirected");
    for (const auto& call : memberListCalls) Check(call.target == 0x7468C0, "the member list builder is the target", call.rva);
    calls.insert(calls.end(), memberListCalls.begin(), memberListCalls.end());
    // The voice chat HUD's record allocation goes to VectorOperatorNew (vectoralloc.h).
    const auto sessionCalls = SessionCalls();
    Check(sessionCalls.size() == 1 && sessionCalls[0].rva == 0x9606AE && sessionCalls[0].target == 0x12D85B0,
          "the voice chat HUD's operator new call is redirected", 0x9606AE);
    calls.insert(calls.end(), sessionCalls.begin(), sessionCalls.end());
    // The helpers the harness uses to grow that list: reserve (748E70) and append-copies (749040), as the
    // builder itself uses them.
    Check(CallTargets(image.At(0x746A30, 5), 0x746A30, 0x748E70) && CallTargets(image.At(0x746B49, 5), 0x746B49, 0x748E70),
          "the member list builder reserves through 748E70", 0x746A30);
    Check(CallTargets(image.At(0x748F6E, 5), 0x748F6E, 0x749040), "reserve clears the old buffer through 749040", 0x748F6E);
    const std::uint8_t reserveHead[] = {0x40, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0xEA, 0x48, 0x8B,
                                        0xF9, 0x48, 0x3B, 0x51, 0x10};  // cmp rdx, [rcx+0x10]: capacity
    const std::uint8_t resizeHead[] = {0x48, 0x89, 0x74, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20,
                                       0x48, 0x8B, 0x41, 0x18};   // mov rax, [rcx+0x18]: size
    Check(std::memcmp(image.At(0x748E70, sizeof(reserveHead)), reserveHead, sizeof(reserveHead)) == 0,
          "748E70 takes the list and a capacity", 0x748E70);
    Check(std::memcmp(image.At(0x749040, sizeof(resizeHead)), resizeHead, sizeof(resizeHead)) == 0,
          "749040 takes the list, a size and an entry", 0x749040);
    for (const auto& call : calls) Check(CallTargets(image.At(call.rva, 5), call.rva, call.target), call.name, call.rva);
    const PointerSlot slot = RoomViewSlot();
    Check(SlotTargets(image.At(slot.rva, 8), image.nt->OptionalHeader.ImageBase, slot.target), slot.name, slot.rva);
    const PointerSlot frameSlot = MainFrameSlot();
    Check(SlotTargets(image.At(frameSlot.rva, 8), image.nt->OptionalHeader.ImageBase, frameSlot.target), frameSlot.name, frameSlot.rva);

    // Constants roomview.cpp relies on, checked where the game itself uses them.
    const auto RipTarget = [&](std::uint32_t rva, std::size_t length) {
        const auto at = image.At(rva, length);
        std::int32_t disp = 0;
        std::memcpy(&disp, at + length - 4, 4);
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(rva) + static_cast<std::int64_t>(length) + disp);
    };
    Check(RipTarget(0x901ADB, 7) == 0x180AC00, "member creation stores the MemberInfo vtable 180AC00", 0x901ADB);
    // The remote-player position correction (smoothing.h). SoldierBase's per-frame sync steps its
    // correction vector toward the received one by a fixed fraction; the fraction is a shared 0.05, so
    // only this one instruction's operand may be retargeted. If a game update moves either, the factor
    // must not be written at all.
    const std::uint8_t smoothing[] = {0x0F, 0x59, 0x35, 0xE3, 0xF6, 0x20, 0x01};  // mulps xmm6, [rip+...]
    Check(std::memcmp(image.At(kSmoothingSite, sizeof(smoothing)), smoothing, sizeof(smoothing)) == 0 &&
              RipTarget(kSmoothingSite, sizeof(smoothing)) == 0x17A5A70,
          "the remote player correction still multiplies by the constant at 17A5A70", kSmoothingSite);
    const std::uint8_t splat[16] = {0xCD, 0xCC, 0x4C, 0x3D, 0xCD, 0xCC, 0x4C, 0x3D,
                                    0xCD, 0xCC, 0x4C, 0x3D, 0xCD, 0xCC, 0x4C, 0x3D};
    Check(std::memcmp(image.At(0x17A5A70, sizeof(splat)), splat, sizeof(splat)) == 0,
          "and that constant is still 0.05 four ways", 0x17A5A70);
    // The neighbours the site is identified by: the read before it and the write after.
    const std::uint8_t around[] = {0x0F, 0x10, 0x87, 0x20, 0x19, 0x00, 0x00};  // movups xmm0, [rdi+0x1920]
    Check(std::memcmp(image.At(0x59637C, sizeof(around)), around, sizeof(around)) == 0,
          "the correction vector is still read from player+0x1920", 0x59637C);
    // How long a correction takes to shrink to a tenth, at 90 ms a step.
    Check(SmoothingSettleMs(kVanillaSmoothing) > 3800 && SmoothingSettleMs(kVanillaSmoothing) < 4200,
          "the stock 0.05 needs about four seconds", 0);
    Check(SmoothingSettleMs(0.5f) > 250 && SmoothingSettleMs(0.5f) < 350, "half closes it in about 0.3 s", 0);
    Check(SmoothingSettleMs(0.0f) == 0 && SmoothingSettleMs(-1.0f) == 0 && SmoothingSettleMs(2.0f) == 0,
          "a factor outside (0, 1] has no settle time", 0);
    // MemberInfo's name, which roomview.cpp logs as the room roster. Creation (the shared_ptr block starts
    // 0x10 before the object) clears the class-ok byte at +0x08, writes the class at +0x0C and then builds
    // the name in place at +0x10; 8F1E80 is that constructor.
    const std::uint8_t memberHead[] = {0xC6, 0x43, 0x18, 0x00,              // mov byte [rbx+0x18], 0   -> +0x08
                                       0x44, 0x89, 0x73, 0x1C,              // mov [rbx+0x1C], r14d     -> +0x0C
                                       0x48, 0x8D, 0x4B, 0x20};             // lea rcx, [rbx+0x20]      -> +0x10
    Check(std::memcmp(image.At(0x901AE6, sizeof(memberHead)), memberHead, sizeof(memberHead)) == 0 &&
              RipTarget(0x901AF2, 5) == 0x8F1E80,
          "MemberInfo keeps the name at +0x10, built by 8F1E80", 0x901AE6);
    const std::uint8_t nameCapacity[] = {0x48, 0xC7, 0x41, 0x20, 0x07, 0x00, 0x00, 0x00};  // [rcx+0x20] = 7
    const std::uint8_t nameEmpty[] = {0x48, 0x89, 0x73, 0x10,   // [rcx+0x18] = 0     the size
                                      0x66, 0x89, 0x33,         // mov word [rcx+8], si  a wchar_t terminator
                                      0x40, 0x88, 0x31};        // mov byte [rcx], sil   the name-valid byte
    Check(std::memcmp(image.At(0x8F1EA0, sizeof(nameCapacity)), nameCapacity, sizeof(nameCapacity)) == 0 &&
              std::memcmp(image.At(0x8F1EB4, sizeof(nameEmpty)), nameEmpty, sizeof(nameEmpty)) == 0,
          "the name is a std::wstring at +0x18 with SSO capacity 7, behind a valid byte at +0x10", 0x8F1EA0);
    Check(RipTarget(0x1188315, 7) == 0x2136530, "pad poll uses the input slots at 2136530", 0x1188315);
    const std::uint8_t rsClick[] = {0x4A, 0x8B, 0x8C, 0x2B, 0xB8, 0x00, 0x00, 0x00};  // mov rcx, [rbx+r13+0xB8]
    Check(std::memcmp(image.At(0x1188C35, sizeof(rsClick)), rsClick, sizeof(rsClick)) == 0,
          "right stick click is the channel at slot+0xB8", 0x1188C35);
    const std::uint8_t pressed[] = {0xF3, 0x0F, 0x11, 0x49, 0x48, 0x44, 0x88, 0x41, 0x4C};  // value +0x48, pressed +0x4C
    Check(std::memcmp(image.At(0x1182A00, sizeof(pressed)), pressed, sizeof(pressed)) == 0,
          "input channel keeps the pressed state at +0x4C", 0x1182A00);
    const std::uint8_t templates[] = {0x48, 0x8D, 0x05};  // lea rax, [Button_Master] in the panel builder
    Check(std::memcmp(image.At(0x8F805B, 3), templates, 3) == 0 && RipTarget(0x8F805B, 7) == 0x180B428,
          "panel builder's template table starts with Button_Master", 0x8F805B);

    for (const auto& patch : guest) {
        Check(Matches(image.At(patch.rva, patch.original.size()), patch), patch.name, patch.rva);
        Check(patch.original.size() == patch.replacement.size(), "replacement has the original size", patch.rva);
    }

    // Room tables: each patch turns one operand 4 into 8 (or 4 records into 8 records) in the three constructors that
    // size a table per room member.
    const auto sessions = SessionPatches();
    Check(sessions.size() == 7, "session patch table size");
    for (const auto& patch : sessions) {
        Check(Matches(image.At(patch.rva, patch.original.size()), patch), patch.name, patch.rva);
        Check(patch.original.size() == patch.replacement.size() && patch.original != patch.replacement, "session patch changes bytes in place", patch.rva);
        if (patch.rva == 0x9606A9) {
            Check(Operand(patch.original, 1, 4) == 0x50 * kVanillaPlayers && Operand(patch.replacement, 1, 4) == 0x50 * kMaxPlayers,
                  "the voice chat HUD allocates eight 0x50-byte records", patch.rva);
            continue;
        }
        // Every other one is an imm32 that ends the instruction: 4 becomes the room size, the opcode stays.
        const std::size_t at = patch.original.size() - 4;
        Check(Operand(patch.original, at, 4) == kVanillaPlayers && Operand(patch.replacement, at, 4) == kMaxPlayers &&
                  std::equal(patch.original.begin(), patch.original.begin() + at, patch.replacement.begin()),
              "session patch changes an imm32 4 into the room size and nothing else", patch.rva);
        Check((patch.rva > 0x12B77E0 && patch.rva < 0x12B79A3) || (patch.rva > 0x12CB5F0 && patch.rva < 0x12CBA54) ||
                  (patch.rva > 0x9605E0 && patch.rva < 0x960844),
              "session patch lies in the Users, packet Controller or UiVoiceChat_Notify constructor", patch.rva);
    }
    // UiVoiceChat_Notify: the constructor's count feeds the record resize; the update (vtable slot 1) writes records
    // 0x50 bytes apart from +0x140 for every room member.
    Check(CallTargets(image.At(0x9606AE, 5), 0x9606AE, 0x12D85B0) && CallTargets(image.At(0x960789, 5), 0x960789, 0x9621A0),
          "the voice chat HUD allocates and resizes its records in the constructor", 0x960789);
    Check(SlotTargets(image.At(0x1811998, 8), image.nt->OptionalHeader.ImageBase, 0x961140), "961140 is UiVoiceChat_Notify's update", 0x1811998);
    // ...and frees them in 961930 (from its destructor 960990 / 960A20, which a mission's HUD runs when it goes) the way
    // std::allocator does: a size of 0x1000 or more (`cmp rdx, 0x1000; jb`) is an aligned block whose address is at
    // [buffer-8] (`mov r8, [rcx-8]`), and anything but 8..0x27 bytes below the buffer fails fast (`cmp rax, 0x1f; ja`).
    // The grown allocation is past that size, so it has to be such a block (VectorOperatorNew).
    constexpr std::array<std::uint8_t, 7> freeBigCheck = {0x48, 0x81, 0xFA, 0x00, 0x10, 0x00, 0x00};  // cmp rdx, 0x1000
    constexpr std::array<std::uint8_t, 4> freeBlockRead = {0x4C, 0x8B, 0x41, 0xF8};                    // mov r8, [rcx-8]
    constexpr std::array<std::uint8_t, 4> freeShiftCheck = {0x48, 0x83, 0xF8, 0x1F};                   // cmp rax, 0x1f
    Check(std::memcmp(image.At(0x961A25, 7), freeBigCheck.data(), 7) == 0 && std::memcmp(image.At(0x961A32, 4), freeBlockRead.data(), 4) == 0 &&
              std::memcmp(image.At(0x961A3D, 4), freeShiftCheck.data(), 4) == 0 && CallTargets(image.At(0x961A46, 5), 0x961A46, 0x12D85EC),
          "961930 frees records of 0x1000 bytes or more as an aligned STL block", 0x961A25);
    Check(CallTargets(image.At(0x9609CF, 5), 0x9609CF, 0x961930) && CallTargets(image.At(0x960B48, 5), 0x960B48, 0x961930),
          "UiVoiceChat_Notify's destructors free the records through 961930", 0x9609CF);
    Check(0x50u * kMaxPlayers >= kBigAllocationThreshold, "the grown record allocation is an aligned STL block", 0x9606A9);
    const std::uint8_t recordBase[] = {0x48, 0x8B, 0x98, 0x40, 0x01, 0x00, 0x00};  // mov rbx, [rax+0x140]
    const std::uint8_t recordStep[] = {0x49, 0x83, 0xC5, 0x50};                    // add r13, 0x50
    Check(std::memcmp(image.At(0x961560, 7), recordBase, 7) == 0 && std::memcmp(image.At(0x9616DD, 4), recordStep, 4) == 0,
          "the voice chat HUD update writes 0x50-byte records from +0x140", 0x961560);
    Check(CallTargets(image.At(0x12BD4D7, 5), 0x12BD4D7, 0x12B77E0), "the room constructor builds its Users with 12B77E0", 0x12BD4D7);
    Check(CallTargets(image.At(0x12B7967, 5), 0x12B7967, 0x732410), "Users grows its empty slot vector through 732410", 0x12B7967);
    Check(CallTargets(image.At(0x12D2678, 5), 0x12D2678, 0x12CB5F0), "ControllerImpl builds on the packet Controller 12CB5F0", 0x12D2678);
    Check(CallTargets(image.At(0x12CB97E, 5), 0x12CB97E, 0x12D2060) && CallTargets(image.At(0x12CBA0A, 5), 0x12CBA0A, 0x12CAA40),
          "the session count feeds the reserve and the resize", 0x12CB97E);
    const std::uint8_t slotSearch[] = {0x4D, 0x39, 0x34, 0xC1};                                                // cmp [r9+rax*8], r14
    const std::uint8_t slotStore[] = {0x49, 0x63, 0x4C, 0x24, 0x40, 0x48, 0xC1, 0xE1, 0x04, 0x49, 0x03, 0x4D, 0x00};  // slot = [user+0x40]
    Check(std::memcmp(image.At(0x12B8058, 4), slotSearch, 4) == 0 && std::memcmp(image.At(0x12B824A, 13), slotStore, 13) == 0,
          "Users::Add takes the first empty slot and stores the user at its index", 0x12B8058);
    const std::uint8_t sessionIndex[] = {0x4C, 0x63, 0x60, 0x40, 0x49, 0xC1, 0xE4, 0x04, 0x49, 0x8B, 0x45, 0x20};  // [ctrl+0x20][slot]
    Check(std::memcmp(image.At(0x12CDA8E, 12), sessionIndex, 12) == 0, "packet sessions are indexed by the user slot", 0x12CDA8E);

    // Mission phase tables.
    const auto missionPatches = MissionPatches();
    const auto missionHooks = MissionHooks();
    const auto missionCalls = MissionCalls();
    for (const auto& patch : missionPatches) {
        Check(Matches(image.At(patch.rva, patch.original.size()), patch), patch.name, patch.rva);
        Check(patch.original.size() == patch.replacement.size() && patch.original != patch.replacement, "mission patch changes bytes in place", patch.rva);
    }
    for (const auto& hook : missionHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        Check(hook.original.size() >= 5 && hook.displacedOffset + hook.displacedSize <= hook.original.size(), "hook covers a jump", hook.rva);
    }
    for (const auto& call : missionCalls) Check(CallTargets(image.At(call.rva, 5), call.rva, call.target), call.name, call.rva);
    Check(missionPatches.size() == 16 && missionHooks.size() == 25 && missionCalls.size() == 5, "mission table sizes");
    // The script VM's player table: four 0x18-byte entries built at +0x168 by its constructor (eh vector constructor
    // 12D8D44 with size 0x18, count 4), and the four unbounded readers BvmPlayerTableHooks bound.
    const std::uint8_t bvmTable[] = {0x48, 0x8D, 0x8F, 0x68, 0x01, 0x00, 0x00};  // 20DCAC lea rcx, [rdi+0x168]
    const std::uint8_t bvmShape[] = {0x8D, 0x56, 0x18, 0x44, 0x8D, 0x46, 0x04};  // 20DCCB lea edx, [rsi+0x18]; lea r8d, [rsi+4]
    Check(std::memcmp(image.At(0x20DCAC, 7), bvmTable, 7) == 0 && std::memcmp(image.At(0x20DCCB, 7), bvmShape, 7) == 0 &&
              CallTargets(image.At(0x20DCD2, 5), 0x20DCD2, 0x12D8D44),
          "the script VM builds its player table of four 0x18-byte entries at +0x168", 0x20DCAC);
    const std::uint8_t bvmCount[] = {0x8B, 0x80, 0xF8, 0x4F, 0x01, 0x00};  // 2252B9 mov eax, [rax+0x14FF8]
    Check(std::memcmp(image.At(0x2252B9, 6), bvmCount, 6) == 0 && CallTargets(image.At(0x21F3B2, 5), 0x21F3B2, 0x225290) &&
              CallTargets(image.At(0x21F8CA, 5), 0x21F8CA, 0x225290),
          "21F380 loops over the online player count (225290)", 0x21F3B2);
    const std::uint8_t indexed[] = {0x48, 0x63, 0xC2};  // movsxd rax, edx
    for (const std::uint32_t entry : {0x22274Fu, 0x228ADDu, 0x22A671u})
        Check(std::memcmp(image.At(entry, 3), indexed, 3) == 0, "a BVM entry reader takes its index in edx", entry);
    for (const auto& hook : BvmPlayerTableHooks()) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        Check(BvmPlayerTableHandler(hook.rva) != nullptr, "every BVM table site has a handler", hook.rva);
    }
    // The bounds an imm8 could not hold (widecmp.h): each is the game's `cmp, 4` and the jcc after it.
    const auto compares = [] {
        auto all = SessionCompares();
        const auto mission = MissionCompares();
        all.insert(all.end(), mission.begin(), mission.end());
        return all;
    }();
    for (const auto& site : compares) {
        const Patch verify{site.name, site.rva, site.original, site.original};
        Check(Matches(image.At(site.rva, site.original.size()), verify), site.name, site.rva);
        Check(site.original.size() >= 5, "a widened compare covers a jump", site.rva);
    }
    // FindPlayerIndex: the "not found" `lea eax, [rbp-5]` is reached only by the loop's fall-through, and the
    // epilogue after it reads no flags, so `or eax, -1` stands for it at any count.
    Check(image.At(0x1DA405, 2)[0] == 0xEB && image.At(0x1DA405, 2)[1] == 0x03,
          "FindPlayerIndex jumps from its not-found value to the epilogue", 0x1DA405);
    {
        const auto* code = IMAGE_FIRST_SECTION(image.nt);
        const std::uint8_t* text = image.At(code->VirtualAddress, code->SizeOfRawData);
        int into = 0;
        // Short and near jumps inside FindPlayerIndex (1DA340..1DA429) that land on 1DA402.
        for (std::uint32_t rva = 0x1DA340; text && rva < 0x1DA429; ++rva) {
            const std::uint8_t* at = image.At(rva, 6);
            if ((at[0] & 0xF0) == 0x70 || at[0] == 0xEB) into += rva + 2 + static_cast<std::int8_t>(at[1]) == 0x1DA402;
        }
        Check(into == 0, "nothing in FindPlayerIndex jumps to its not-found value", 0x1DA402);
    }
    // The ninth remote flag would land on the user vector CreatePlayers keeps at rsp+0x30 and re-reads
    // every pass of the loop that writes the flags (mission.cpp, RemoteFlagHandler).
    const std::uint8_t vectorBegin[] = {0x48, 0x8B, 0x7C, 0x24, 0x30};  // 1D98E9 mov rdi, [rsp+0x30]
    Check(std::memcmp(image.At(0x1D98E9, 5), vectorBegin, 5) == 0 && std::memcmp(image.At(0x1D985E, 5), vectorBegin, 5) == 0,
          "the flags at rsp+0x28 have eight bytes before the user vector at rsp+0x30", 0x1D98E9);
    const auto missionSlots = MissionSlots();
    Check(missionSlots.size() == 3, "mission slot table size");
    for (const auto& missionSlot : missionSlots)
        Check(SlotTargets(image.At(missionSlot.rva, 8), image.nt->OptionalHeader.ImageBase, missionSlot.target), missionSlot.name, missionSlot.rva);
    // The three item stores are the same leaf: movsxd r9, [rdx]; mov rax, [GameStatus]; mov rcx, [r8]; mov [rax+r9*8+0x14FD4], rcx; ret
    for (const auto& missionSlot : missionSlots) {
        const std::uint8_t* leaf = image.At(missionSlot.target, 22);
        const std::uint8_t head[] = {0x4C, 0x63, 0x0A, 0x48, 0x8B, 0x05};
        const std::uint8_t tail[] = {0x49, 0x8B, 0x08, 0x4A, 0x89, 0x8C, 0xC8, 0xD4, 0x4F, 0x01, 0x00, 0xC3};
        Check(leaf && std::memcmp(leaf, head, 6) == 0 && RipTarget(missionSlot.target + 3, 7) == kGameStatusPointer &&
                  std::memcmp(leaf + 10, tail, sizeof(tail)) == 0,
              "item store callback writes GameStatus+0x14FD4 + position*8", missionSlot.target);
    }
    // Constants mission.cpp relies on, where the game itself uses them.
    Check(RipTarget(0x790899, 7) == kGameStatusPointer, "MissionSync_Update loads the GameStatus pointer", 0x790899);
    const std::uint8_t recordsLea[] = {0x48, 0x8D, 0x88, 0x78, 0x4C, 0x01, 0x00};  // lea rcx, [rax+0x14C78]
    Check(std::memcmp(image.At(0x7908A5, 7), recordsLea, 7) == 0, "loadout records start at GameStatus+0x14C78", 0x7908A5);
    // The online HUD's colour tables (patches.h, HudColourPatches / HudIndexWrapHooks): 7FFBD0 writes the
    // player's index to the local each caller indexes its table with, and every one of those tables is built
    // for four entries. These three readers are the only callers of 7FFBD0, so widening the three tables (or
    // wrapping the index) covers every use.
    const std::uint8_t statusIndexOut[] = {0x48, 0x8D, 0x54, 0x24, 0x64};  // 804EB6 lea rdx, [rsp+0x64]
    const std::uint8_t chatIndexOut[] = {0x48, 0x8D, 0x55, 0xB0};          // 801820 lea rdx, [rbp-0x50]
    const std::uint8_t radarIndexOut[] = {0x48, 0x8D, 0x54, 0x24, 0x44};   // 82A1FC lea rdx, [rsp+0x44]
    Check(std::memcmp(image.At(0x804EB6, 5), statusIndexOut, 5) == 0 && CallTargets(image.At(0x804EBF, 5), 0x804EBF, 0x7FFBD0),
          "the status HUD asks 7FFBD0 for the index at rsp+0x64", 0x804EBF);
    Check(std::memcmp(image.At(0x801820, 4), chatIndexOut, 4) == 0 && CallTargets(image.At(0x801828, 5), 0x801828, 0x7FFBD0),
          "the chat HUD asks 7FFBD0 for the index at rbp-0x50", 0x801828);
    Check(std::memcmp(image.At(0x82A1FC, 5), radarIndexOut, 5) == 0 && CallTargets(image.At(0x82A205, 5), 0x82A205, 0x7FFBD0),
          "the radar asks 7FFBD0 for the index at rsp+0x44", 0x82A205);
    const std::uint8_t statusLamp[] = {0x8B, 0x44, 0x24, 0x64, 0x48, 0x8D, 0x04, 0x40, 0x48, 0xC1,
                                       0xE0, 0x04, 0x48, 0x03, 0x87, 0xD8, 0x00, 0x00, 0x00};  // 8056F6 [rdi+0xD8] + index*0x30
    const std::uint8_t chatEntry[] = {0x4C, 0x63, 0x6D, 0xB0, 0x49, 0xC1, 0xE5, 0x05};  // 801D7F index*0x20
    const std::uint8_t radarColour[] = {0x48, 0x63, 0x44, 0x24, 0x44, 0x48, 0xC1, 0xE0,
                                        0x04, 0x4C, 0x8D, 0x8D, 0x50, 0x01, 0x00, 0x00};  // 82A6CD rbp+0x150 + index*0x10
    Check(std::memcmp(image.At(0x8056F6, sizeof(statusLamp)), statusLamp, sizeof(statusLamp)) == 0,
          "the status HUD takes the player lamp from table[index]", 0x8056F6);
    Check(std::memcmp(image.At(0x801D7F, sizeof(chatEntry)), chatEntry, sizeof(chatEntry)) == 0,
          "the chat HUD takes the balloon from table[index]", 0x801D7F);
    Check(std::memcmp(image.At(0x82A6CD, sizeof(radarColour)), radarColour, sizeof(radarColour)) == 0 &&
              std::memcmp(image.At(0x82A743, sizeof(radarColour)), radarColour, sizeof(radarColour)) == 0,
          "the radar takes the marker colour from table[index]", 0x82A6CD);
    const std::uint8_t fourBy30[] = {0xB9, 0xC0, 0x00, 0x00, 0x00};  // mov ecx, 4 * 0x30
    const std::uint8_t fourBy20[] = {0xB9, 0x80, 0x00, 0x00, 0x00};  // mov ecx, 4 * 0x20
    Check(std::memcmp(image.At(0x8071F7, 5), fourBy30, 5) == 0 && std::memcmp(image.At(0x807400, 5), fourBy30, 5) == 0,
          "HudPlayer_MultiPlayStatus builds its class and lamp tables for four", 0x807400);
    Check(std::memcmp(image.At(0x802A95, 5), fourBy20, 5) == 0, "HudPlayer_Chat builds its balloon table for four", 0x802A95);
    {
        // Every E8/E9 rel32 in the code section that lands on 7FFBD0.
        const auto* code = IMAGE_FIRST_SECTION(image.nt);
        const std::uint8_t* text = image.At(code->VirtualAddress, code->SizeOfRawData);
        int callers = 0;
        for (std::uint32_t i = 0; text && i + 5 <= code->SizeOfRawData; ++i)
            if ((text[i] == 0xE8 || text[i] == 0xE9) && CallTargets(text + i, code->VirtualAddress + i, 0x7FFBD0)) ++callers;
        Check(callers == 3, "7FFBD0 has exactly the three HUD callers", 0x7FFBD0);
    }
    const auto hudPatches = HudColourPatches();
    const auto hudHooks = HudColourHooks();
    const auto hudWrap = HudIndexWrapHooks();
    for (const auto& patch : hudPatches) {
        Check(Matches(image.At(patch.rva, patch.original.size()), patch), patch.name, patch.rva);
        Check(patch.original.size() == patch.replacement.size() && patch.original != patch.replacement, "HUD patch changes bytes in place", patch.rva);
    }
    for (const auto* list : {&hudHooks, &hudWrap})
        for (const auto& hook : *list) {
            const Patch verify{hook.name, hook.rva, hook.original, hook.original};
            Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
            Check(hook.original.size() >= 5 && hook.displacedOffset + hook.displacedSize <= hook.original.size(), "hook covers a jump", hook.rva);
            Check(list == &hudWrap ? MissionHookHandler(hook.rva) != nullptr : HudColourHookHandler(hook.rva) != nullptr,
                  "every HUD site has a handler", hook.rva);
        }
    Check(hudPatches.size() == 15 && hudHooks.size() == 4 && hudWrap.size() == 1, "HUD table sizes");
    // The radar's index comes from 7FFBD0's two stores to [r14] (rdx = rsp+0x44 of the radar, 82A1FC): online
    // 7FFD99 `mov [r14], ecx` after `movsxd rcx, [rsi+0x48]` (the site PlayerTagIndexHandler replaces and wraps),
    // offline 7FFDFA after `mov ecx, [rdx+0x338]` (the split-screen number); before both it stores -1 through rdx
    // itself (7FFC17, r14 = rdx).
    const std::uint8_t indexOnline[] = {0x48, 0x63, 0x4E, 0x48, 0x41, 0x89, 0x0E};  // 7FFD95
    const std::uint8_t indexOffline[] = {0x8B, 0x8A, 0x38, 0x03, 0x00, 0x00, 0x41, 0x89, 0x0E};  // 7FFDF4
    const std::uint8_t minusOne[] = {0x48, 0xC7, 0xC7, 0xFF, 0xFF, 0xFF, 0xFF, 0x89, 0x3A};  // 7FFC10 mov rdi, -1; mov [rdx], edi
    Check(std::memcmp(image.At(0x7FFD95, sizeof(indexOnline)), indexOnline, sizeof(indexOnline)) == 0 &&
              std::memcmp(image.At(0x7FFDF4, sizeof(indexOffline)), indexOffline, sizeof(indexOffline)) == 0 &&
              std::memcmp(image.At(0x7FFC10, sizeof(minusOne)), minusOne, sizeof(minusOne)) == 0,
          "7FFBD0 stores the radar's index as -1, the online index or the split-screen number", 0x7FFD95);
    {
        // Every `mov [r14], r32` (41 89 /r, mod 00, r/m r14) in 7FFBD0 (7FFBD0..7FFEE6 by its unwind entry): the two.
        int stores = 0;
        const std::uint8_t* body = image.At(0x7FFBD0, 0x316);
        for (std::uint32_t i = 0; body && i + 3 <= 0x316; ++i)
            if (body[i] == 0x41 && body[i + 1] == 0x89 && (body[i + 2] & 0xC7) == 0x06) ++stores;
        Check(stores == 2, "7FFBD0 writes a player index out at 7FFD99 and 7FFDFA and nowhere else", 0x7FFBD0);
    }
    // Whatever index reaches the radar: a player's wraps around the colour table, a negative one keeps the game's
    // in-frame address.
    const std::uint64_t frame = 0x10000;
    for (const std::int32_t index : {0, 3, 31, 32, 33, 1023, 0x7FFFFFFF}) {
        const std::uint64_t address = RadarColourAddress(index, frame);
        Check(address == reinterpret_cast<std::uintptr_t>(RadarColour(index % kHudColourCount)),
              "a player's radar colour is a table entry, wrapped", static_cast<std::uint32_t>(index));
    }
    Check(RadarColourAddress(-1, frame) == frame + 0x150 - 16, "a unit that is no player keeps the game's address");
    // What the hooks replace: the lamp texture name, the chat balloon names (in the order of the colours) and
    // the radar's four colours - the game's own are the first four entries of hudcolours.h.
    const auto wideAt = [&](std::uint32_t rva) { return reinterpret_cast<const wchar_t*>(image.At(rva, 2)); };
    Check(RipTarget(0x8074E7, 7) == 0x17F6CF0 && std::wcscmp(wideAt(0x17F6CF0), L"player_lamp.dds") == 0,
          "the lamp loop loads player_lamp.dds", 0x8074E7);
    for (int i = 0; i < 4; ++i) {
        const std::uint32_t lea = 0x802B5E + 7 * static_cast<std::uint32_t>(i);
        Check(std::wcscmp(wideAt(RipTarget(lea, 7)), kHudColours[i].chatTexture) == 0, "chat balloon i of the game is colour i", lea);
        const std::uint32_t load = 0x82A0EB + 14 * static_cast<std::uint32_t>(i);  // movaps xmm, [rip+x]; movaps [rbp+0x150+16i], xmm
        const std::uint8_t store = static_cast<std::uint8_t>(0x50 + 16 * i);
        Check(std::memcmp(image.At(RipTarget(load, 7), 16), RadarColour(i), 16) == 0 && image.At(load + 10, 1)[0] == store,
              "radar colour i of the game is colour i", load);
    }
    for (int i = 0; i < kHudColourCount; ++i)
        for (int j = 0; j < i; ++j)
            Check(_wcsicmp(kHudColours[i].chatTexture, kHudColours[j].chatTexture) != 0 && kHudColours[i].rgb != kHudColours[j].rgb,
                  "every HUD colour is its own", static_cast<std::uint32_t>(i));
    Check(std::bit_cast<std::uint32_t>(1.0f / kHudColourCount) == 0x3D000000 && kHudColourCount == 32,
          "32 lamps, 1/32 of the texture each (the stride written into the lamp loop)");
    const std::uint8_t contextSize[] = {0xBA, 0xF8, 0x02, 0x00, 0x00};  // mov edx, 0x2F8 (sized delete)
    Check(std::memcmp(image.At(0x1D6F31, 5), contextSize, 5) == 0 && kMovedPlayerArray >= 0x2F8, "moved array starts after MissionContext", 0x1D6F31);
    const std::uint8_t vanillaArray[] = {0x48, 0x8D, 0x8B, 0xE8, 0x00, 0x00, 0x00};  // lea rcx, [rbx+0xE8], rbx = this+0x18
    Check(std::memcmp(image.At(0x1D628E, 7), vanillaArray, 7) == 0, "constructor builds the player array at +0x100", 0x1D628E);
    Check(RipTarget(0x1D6295, 7) == 0x1D6C50 && RipTarget(0x1D6ED5, 7) == 0x1D6C50, "constructor and destructor use the weak_ptr destructor", 0x1D6ED5);

    // The mission start message (packetfit.h): the record write/read calls, the hook after the host's record loop,
    // and what the fix is built on - where the stream is, how a record and a stream are laid out, and the sizes the
    // game's transport allows.
    const auto packetFitCalls = PacketFitCalls();
    const auto packetFitHooks = PacketFitHooks();
    Check(packetFitCalls.size() == 2 && packetFitHooks.size() == 1, "packet fit table sizes");
    for (const auto& call : packetFitCalls) Check(CallTargets(image.At(call.rva, 5), call.rva, call.target), call.name, call.rva);
    for (const auto& hook : packetFitHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        Check(hook.original.size() >= 5 && hook.displacedOffset + hook.displacedSize <= hook.original.size(), "hook covers a jump", hook.rva);
    }
    const auto bytesAt = [&](std::uint32_t rva, std::initializer_list<std::uint8_t> bytes) {
        const std::vector<std::uint8_t> expected(bytes);
        const std::uint8_t* at = image.At(rva, expected.size());
        return at && std::memcmp(at, expected.data(), expected.size()) == 0;
    };
    Check(CallTargets(image.At(0x1DC525, 5), 0x1DC525, 0x591130) &&
              bytesAt(0x1DC544, {0x48, 0x85, 0xC0, 0x75, 0x0C}) &&
              bytesAt(0x1DC549, {0x4D, 0x89, 0x37, 0x4D, 0x89, 0x77, 0x08}) &&
              bytesAt(0x1D9B17, {0x4D, 0x85, 0xF6, 0x74, 0x4D}),
          "mission admission call has native null output and outer skip guards", 0x1DC525);
    Check(bytesAt(0x78D11B, {0x4C, 0x89, 0x44, 0x24, 0x60}) && bytesAt(0x78D5AB, {0x48, 0x8B, 0x7C, 0x24, 0x60}),
          "MissionSync_Res keeps its output stream at rsp+0x60 and writes the count from it", 0x78D11B);
    Check(CallTargets(image.At(0x78D6E3, 5), 0x78D6E3, 0x773740) && CallTargets(image.At(0x78D5B6, 5), 0x78D5B6, 0x12B5580),
          "MissionSync_Res reads each reply's record back and writes the count before the records", 0x78D6E3);
    Check(bytesAt(0x77377C, {0x41, 0x89, 0x0E}) && bytesAt(0x790878, {0x48, 0x63, 0x4D, 0x20, 0x85, 0xC9, 0x0F, 0x88}),
          "a record's index is its first dword, and a negative one is skipped", 0x790878);
    Check(bytesAt(0x12B4665, {0x4C, 0x8B, 0x51, 0x08}) && bytesAt(0x12B5200, {0x48, 0x8B, 0x81, 0xF0, 0x05, 0x00, 0x00}) &&
              bytesAt(0x12B5217, {0x41, 0x80, 0xE1, 0x1F, 0x41, 0x80, 0xC9, 0xA0}) &&
              bytesAt(0x12B45C8, {0xBA, 0xF8, 0x05, 0x00, 0x00}),
          "streams read at +8, write at +0x5F0, are 0x5F8 bytes, and byte arrays are tagged 0xA0", 0x12B5200);
    Check(bytesAt(0x12D0B96, {0xC7, 0x01, 0x00, 0x12, 0x40, 0x00}) && bytesAt(0x12D0BE6, {0x48, 0x3D, 0x78, 0x05, 0x00, 0x00}) &&
              bytesAt(0x12D0017, {0x48, 0x81, 0xFA, 0x4C, 0x04, 0x00, 0x00}) && bytesAt(0x74ED6D, {0x41, 0xB9, 0xFA, 0x00, 0x00, 0x00}),
          "the controller frames packets with 0x401200 and allows 1400 bytes, datagrams 1100, the builder 250", 0x12D0BE6);
    Check(bytesAt(0x12C8D1D, {0xC7, 0x44, 0x24, 0x60, 0x00, 0x10, 0x00, 0x00}) && bytesAt(0x12C8D25, {0x48, 0x89, 0x74, 0x24, 0x68}),
          "the game receives up to 4096 bytes from any channel", 0x12C8D1D);

    const auto spawnHooks = SpawnHooks();
    Check(spawnHooks.size() == 8, "spawn hook table size");
    for (const auto& hook : spawnHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        Check(hook.original.size() >= 5 && hook.displacedOffset == 0 && hook.displacedSize == hook.original.size(),
              "spawn hooks run all of their instructions after the handler", hook.rva);
    }
    // The group loops read the count again from the parameter block each pass (`cmp r12d, [r14+0x68]` and
    // `cmp r12d, [r13+0x50]`), which is what lets the handler raise it after the first object.
    const std::uint8_t pointLoop[] = {0x45, 0x3B, 0x66, 0x68};  // 1D932B cmp r12d, [r14+0x68]
    const std::uint8_t areaLoop[] = {0x45, 0x3B, 0x65, 0x50};   // 1D879A cmp r12d, [r13+0x50]
    Check(std::memcmp(image.At(0x1D932B, 4), pointLoop, 4) == 0, "CreateObjectGroup loop compares with params+0x68", 0x1D932B);
    Check(std::memcmp(image.At(0x1D879A, 4), areaLoop, 4) == 0, "CreateAreaObject loop compares with params+0x50", 0x1D879A);
    // "copy armor" (armor.cpp): the reads of the armor pickup count that are raised, and the four writes of
    // it that must stay clear of them, or a raised count would reach the save. The read the room is shown
    // (D7BFA, inside D7BD0) is deliberately not raised, so nobody ever copies a copied number.
    // 1DC4FB picks the path: online every player comes from a loadout record (595A12), offline this machine's
    // own comes straight from the save (595CD9). Both have to be raised or the armor only reaches one of them.
    const std::uint8_t onlineBranch[] = {0x83, 0xF9, 0xFF};  // 1DC4FB cmp ecx, -1
    Check(std::memcmp(image.At(0x1DC4FB, 3), onlineBranch, 3) == 0 &&
              CallTargets(image.At(0x1DC525, 5), 0x1DC525, 0x591130) &&
              CallTargets(image.At(0x1DC539, 5), 0x1DC539, 0x591410),
          "CreatePlayers builds this machine's player from a record online and from the save offline", 0x1DC4FB);
    Check(CallTargets(image.At(0x591448, 5), 0x591448, 0x595C80), "the offline path reads the save at 595C80", 0x591448);
    const auto armorHooks = ArmorHooks();
    Check(armorHooks.size() == 3, "armor hook table size");
    for (const auto& hook : armorHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        Check(hook.displacedSize == 0 && hook.original.size() >= 5, "the armor handlers replace the whole read", hook.rva);
    }
    const std::uint8_t armorStore[] = {0x89, 0x8C, 0x98, 0x88, 0x6F, 0x00, 0x00};  // AB311 mov [rax+rbx*4+0x6F88], ecx
    const std::uint8_t armorScreen[] = {0x44, 0x89, 0x84, 0x90, 0x88, 0x6F, 0x00, 0x00};  // 87AF8F ..., r8d
    Check(std::memcmp(image.At(0x0AB311, sizeof(armorStore)), armorStore, sizeof(armorStore)) == 0,
          "the save load writes the pickup count without reading it", 0x0AB311);
    Check(std::memcmp(image.At(0x87AF8F, sizeof(armorScreen)), armorScreen, sizeof(armorScreen)) == 0,
          "the armor screen writes the pickup count from its own state", 0x87AF8F);
    const std::uint8_t armorFromScreen[] = {0xF3, 0x4C, 0x0F, 0x2C, 0x81, 0xC0, 0x07, 0x00, 0x00};  // 87AF6F
    Check(std::memcmp(image.At(0x87AF6F, sizeof(armorFromScreen)), armorFromScreen, sizeof(armorFromScreen)) == 0,
          "and that state is a float of its own, not a read of the count", 0x87AF6F);
    // The rules the copied armor is worked out with: base and step per class, from the game data.
    Check(CallTargets(image.At(0x0D7C07, 5), 0x0D7C07, 0x0E3470) && CallTargets(image.At(0x0D7C14, 5), 0x0D7C14, 0x0E34A0),
          "armor = base(class) + step(class) * pickups", 0x0D7BD0);
    const std::uint8_t roomArmor[] = {0x8B, 0xBC, 0x91, 0x88, 0x6F, 0x00, 0x00};  // D7BFA, left as the game has it
    Check(std::memcmp(image.At(0x0D7BFA, sizeof(roomArmor)), roomArmor, sizeof(roomArmor)) == 0,
          "the armor the room is shown is still read straight from the save", 0x0D7BFA);
    for (const auto& hook : armorHooks) Check(hook.rva != 0x0D7BFA, "and is not one of the raised reads", hook.rva);

    for (const auto& hook : DiagnosticHooks()) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
    }
    for (const auto& call : DiagnosticCalls()) Check(CallTargets(image.At(call.rva, 5), call.rva, call.target), call.name, call.rva);
    for (const auto& call : RecoveryCalls()) Check(CallTargets(image.At(call.rva, 5), call.rva, call.target), call.name, call.rva);

    // KeepRoomOnPeerTimeout (peertimeout.h): the sites, and every step of the chain from a timed-out handshake to
    // EOS_Lobby_LeaveLobby that the redirected call cuts, as the game has them.
    const auto peerTimeoutHooks = PeerTimeoutHooks();
    const auto peerTimeoutCalls = PeerTimeoutCalls();
    Check(peerTimeoutHooks.size() == 1 && peerTimeoutCalls.size() == 1, "peer timeout table sizes");
    for (const auto& hook : peerTimeoutHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        Check(hook.displacedOffset == 0 && hook.displacedSize == hook.original.size(), "the join-time hook keeps both moves", hook.rva);
    }
    for (const auto& call : peerTimeoutCalls) Check(CallTargets(image.At(call.rva, 5), call.rva, call.target), call.name, call.rva);
    const auto Bytes = [&](std::uint32_t rva, std::initializer_list<std::uint8_t> bytes) {
        const auto at = image.At(rva, bytes.size());
        return at && std::memcmp(at, bytes.begin(), bytes.size()) == 0;
    };
    // Users::Add: the join-time hook sits right after make_shared<eos::User>, with rbx still the {id, remote} argument
    // (12B7F7A `mov rbx, r8`; the only other writes of rbx before 12B80E0 are on the paths that return empty).
    Check(CallTargets(image.At(0x12B80DB, 5), 0x12B80DB, 0x12B7610) && Bytes(0x12B7F7A, {0x49, 0x8B, 0xD8}) &&
              Bytes(0x12B80CB, {0x4C, 0x8D, 0x4B, 0x08, 0x4C, 0x8B, 0xC3}),
          "Users::Add makes the user at 12B7610, rbx = its argument", 0x12B80DB);
    // The room update: `mov rcx, rdi; call IsLocalHost; test al, al; jne skip; mov rcx, rdi; call AnyLinkTimedOut;
    // test al, al; je skip`, then the leave.
    Check(Bytes(0x788AB0, {0x48, 0x8B, 0xCF}) && CallTargets(image.At(0x788AB3, 5), 0x788AB3, 0x12BE580) &&
              Bytes(0x788AB8, {0x84, 0xC0, 0x75, 0x47, 0x48, 0x8B, 0xCF}) && Bytes(0x788AC4, {0x84, 0xC0, 0x74, 0x3B}),
          "the room update asks IsLocalHost first, then whether a link timed out", 0x788AB0);
    Check(CallTargets(image.At(0x788AE7, 5), 0x788AE7, 0x787090) && CallTargets(image.At(0x787262, 5), 0x787262, 0x728740) &&
              Bytes(0x78724A, {0x48, 0x8D, 0xA8, 0x68, 0xFF, 0xFF, 0xFF}),
          "a timed-out link leaves the room: 787090 hands 728740 an empty room", 0x788AE7);
    Check(Bytes(0x728A1C, {0x48, 0x89, 0x87, 0xC0, 0x00, 0x00, 0x00}) && CallTargets(image.At(0x74214E, 5), 0x74214E, 0x741AB0) &&
              CallTargets(image.At(0x741AD7, 5), 0x741AD7, 0x12BFB30) && CallTargets(image.At(0x12BFBF5, 5), 0x12BFBF5, 0x12BE860),
          "releasing the room runs RoomImpl's destructor down to 12BE860", 0x728A1C);
    // 12BE96C: call [IAT slot] of EOS_Lobby_LeaveLobby.
    const auto ImportAt = [&](std::uint32_t slot) -> std::string {
        const auto& directory = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        for (std::uint32_t d = directory.VirtualAddress;; d += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            const auto descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.At(d, sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            if (!descriptor || !descriptor->Name) return {};
            for (std::uint32_t i = 0;; ++i) {
                const auto entry = image.At(descriptor->OriginalFirstThunk + 8 * i, 8);
                std::uint64_t value = 0;
                if (entry) std::memcpy(&value, entry, 8);
                if (!value) break;
                if (descriptor->FirstThunk + 8 * i != slot || (value >> 63)) continue;
                // IMAGE_IMPORT_BY_NAME: a 2-byte hint, then the name.
                const auto name = reinterpret_cast<const char*>(image.At(static_cast<std::uint32_t>(value) + 2, 1));
                return name ? std::string(name) : std::string{};
            }
        }
    };
    Check(Bytes(0x12BE96C, {0xFF, 0x15}) && ImportAt(RipTarget(0x12BE96C, 6)) == "EOS_Lobby_LeaveLobby",
          "12BE860 leaves the EOS lobby", 0x12BE96C);
    // IsLocalHost: Users (room+0x160) -> local user (+0x18) -> flags bit 2.
    Check(Bytes(0x12BE597, {0x48, 0x8B, 0x89, 0x60, 0x01, 0x00, 0x00}) && Bytes(0x12BE5AF, {0x48, 0x8B, 0x49, 0x18}) &&
              CallTargets(image.At(0x12BE5BD, 5), 0x12BE5BD, 0x12AC6F0) &&
              Bytes(0x12AC6F0, {0x8B, 0x41, 0x10, 0xC1, 0xE8, 0x02, 0x24, 0x01, 0xC3}) &&
              kRoomUsersOffset == 0x160 && kUsersLocalOffset == 0x18,
          "12BE580 asks the local user (room+0x160, +0x18) for its host flag", 0x12BE580);
    // AnyLinkTimedOut: manager = room+0xB0, then 12C86B0 walks the link list (manager+0xD0, node+0x30 = Link).
    Check(Bytes(0x12BE750, {0x48, 0x8B, 0x89, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x0F, 0x85}) &&
              RipTarget(0x12BE75A, 6) == 0x12C86B0 && Bytes(0x12BE760, {0x32, 0xC0, 0xC3}) && kRoomLinksOwnerOffset == 0xB0,
          "12BE750 = [room+0xB0] ? 12C86B0 : false", 0x12BE750);
    Check(Bytes(0x12C8757, {0x4C, 0x8B, 0x83, 0xD0, 0x00, 0x00, 0x00, 0x49, 0x8B, 0x10}) &&
              Bytes(0x12C8771, {0x48, 0x8B, 0x1F, 0x48, 0x3B, 0xDF}) && Bytes(0x12C8780, {0x48, 0x8B, 0x4B, 0x30}) &&
              CallTargets(image.At(0x12C8784, 5), 0x12C8784, 0x12D5A40) && Bytes(0x12C878D, {0x48, 0x8B, 0x1B}) &&
              kLinkListOffset == 0xD0 && kLinkNodeValueOffset == 0x30,
          "12C86B0 walks the list at manager+0xD0, Link at node+0x30, next at node+0", 0x12C86B0);
    Check(Bytes(0x12D5A40, {0x0F, 0xB6, 0x81, 0xA8, 0x00, 0x00, 0x00, 0xC3}) && kLinkTimedOutOffset == 0xA8,
          "12D5A40 returns Link+0xA8", 0x12D5A40);
    // Link::OnInitial: the weak_ptr<User> at +0x60/+0x68, the deadline at +0xA0 (20000 unless the peer hosts).
    Check(Bytes(0x12D5BEB, {0x48, 0x8B, 0x51, 0x68}) && Bytes(0x12D5C43, {0x48, 0x8B, 0x5F, 0x60}) &&
              Bytes(0x12D5C82, {0x0F, 0x2F, 0x87, 0xA0, 0x00, 0x00, 0x00}) &&
              Bytes(0x12D5E0B, {0xC7, 0x87, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x40, 0x9C, 0x46}) &&
              Bytes(0x12D5C90, {0xC6, 0x87, 0xA8, 0x00, 0x00, 0x00, 0x01}) &&
              kLinkUserOffset == 0x60 && kLinkDeadlineOffset == 0xA0,
          "Link::OnInitial: user weak_ptr +0x60, deadline +0xA0 (20000 ms), timed out +0xA8", 0x12D5AA0);
    // These constructors overwrite eos::lobby::Room's base vtable with eos::RoomImpl.
    // Tie the runtime guard to the actual image, so a fake fixture cannot repeat a wrong assumption.
    for (const auto site : {0x741088u, 0x741557u}) {
        Check(RipTarget(site, 7) == kActiveRoomVtableRva &&
              std::memcmp(image.At(site + 7, 3), "\x48\x89\x03", 3) == 0,
              "both live RoomImpl constructors install the vtable accepted by diagnostics/recovery", site);
    }
    Check(SlotTargets(image.At(static_cast<std::uint32_t>(kActiveRoomVtableRva) + 8, 8),
                      image.nt->OptionalHeader.ImageBase, 0x12BD420),
          "RoomImpl inherits the audited GetUsers implementation", static_cast<std::uint32_t>(kActiveRoomVtableRva));
    for (const auto& hook : GhostHooks()) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
    }
    for (const auto& call : GhostCalls()) Check(CallTargets(image.At(call.rva, 5), call.rva, call.target), call.name, call.rva);

    std::vector<CallSite> allCalls = calls;
    allCalls.insert(allCalls.end(), missionCalls.begin(), missionCalls.end());
    allCalls.insert(allCalls.end(), packetFitCalls.begin(), packetFitCalls.end());
    const auto ghostCalls = GhostCalls();
    allCalls.insert(allCalls.end(), ghostCalls.begin(), ghostCalls.end());
    const auto diagnosticCalls = DiagnosticCalls();
    allCalls.insert(allCalls.end(), diagnosticCalls.begin(), diagnosticCalls.end());
    const auto recoveryCalls = RecoveryCalls();
    allCalls.insert(allCalls.end(), recoveryCalls.begin(), recoveryCalls.end());
    allCalls.insert(allCalls.end(), peerTimeoutCalls.begin(), peerTimeoutCalls.end());
    auto allHooks = missionHooks;
    allHooks.insert(allHooks.end(), packetFitHooks.begin(), packetFitHooks.end());
    allHooks.insert(allHooks.end(), spawnHooks.begin(), spawnHooks.end());
    const auto diagnosticHooks = DiagnosticHooks();
    allHooks.insert(allHooks.end(), diagnosticHooks.begin(), diagnosticHooks.end());
    allHooks.insert(allHooks.end(), armorHooks.begin(), armorHooks.end());
    const auto ghostHooks = GhostHooks();
    allHooks.insert(allHooks.end(), ghostHooks.begin(), ghostHooks.end());
    allHooks.insert(allHooks.end(), peerTimeoutHooks.begin(), peerTimeoutHooks.end());
    // Member slots (userslots.h): Users::Add takes the first empty slot - the loop from 12B802E (`mov [rbp], r14d`,
    // index 0) over the slots (r9, 16 bytes each, r8 of them) stops at the first null, keeping the index in ecx and
    // [rbp] - and the User's constructor (12B7610) gets &[rbp]; Users::Remove empties slots[User+0x40] and moves nothing.
    const auto slotHooks = UserSlotHooks();
    Check(slotHooks.size() == 2, "member slot hook table size");
    for (const auto& hook : slotHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        Check(hook.displacedOffset == 0 && hook.displacedSize == hook.original.size(), "the slot hooks run their site unchanged",
              hook.rva);
        Check(UserSlotHookHandler(hook.rva) != nullptr, "every slot hook has a handler", hook.rva);
    }
    Check(Bytes(0x12B802E, {0x44, 0x89, 0x75, 0x00}) && Bytes(0x12B8058, {0x4D, 0x39, 0x34, 0xC1}) &&
              Bytes(0x12B8061, {0x89, 0x4D, 0x00}) && Bytes(0x12B8074, {0x75, 0x55}) && Bytes(0x12B80D2, {0x48, 0x8D, 0x55, 0x00}),
          "Users::Add: the first empty slot in ecx and [rbp], whose address the User's constructor gets", 0x12B806E);
    Check(Bytes(0x12B8869, {0x49, 0x8B, 0x1E, 0x48, 0x63, 0x6B, 0x40}) && Bytes(0x12B8A2B, {0x49, 0x03, 0x07, 0x33, 0xC9, 0x48, 0x89, 0x08}),
          "Users::Remove: rbp = User+0x40, that slot of [r15] is emptied", 0x12B8A24);
    allHooks.insert(allHooks.end(), slotHooks.begin(), slotHooks.end());
    // Host data (hostdataopen.h): the file open EDFModLoader wraps. After `call [vtable+0x10]` on rcx, rdi is the
    // path wstring (rdx), turned into its characters when not stored inline (capacity 8+); the moves at the site
    // hand them to the open (752E0); after it rdi only holds a result byte (`sete dil`/`setne dil`).
    const auto hostDataHooks = HostDataHooks();
    Check(hostDataHooks.size() == 1, "host data hook table size");
    for (const auto& hook : hostDataHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        Check(hook.displacedOffset == 0 && hook.displacedSize == hook.original.size(), "the open keeps both moves", hook.rva);
    }
    Check(Bytes(0x741E6, {0x48, 0x8B, 0xFA}) && Bytes(0x74206, {0x48, 0x83, 0x7F, 0x18, 0x08, 0x72, 0x03, 0x48, 0x8B, 0x3F}) &&
              Bytes(0x74217, {0x48, 0x8B, 0x0D}) && CallTargets(image.At(0x7421E, 5), 0x7421E, 0x752E0) &&
              Bytes(0x74266, {0x40, 0x0F, 0x94, 0xC7}) && Bytes(0x7428C, {0x40, 0x0F, 0x95, 0xC7}),
          "the file open: rdi = the path's characters, handed to 752E0, then only a result", 0x741C0);
    allHooks.insert(allHooks.end(), hostDataHooks.begin(), hostDataHooks.end());
    // Weapon guard (weaponguard.h): its two sites, and what it reads of the game.
    const auto guardHooks = WeaponGuardHooks();
    Check(guardHooks.size() == 2, "weapon guard hook table size");
    for (const auto& hook : guardHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
    }
    Check(guardHooks[0].displacedOffset == 0 && guardHooks[0].displacedSize == 4,
          "the room info site keeps its lea r12, [r13+0x38] (the nop after it goes)", guardHooks[0].rva);
    Check(Bytes(0x745640, {0x41, 0x8B, 0x34, 0x24}), "the room info loop starts after the site (mov esi, [r12])", 0x745640);
    const auto missionStart = std::find_if(missionHooks.begin(), missionHooks.end(),
                                           [&](const MidSite& site) { return site.rva == guardHooks[1].rva; });
    Check(missionStart != missionHooks.end() && missionStart->original == guardHooks[1].original &&
              guardHooks[1].displacedSize == guardHooks[1].original.size(),
          "the mission start site is the mission phase's, and alone it runs its imul unchanged", guardHooks[1].rva);
    // 959F80: () -> the WEAPONTABLE's rows, e23f0 on [20B2890]+0x130.
    Check(Bytes(0x959F80, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D}) && Bytes(0x959F8B, {0x48, 0x81, 0xC1, 0x30, 0x01, 0x00, 0x00}) &&
              CallTargets(image.At(0x959F92, 5), 0x959F92, 0xE23F0),
          "959F80 returns the weapon table's rows from GameStatus+0x130", 0x959F80);
    allHooks.push_back(guardHooks[0]);
    // 8Player MOD (hostmode.cpp): the sites it replaces, and the game functions it calls from the menu frame.
    const auto hostHooks = HostModeHooks();
    Check(hostHooks.size() == 12, "host mode hook table size");
    for (const auto& hook : hostHooks) {
        const Patch verify{hook.name, hook.rva, hook.original, hook.original};
        Check(Matches(image.At(hook.rva, hook.original.size()), verify), hook.name, hook.rva);
        // Either the handler replaces the whole site, or the site's instructions all run unchanged after it.
        Check(hook.original.size() >= 5 && hook.displacedOffset == 0 &&
                  (hook.displacedSize == 0 || hook.displacedSize == hook.original.size()),
              "host mode handlers replace the whole site or run all of it", hook.rva);
        // 74AC69/74/7F/8A: movabs rax, (high << 32) | 0x91, the vanilla range of one room kind.
        if (hook.original.size() == 10 && hook.original[0] == 0x48 && hook.original[1] == 0xB8) {
            const std::uint64_t range = Operand(hook.original, 2, 8);
            const auto high = static_cast<std::uint32_t>(range >> 32);
            Check(static_cast<std::uint32_t>(range) == 0x91 &&
                      high >= 0x91 && high <= 0x94,
                  "search range site is movabs rax, vanilla [0x91, high]", hook.rva);
        }
        if (hook.original[0] == 0xBB) {
            const auto vanilla = static_cast<std::uint64_t>(hook.original[1]);
            const std::uint64_t mirrored = 2 * kSearchTypeCenter - vanilla;
            Check(vanilla >= 0x91 && vanilla <= 0x94 && mirrored >= 2 * kSearchTypeCenter - 0x94 && mirrored <= 2 * kSearchTypeCenter - 0x91,
                  "published SEARCH_TYPE is the vanilla value mirrored around the centre", hook.rva);
            Check((mirrored & ~0xFull) != 0x90, "vanilla join check refuses the mirrored value", hook.rva);
            Check((mirrored & ~0x1Full) != 0x80 && (mirrored < 0x8C || mirrored > 0x94), "MultiSlot 0.2-0.4.1 neither searches nor accepts it", hook.rva);
            Check(mirrored < 0x7C || mirrored > 0x7F, "MultiSlot 0.4.2-0.4.3 (join check 0x7C..0x7F) refuses it", hook.rva);
            Check(mirrored < 0x74 || mirrored > 0x77, "MultiSlot 0.5.0-1.0.0 (join check 0x74..0x77) refuses it", hook.rva);
            Check(mirrored < 0x6C || mirrored > 0x6F, "MultiSlot 1.1.0-1.1.1 (join check 0x6C..0x6F) refuses it", hook.rva);
            Check(mirrored < 0x64 || mirrored > 0x67, "MultiSlot 1.2.0 (join check 0x64..0x67) refuses it", hook.rva);
            Check(mirrored < 0x5C || mirrored > 0x5F, "MultiSlot 1.2.1-1.2.5 (join check 0x5C..0x5F) refuses it", hook.rva);
            // 2.3.0's family lies below every 2.2 room-size family (8p 0x54.., 10p 0x48.., 12p 0x40.., 16p 0x38..,
            // 24p 0x28.., 32p 0x20..0x23): none of those builds lists or accepts it, nor it theirs.
            Check(mirrored < 0x20, "no 2.2 room-size build lists or accepts a 2.3.0 room", hook.rva);
            Check((mirrored < 0x5D || mirrored > 0x93) && (mirrored < 0x5E || mirrored > 0x92) && (mirrored < 0x5C || mirrored > 0x94) &&
                      (mirrored < 0x5F || mirrored > 0x91),
                  "MultiSlot 1.2.1-1.2.5 searches (and earlier ones) do not list it", hook.rva);
            Check(mirrored >= 2 * (kSearchTypeCenter - 4) - 0x94, "the published values stay inside the search range of the next family", hook.rva);
        }
    }
    Check(CallTargets(image.At(0x742B32, 5), 0x742B32, 0x12B3160), "the create function with the capacity site creates the lobby", 0x742B32);
    // The Steam lobby step: 7435F7's r8d is cMaxMembers of 7812B0, which calls ISteamMatchmaking::CreateLobby
    // (vtable +0x68) on the SteamMatchMaking009 interface (context 1F76000, initialiser 787B60).
    Check(CallTargets(image.At(0x74360C, 5), 0x74360C, 0x7812B0), "the Steam lobby capacity feeds 7812B0", 0x74360C);
    const std::uint8_t createLobby[] = {0xFF, 0x50, 0x68};  // call [rax+0x68]
    Check(RipTarget(0x78130D, 7) == 0x1F76000 && std::memcmp(image.At(0x781341, 3), createLobby, 3) == 0 &&
              std::memcmp(image.At(0x78133E, 3), "\x44\x8B\xC5", 3) == 0,  // mov r8d, ebp (ebp = the caller's r8d)
          "7812B0 passes r8d to ISteamMatchmaking::CreateLobby", 0x781341);
    Check(SlotTargets(image.At(0x1F76000, 8), image.nt->OptionalHeader.ImageBase, 0x787B60) && RipTarget(0x787B71, 7) == 0x17EE480 &&
              std::memcmp(image.At(0x17EE480, 20), "SteamMatchMaking009", 20) == 0,
          "context 1F76000 is SteamMatchMaking009", 0x1F76000);
    Check(CallTargets(image.At(0x749C9B, 5), 0x749C9B, 0x12BFE20), "the update capacity feeds SetMaxMembers", 0x749C9B);
    Check(CallTargets(image.At(0x8C12EE, 5), 0x8C12EE, 0x84B490), "HUiMainFrame::OnUpdate starts with HUiLayout::OnUpdate", 0x8C12EE);
    Check(CallTargets(image.At(0x8C0A98, 5), 0x8C0A98, 0x839600), "MainFrame looks components up by name with 839600", 0x8C0A98);
    Check(CallTargets(image.At(0x8C1EE2, 5), 0x8C1EE2, 0x8859C0) && CallTargets(image.At(0x8C1EF5, 5), 0x8C1EF5, 0x863690),
          "MainFrame sets component text with 8859C0 + 863690", 0x8C1EE2);
    const std::uint8_t isRoomHost[] = {0x48, 0x8B, 0x0D};  // mov rcx, [g_Online]; jmp IsRoomHost
    Check(std::memcmp(image.At(0x70EEB0, 3), isRoomHost, 3) == 0 && RipTarget(0x70EEB0, 7) == 0x20B2AC8 &&
              image.At(0x70EEB7, 1)[0] == 0xE9,
          "IsRoomHost native reaches 787370 with the online manager", 0x70EEB0);
    Check(RipTarget(0x7859AA, 7) == 0x20B2AC0, "the session check reads the holder at 20B2AC0", 0x7859AA);
    const std::uint8_t sessionField[] = {0x48, 0x83, 0xB8, 0xC0, 0x00, 0x00, 0x00, 0x00};  // cmp qword [rax+0xC0], 0
    Check(std::memcmp(image.At(0x7859D3, 8), sessionField, 8) == 0, "session present = [holder-0x98+0xC0] != 0", 0x7859D3);

    auto all = guest;
    all.insert(all.end(), sessions.begin(), sessions.end());
    all.insert(all.end(), missionPatches.begin(), missionPatches.end());
    all.insert(all.end(), hudPatches.begin(), hudPatches.end());
    allHooks.insert(allHooks.end(), hostHooks.begin(), hostHooks.end());
    // The colour hooks and the wrap are never installed together, but both have to keep clear of everything else.
    allHooks.insert(allHooks.end(), hudHooks.begin(), hudHooks.end());
    allHooks.insert(allHooks.end(), hudWrap.begin(), hudWrap.end());
    auto spans = WriteSpans(all, allCalls, allHooks);
    // The remote-player correction factor is built at load (its operand depends on where the constant
    // lands), so it is not in the tables above - but it is still a write into EDF.dll and has to keep
    // clear of EDF6VR like every other one. The package ships both mods together.
    spans.insert({kSmoothingSite, kSmoothingSite + 7});
    CheckNoOverlap(spans);
    CheckClearOfVr(spans);

    Check(CapacityFromInfo(2, 3, 5) == 5, "capacity: consistent 5-player room");
    Check(CapacityFromInfo(4, 0, 4) == 4, "capacity: full vanilla room");
    Check(CapacityFromInfo(0, 8, 8) == 8, "capacity: empty 8-player room");
    Check(CapacityFromInfo(3, 3, 5) == 0, "capacity: members + available != max is rejected");
    Check(CapacityFromInfo(0, 0, 0) == 0, "capacity: zero max is rejected");
    Check(CapacityFromInfo(1, 70, 71) == 0, "capacity: more than EOS's 64 is rejected");
    // Rooms larger than an EOS lobby (kRoomSizeKey): a full-sized lobby stands for the room's published size.
    Check(CapacityFromInfo(64, 0, 64, 1024) == 1024 && CapacityFromInfo(10, 54, 64, 200) == 200,
          "capacity: a 64-member lobby with a larger published room size is that room");
    Check(CapacityFromInfo(3, 2, 5, 1024) == 5, "capacity: a published size counts only for a full-sized lobby");
    Check(CapacityFromInfo(64, 0, 64, 0) == 64 && CapacityFromInfo(64, 0, 64, 32) == 64 &&
              CapacityFromInfo(64, 0, 64, kMaxPlayers + 1) == 64,
          "capacity: a published size below the lobby's, or past this build's, is ignored");
    Check(CapacityFromInfo(1, 70, 71, 1024) == 0, "capacity: an inconsistent lobby stays rejected with a room size");
    // Members beyond Epic's lobby (docs/net-re/roomsize.md §4): the room this machine is in reads them through the
    // game's member count; a room list entry has its owner's published count.
    const auto same = [](RoomCount a, std::uint32_t members, std::uint32_t capacity) {
        return a.members == members && a.capacity == capacity;
    };
    Check(same(RoomCountFromInfo(70, 0, 64, 200), 70, 200) && same(RoomCountFromInfo(63, 4, 64, 200), 63, 200),
          "count: our own room counts its members beyond Epic's lobby, full or not");
    Check(same(RoomCountFromInfo(64, 0, 64, 200, 150), 150, 200) && same(RoomCountFromInfo(70, 0, 64, 200, 65), 70, 200),
          "count: a room list entry shows the owner's published members, never fewer than are read");
    Check(same(RoomCountFromInfo(64, 0, 64, 200, 500), 64, 200), "count: a published count past the room's size is ignored");
    Check(RoomCountFromInfo(60, 0, 64, 200).capacity == 0, "count: fewer than Epic lists is inconsistent");
    Check(RoomCountFromInfo(9, 0, 8).capacity == 0 && same(RoomCountFromInfo(8, 0, 8, 0, 20), 8, 8),
          "count: a room EOS holds whole counts exactly Epic's members, published counts aside");
    // The host's HIDDEN check: a room of 200 with 70 in it is not full, one of 200 with 200 is.
    Check(RoomCountFromInfo(70, 0, 64, 200).members < RoomCountFromInfo(70, 0, 64, 200).capacity &&
              RoomCountFromInfo(200, 0, 64, 200).members >= RoomCountFromInfo(200, 0, 64, 200).capacity,
          "count: a room beyond Epic's lobby is full at its own size");

    // Which slot a member takes: the host's when known, in the table and empty here; else the game's first empty one.
    Check(ChooseUserSlot(1, 3, 1024, true) == 3 && ChooseUserSlot(1, 3, 1024, false) == 1 && ChooseUserSlot(1, -1, 1024, true) == 1 &&
              ChooseUserSlot(1, 1024, 1024, true) == 1 && ChooseUserSlot(0, 0, 1024, true) == 0,
          "a member takes the host's slot when it is empty here, the first empty one otherwise");
    {
        // The Add handler on a stand-in of Users::Add's registers: slots 0 and 2 taken, the host has "D" in 3.
        std::uint64_t slots[8]{};  // 4 shared_ptr {object, control}
        slots[0] = 1;
        slots[4] = 1;
        const char* puid = "D";
        std::int32_t index = 1;
        SetUserSlotSources([](const std::string& m) { return m == "D" ? 3 : m == "E" ? 2 : -1; },
                           [](const void* id, char* out, std::size_t size) -> const char* {
                               strcpy_s(out, size, static_cast<const char*>(id));
                               return out;
                           });
        CpuContext add{};
        add.rcx = 1;  // the first empty slot
        add.r8 = 4;
        add.r9 = reinterpret_cast<std::uintptr_t>(slots);
        add.r13 = 0x5000;
        add.rbx = reinterpret_cast<std::uintptr_t>(&puid);
        add.rbp = reinterpret_cast<std::uintptr_t>(&index);
        UserSlotHookHandler(0x12B806E)(&add);
        Check(static_cast<std::int32_t>(add.rcx) == 3 && index == 3, "Users::Add puts D in the host's slot 3");
        const char* taken = "E";  // the host's slot 2 is taken here: the game's choice stands
        add.rcx = 1;
        index = 1;
        add.rbx = reinterpret_cast<std::uintptr_t>(&taken);
        UserSlotHookHandler(0x12B806E)(&add);
        Check(static_cast<std::int32_t>(add.rcx) == 1 && index == 1, "a host slot taken here leaves the first empty one");
        Check((GameSlotTable() == std::vector<std::string>{"", "E", "", "D"}), "the slot table follows the adds");
        CpuContext remove{};
        remove.rbp = 3;
        remove.r15 = 0x5000;
        UserSlotHookHandler(0x12B8A24)(&remove);
        Check((GameSlotTable() == std::vector<std::string>{"", "E"}), "a removed member's slot is empty, nothing moves up");
        add.r13 = 0x6000;  // another room's Users
        UserSlotHookHandler(0x12B806E)(&add);
        Check((GameSlotTable() == std::vector<std::string>{"", "E"}), "another room starts its own table");
        // Native pre-spawn identity lookup distinguishes mission index from lobby slot.
        std::uint8_t users[2][0xA0]{};
        const char* ids[2]{"A", "B"};
        int missionIndices[2]{7, 3};
        std::uint64_t nativeSlots[4]{reinterpret_cast<std::uintptr_t>(users[0]), 0,
                                     reinterpret_cast<std::uintptr_t>(users[1]), 0};
        std::uint64_t nativeUsers[2]{reinterpret_cast<std::uintptr_t>(nativeSlots),
                                     reinterpret_cast<std::uintptr_t>(nativeSlots + 4)};
        for (int n = 0; n < 2; ++n) {
            std::memcpy(users[n] + 0x18, &ids[n], sizeof(ids[n]));
            std::memcpy(users[n] + 0x48, &missionIndices[n], sizeof(int));
            add.r13 = reinterpret_cast<std::uintptr_t>(nativeUsers);
            add.rcx = static_cast<std::uint64_t>(n); add.r8 = 2;
            add.r9 = reinterpret_cast<std::uintptr_t>(nativeSlots);
            add.rbx = reinterpret_cast<std::uintptr_t>(&ids[n]);
            UserSlotHookHandler(0x12B806E)(&add);
        }
        char resolved[65]{};
        Check(ResolveMissionPlayerPuid(7, resolved, sizeof(resolved)) && std::string(resolved) == "A",
              "mission index 7 maps to slot 0's authenticated identity");
        Check(ResolveMissionPlayerPuid(3, resolved, sizeof(resolved)) && std::string(resolved) == "B",
              "mission index 3 maps to slot 1");
        Check(!ResolveMissionPlayerPuid(0, resolved, sizeof(resolved)) && !resolved[0], "lobby slot 0 is not mission index 0");
        std::memcpy(users[1] + 0x48, &missionIndices[0], sizeof(int));
        Check(!ResolveMissionPlayerPuid(7, resolved, sizeof(resolved)), "duplicate mission indices fail closed");
        std::memcpy(users[1] + 0x48, &missionIndices[1], sizeof(int));
        remove.r15 = reinterpret_cast<std::uintptr_t>(nativeUsers); remove.rbp = 0;
        UserSlotHookHandler(0x12B8A24)(&remove);
        Check(!ResolveMissionPlayerPuid(7, resolved, sizeof(resolved)), "removed member cannot resolve from stale native pointer");
        ++nativeUsers[1];
        Check(!ResolveMissionPlayerPuid(3, resolved, sizeof(resolved)), "misaligned native vector is rejected");
        SetUserSlotSources(nullptr, nullptr);
    }

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("patch tables verified against %s\n", argv[1]);
    return 0;
}
