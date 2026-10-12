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

    // A game update: the function's code is not this build's, whoever holds the slot.
    image.base[slot.target + slot.code.size() - 1] ^= 0x01;
    image.Set(slot, original);
    Check(image.Next(slot) == 0, "changed code with an untouched slot is refused", slot.name);
    image.Set(slot, foreign);
    Check(image.Next(slot) == 0, "changed code with another plugin's hook is refused", slot.name);
    image.base[slot.target + slot.code.size() - 1] ^= 0x01;
    image.Set(slot, original);
    Check(image.Next(slot) == original, "the code restored is accepted again", slot.name);

    // The function's head is read inside the image only.
    Check(SlotNext(image.base + slot.rva, image.base, image.Address(), slot.target + slot.code.size() - 1, slot) == 0,
          "a head past the image's end is refused", slot.name);
}

void CheckExclusive(Image& image, const PointerSlot& slot) {
    Check(slot.code.empty(), "is not shared", slot.name);
    const std::uint64_t original = image.Address() + slot.target;
    image.Set(slot, original);
    Check(image.Next(slot) == original, "holding the game's function is accepted", slot.name);
    image.Set(slot, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&ForeignOnUpdate)));
    Check(image.Next(slot) == 0, "anything else is refused, as before", slot.name);
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
