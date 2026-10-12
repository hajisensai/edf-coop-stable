// Shared vtable slots without the game (SlotNext, patches.h): an image the size of EDF.dll holding only the
// functions' heads and the slots. Another plugin may have chained its hook onto HUiRoom::OnUpdate (EDF6VehicleCrew
// does) or HUiMainFrame::OnUpdate before EDF6Coop loads; whether the game is the known build is told by the code.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../src/patches.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const char* what, const char* slot) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s: %s\n", slot, what);
    }
}

std::uint64_t ForeignOnUpdate(void*, void*) { return 0; }
int foreignData = 0;  // in this exe's data section: a loaded module, but not code

std::uint64_t Address(const void* at) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at)); }

// Memory no module owns: the heap's (read/write) and a JIT-like executable block.
struct Loose {
    void* data = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* code = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READ);
    ~Loose() {
        if (data) VirtualFree(data, 0, MEM_RELEASE);
        if (code) VirtualFree(code, 0, MEM_RELEASE);
    }
};

struct Image {
    unsigned char* base = nullptr;
    Image() { base = static_cast<unsigned char*>(VirtualAlloc(nullptr, kImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)); }
    ~Image() {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
    }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base)); }
    void Set(const PointerSlot& slot, std::uint64_t value) { std::memcpy(base + slot.rva, &value, sizeof(value)); }
    std::uint64_t Next(const PointerSlot& slot) const { return SlotNext(base + slot.rva, base, Address(), kImageSize, slot); }
};

void CheckShared(Image& image, const PointerSlot& slot) {
    Check(!slot.code.empty(), "is shared (has its function's code)", slot.name);
    std::memcpy(image.base + slot.target, slot.code.data(), slot.code.size());
    const std::uint64_t original = image.Address() + slot.target;
    const auto foreign = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&ForeignOnUpdate));

    image.Set(slot, original);
    Check(image.Next(slot) == original, "an untouched slot: the hook continues with the game's function", slot.name);
    image.Set(slot, foreign);
    Check(image.Next(slot) == foreign, "another plugin's hook in the slot: ours chains onto it", slot.name);
    image.Set(slot, 0);
    Check(image.Next(slot) == 0, "an empty slot is refused", slot.name);
    image.Set(slot, image.Address() + slot.target + 0x40);
    Check(image.Next(slot) == 0, "a slot pointing elsewhere into EDF.dll is refused (not this vtable)", slot.name);

    // A foreign pointer is only taken when it is code in a loaded module (another plugin's hook).
    Loose loose;
    Check(loose.data && loose.code, "memory for the loose blocks", slot.name);
    image.Set(slot, Address(loose.data));
    Check(image.Next(slot) == 0, "a pointer into heap memory is refused", slot.name);
    image.Set(slot, Address(loose.code));
    Check(image.Next(slot) == 0, "executable memory outside every module is refused", slot.name);
    image.Set(slot, Address(&foreignData));
    Check(image.Next(slot) == 0, "a loaded module's data (not code) is refused", slot.name);
    VirtualFree(loose.code, 0, MEM_RELEASE);
    image.Set(slot, Address(loose.code));
    const void* freed = loose.code;
    loose.code = nullptr;
    Check(image.Next(slot) == 0 && freed, "a freed block is refused", slot.name);

    // The function's entry changed: a game update, or another plugin's inline detour. The slot still holding the
    // game's function is accepted exactly as before shared slots (the code is not read then); a foreign pointer,
    // which leaves only the code to tell the build, is refused.
    image.base[slot.target] ^= 0x01;
    image.Set(slot, original);
    Check(image.Next(slot) == original, "changed code with an untouched slot is accepted, as it always was", slot.name);
    image.Set(slot, foreign);
    Check(image.Next(slot) == 0, "changed code with another plugin's hook is refused", slot.name);
    image.base[slot.target] ^= 0x01;
    image.base[slot.target + slot.code.size() - 1] ^= 0x01;
    image.Set(slot, foreign);
    Check(image.Next(slot) == 0, "a change at the head's last byte is seen too", slot.name);
    image.base[slot.target + slot.code.size() - 1] ^= 0x01;
    Check(image.Next(slot) == foreign, "the code restored is accepted again", slot.name);

    // The function's head is read inside the image only (for a foreign pointer; the game's function needs no read).
    Check(SlotNext(image.base + slot.rva, image.base, image.Address(), slot.target + slot.code.size() - 1, slot) == 0,
          "a head past the image's end is refused", slot.name);
    image.Set(slot, original);
    Check(SlotNext(image.base + slot.rva, nullptr, image.Address(), kImageSize, slot) == original,
          "the game's function is accepted without reading the code", slot.name);
}

void CheckExclusive(Image& image, const PointerSlot& slot) {
    Check(slot.code.empty(), "is not shared", slot.name);
    const std::uint64_t original = image.Address() + slot.target;
    image.Set(slot, original);
    Check(image.Next(slot) == original, "holding the game's function is accepted", slot.name);
    image.Set(slot, Address(reinterpret_cast<const void*>(&ForeignOnUpdate)));
    Check(image.Next(slot) == 0, "anything else is refused, as before", slot.name);
    image.Set(slot, 0);
    Check(image.Next(slot) == 0, "an empty slot is refused", slot.name);
}

}  // namespace

int main() {
    Image image;
    if (!image.base) {
        std::printf("FAIL: no memory for the image\n");
        return 1;
    }
    CheckShared(image, RoomViewSlot());
    CheckShared(image, MainFrameSlot());
    for (const auto& slot : MissionSlots()) CheckExclusive(image, slot);
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("shared vtable slots verified\n");
    return 0;
}
