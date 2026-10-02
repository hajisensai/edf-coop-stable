#include "modfile.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <vector>

#include "mod_assets.h"

namespace multislot {
namespace {

// Larger files at one of these names are another mod's, and are not read whole.
constexpr std::size_t kMaxModFileBytes = 16 << 20;
constexpr const wchar_t kTempSuffix[] = L".multislot-tmp";

constexpr KnownFile kKnownLayouts[] = {
    {7098, 0xDA755A27A8E88468ull},  // 0.6.0, 1.0.0, 1.1.0, 1.1.1, 1.2.0-1.5.3
};
constexpr KnownFile kKnownHudArchives[] = {
    {205334, 0x1CD5561D661F20BBull},  // 2.1.0
};

const ModFile kMenuLayout{L"UI\\LYT_MAINFRAME.SGO", kMenuLayoutBytes, sizeof(kMenuLayoutBytes), kKnownLayouts,
                          std::size(kKnownLayouts)};
const ModFile kHudArchive{L"HUD\\ONLINEHUDTEXTURE.RAB", kHudArchiveBytes, sizeof(kHudArchiveBytes), kKnownHudArchives,
                          std::size(kKnownHudArchives)};

std::uint64_t Fnv1a(const unsigned char* data, std::size_t size) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

// No *_s string functions here: on overflow they end the process through the invalid parameter handler.
bool CopyText(wchar_t* out, std::size_t chars, const wchar_t* text) {
    const std::size_t length = wcslen(text);
    if (length >= chars) return false;
    wmemcpy(out, text, length + 1);
    return true;
}

bool AppendText(wchar_t* out, std::size_t chars, const wchar_t* text) {
    const std::size_t used = wcsnlen(out, chars);
    return used < chars && CopyText(out + used, chars - used, text);
}

enum class Existing { Absent, Current, Ours, Foreign, Unreadable };

Existing Inspect(const ModFile& file, const wchar_t* path) {
    const HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Existing::Absent : Existing::Unreadable;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) != 0;
    const bool small = ok && size.QuadPart >= 0 && static_cast<unsigned long long>(size.QuadPart) <= kMaxModFileBytes;
    std::vector<unsigned char> data;
    if (small && size.QuadPart > 0) {
        data.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(handle, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size();
    }
    CloseHandle(handle);
    if (!ok) return Existing::Unreadable;
    if (!small) return Existing::Foreign;
    if (data.size() == file.size && std::memcmp(data.data(), file.bytes, data.size()) == 0) return Existing::Current;
    return OurModFile(file, data.data(), data.size()) ? Existing::Ours : Existing::Foreign;
}

bool TempPath(wchar_t* out, const wchar_t* path) { return CopyText(out, MAX_PATH, path) && AppendText(out, MAX_PATH, kTempSuffix); }

bool Folder(wchar_t* out, const wchar_t* path) {
    if (!CopyText(out, MAX_PATH, path)) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    *slash = 0;
    return true;
}

// Written next to the target and moved over it, so the game never reads half a file.
bool WriteModFile(const ModFile& file, const wchar_t* path) {
    wchar_t temp[MAX_PATH]{};
    if (!TempPath(temp, path)) return false;
    const HANDLE handle = CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(handle, file.bytes, static_cast<DWORD>(file.size), &written, nullptr) && written == file.size &&
                    FlushFileBuffers(handle);
    CloseHandle(handle);
    if (ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    const DWORD error = GetLastError();
    DeleteFileW(temp);
    SetLastError(error);
    return false;
}

}  // namespace

const ModFile& MenuLayoutFile() { return kMenuLayout; }
const ModFile& HudArchiveFile() { return kHudArchive; }

bool OurModFile(const ModFile& file, const unsigned char* data, std::size_t size) {
    if (!data) return false;
    const std::uint64_t hash = Fnv1a(data, size);
    for (std::size_t i = 0; i < file.knownCount; ++i)
        if (file.known[i].size == size && file.known[i].fnv1a == hash) return true;
    return false;
}

bool ModFilePath(const ModFile& file, const wchar_t* pluginPath, wchar_t* out, std::size_t outChars) {
    wchar_t buffer[MAX_PATH]{};
    if (!pluginPath || !out || !CopyText(buffer, MAX_PATH, pluginPath)) return false;
    wchar_t* name = wcsrchr(buffer, L'\\');
    if (!name) return false;
    *name = 0;
    wchar_t* plugins = wcsrchr(buffer, L'\\');
    if (!plugins || _wcsicmp(plugins + 1, L"Plugins") != 0) return false;
    *plugins = 0;
    const wchar_t* mods = wcsrchr(buffer, L'\\');
    if (!mods || _wcsicmp(mods + 1, L"Mods") != 0) return false;
    return AppendText(buffer, MAX_PATH, L"\\") && AppendText(buffer, MAX_PATH, file.path) && CopyText(out, outChars, buffer);
}

FileInstall InstallModFile(const ModFile& file, const wchar_t* path) {
    const Existing existing = Inspect(file, path);
    if (existing == Existing::Current) return FileInstall::Current;
    if (existing == Existing::Foreign) return FileInstall::Foreign;
    if (existing == Existing::Unreadable) return FileInstall::Failed;
    wchar_t folder[MAX_PATH]{};
    if (!Folder(folder, path)) return FileInstall::Failed;
    if (!CreateDirectoryW(folder, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return FileInstall::Failed;
    if (!WriteModFile(file, path)) return FileInstall::Failed;
    return existing == Existing::Ours ? FileInstall::Updated : FileInstall::Written;
}

FileRemoval RemoveModFile(const ModFile& file, const wchar_t* path) {
    wchar_t temp[MAX_PATH]{};
    if (TempPath(temp, path)) DeleteFileW(temp);  // left behind only if a write was interrupted
    const Existing existing = Inspect(file, path);
    if (existing == Existing::Absent) return FileRemoval::Absent;
    if (existing == Existing::Foreign) return FileRemoval::Foreign;
    if (existing == Existing::Unreadable || !DeleteFileW(path)) return FileRemoval::Failed;
    // The folder goes too when the file was all it held (RemoveDirectory refuses a folder with files).
    wchar_t folder[MAX_PATH]{};
    if (Folder(folder, path)) {
        const DWORD attributes = GetFileAttributesW(folder);
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveDirectoryW(folder);
    }
    return FileRemoval::Removed;
}

}  // namespace multislot
