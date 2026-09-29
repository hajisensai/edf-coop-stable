#include "iat.h"

#include <cstdint>
#include <cstring>

namespace dn {

void** findImportSlot(HMODULE module, const char* dll, const char* function) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;  // no name table: cannot match by name
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

bool patchImport(HMODULE module, const char* dll, const char* function, void* hook, void** original) {
    void** slot = findImportSlot(module, dll, function);
    if (!slot) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
    // Keep whatever is there (maybe another mod's hook) so calls chain through it. `original` is
    // written before the slot changes: a game thread may call the hook the instant it is installed.
    for (void* current = *slot;;) {
        *original = current;
        void* seen = InterlockedCompareExchangePointer(slot, hook, current);
        if (seen == current) break;
        current = seen;
    }
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

}  // namespace dn
