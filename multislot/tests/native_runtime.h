#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstring>

namespace native_test {

// The supported PE is privately mapped with DONT_RESOLVE_DLL_REFERENCES. Copy its metadata instead of
// aliasing byte storage as C++ structures; no game entrypoint or non-CRT dependency is initialized.
template <typename T>
T Read(const std::byte* image, std::size_t offset) {
    T value{};
    std::memcpy(&value, image + offset, sizeof(value));
    return value;
}

inline const char* Text(const std::byte* address) {
    return static_cast<const char*>(static_cast<const void*>(address));
}

inline bool ResolveCrtTable(std::byte* image, const IMAGE_IMPORT_DESCRIPTOR& descriptor) {
    const auto* name = Text(image + descriptor.Name);
    if (_strnicmp(name, "VCRUNTIME", 9) && _strnicmp(name, "api-ms-win-crt-", 15)) return true;
    const HMODULE runtime = LoadLibraryA(name);
    if (!runtime) return false;
    for (std::size_t offset = 0;; offset += sizeof(IMAGE_THUNK_DATA64)) {
        const auto thunk = Read<IMAGE_THUNK_DATA64>(image, descriptor.OriginalFirstThunk + offset);
        if (!thunk.u1.AddressOfData) return true;
        if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) continue;
        const auto* symbol = Text(image + thunk.u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name));
        const FARPROC proc = GetProcAddress(runtime, symbol);
        if (!proc) return false;
        auto* slot = image + descriptor.FirstThunk + offset;
        DWORD protection = 0;
        if (!VirtualProtect(slot, sizeof(proc), PAGE_READWRITE, &protection)) return false;
        std::memcpy(slot, &proc, sizeof(proc));
        if (!VirtualProtect(slot, sizeof(proc), protection, &protection)) return false;
    }
}

inline bool ResolveCrtImports(HMODULE imageModule) {
    auto* image = static_cast<std::byte*>(static_cast<void*>(imageModule));
    const auto dos = Read<IMAGE_DOS_HEADER>(image, 0);
    const auto nt = Read<IMAGE_NT_HEADERS64>(image, dos.e_lfanew);
    const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (std::size_t offset = directory.VirtualAddress;; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const auto descriptor = Read<IMAGE_IMPORT_DESCRIPTOR>(image, offset);
        if (!descriptor.Name) return true;
        if (!ResolveCrtTable(image, descriptor)) return false;
    }
}

}  // namespace native_test
