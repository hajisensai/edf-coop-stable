// The files the plugin keeps under Mods (modfile.h), in a scratch folder: where each goes, writing, updating,
// leaving another mod's file alone, and removing only our own.
//   ModFileTests work-folder
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/modfile.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const char* what, const wchar_t* file = L"") {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %ls: %s\n", file, what);
    }
}

std::vector<unsigned char> ReadBytes(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes.data()),
                                                                  static_cast<std::streamsize>(bytes.size()));
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::vector<unsigned char> Embedded(const ModFile& file) { return std::vector<unsigned char>(file.bytes, file.bytes + file.size); }

bool Holds(const std::vector<unsigned char>& data, const wchar_t* text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    return std::search(data.begin(), data.end(), bytes, bytes + std::wcslen(text) * sizeof(wchar_t)) != data.end();
}

// Install, update, remove and leave alone: the same for every file.
void FileHandling(const ModFile& file, const std::wstring& root) {
    const wchar_t* name = file.path;
    const std::wstring mods = root + L"\\Mods";
    const std::wstring target = mods + L"\\" + file.path;
    const std::wstring folder = target.substr(0, target.rfind(L'\\'));
    const std::wstring temp = target + L".multislot-tmp";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(mods.c_str(), nullptr);
    DeleteFileW(target.c_str());
    DeleteFileW(temp.c_str());
    RemoveDirectoryW(folder.c_str());
    const auto embedded = Embedded(file);
    const auto isEmbedded = [&] { return Exists(target) && ReadBytes(target) == embedded; };

#ifdef MULTISLOT_PLACEHOLDER_ASSETS
    // A CI build (MULTISLOT_CI) may embed a placeholder: the file handling below still runs on it, but only the
    // real asset can be in the known list.
    std::printf("SKIPPED: %ls: the known-list check needs the real asset (this build may have a placeholder)\n", name);
#else
    Check(OurModFile(file, embedded.data(), embedded.size()), "the current file is in the known list (add it when the asset changes)", name);
#endif
    auto changed = embedded;
    changed[changed.size() / 2] ^= 0x5A;
    Check(!OurModFile(file, changed.data(), changed.size()), "a changed file is not ours", name);

    // Path: only for a plugin in Mods\Plugins.
    wchar_t path[MAX_PATH]{};
    Check(ModFilePath(file, L"C:\\Games\\EDF6\\Mods\\Plugins\\EDF6Coop.dll", path, MAX_PATH) &&
              std::wstring(path) == std::wstring(L"C:\\Games\\EDF6\\Mods\\") + file.path,
          "Mods\\Plugins\\x.dll -> Mods\\<path>", name);
    Check(ModFilePath(file, L"C:\\EDF6\\mods\\plugins\\EDF6Coop.dll", path, MAX_PATH), "folder names in any case", name);
    Check(!ModFilePath(file, L"C:\\Games\\EDF6\\EDF6Coop.dll", path, MAX_PATH), "a plugin outside Mods\\Plugins gets no file", name);
    Check(!ModFilePath(file, L"C:\\Games\\EDF6\\Other\\Plugins\\EDF6Coop.dll", path, MAX_PATH), "Plugins must be inside Mods", name);
    Check(!ModFilePath(file, L"EDF6Coop.dll", path, MAX_PATH), "a bare file name gets no file", name);
    wchar_t tiny[8]{};
    Check(!ModFilePath(file, L"C:\\Games\\EDF6\\Mods\\Plugins\\EDF6Coop.dll", tiny, 8), "a short buffer is refused, not overrun", name);
    std::wstring longPath = L"C:\\";
    while (longPath.size() < MAX_PATH + 20) longPath += L"folder\\";
    longPath += L"Mods\\Plugins\\EDF6Coop.dll";
    Check(!ModFilePath(file, longPath.c_str(), path, MAX_PATH), "an over-long plugin path is refused without ending the process", name);

    // Removing when nothing is there.
    Check(RemoveModFile(file, target.c_str()) == FileRemoval::Absent, "remove: nothing to remove", name);

    // Install: creates the folder and writes the file; a second run finds it current.
    Check(InstallModFile(file, target.c_str()) == FileInstall::Written && isEmbedded(), "install writes the file", name);
    Check(!Exists(temp), "no temporary file is left behind", name);
    Check(InstallModFile(file, target.c_str()) == FileInstall::Current && isEmbedded(), "install again: already current", name);

    // Remove: deletes our file and the folder it created.
    Check(RemoveModFile(file, target.c_str()) == FileRemoval::Removed && !Exists(target) && !Exists(folder),
          "remove deletes our file and the empty folder", name);

    // Another mod's file is never replaced or deleted.
    CreateDirectoryW(folder.c_str(), nullptr);
    WriteBytes(target, changed);
    Check(InstallModFile(file, target.c_str()) == FileInstall::Foreign && ReadBytes(target) == changed, "install leaves another mod's file alone", name);
    Check(RemoveModFile(file, target.c_str()) == FileRemoval::Foreign && ReadBytes(target) == changed, "remove leaves another mod's file alone", name);
    DeleteFileW(target.c_str());

    // A folder that holds other files stays.
    const std::wstring other = folder + L"\\OTHER_MOD.BIN";
    WriteBytes(other, {1, 2, 3});
    Check(InstallModFile(file, target.c_str()) == FileInstall::Written, "install next to other files", name);
    Check(RemoveModFile(file, target.c_str()) == FileRemoval::Removed && Exists(other) && Exists(folder), "remove keeps the folder and the other files", name);
    DeleteFileW(other.c_str());

    // A temporary file left by an interrupted write is cleaned up.
    WriteBytes(temp, {9});
    Check(InstallModFile(file, target.c_str()) == FileInstall::Written && isEmbedded(), "install over a stale temporary file", name);
    WriteBytes(temp, {9});
    Check(RemoveModFile(file, target.c_str()) == FileRemoval::Removed && !Exists(temp) && !Exists(folder), "remove cleans a stale temporary file", name);

    // A folder where the file should be cannot be read as a file: nothing is touched.
    CreateDirectoryW(folder.c_str(), nullptr);
    CreateDirectoryW(target.c_str(), nullptr);
    Check(InstallModFile(file, target.c_str()) == FileInstall::Failed, "install refuses when the name is a folder", name);
    Check(RemoveModFile(file, target.c_str()) == FileRemoval::Failed && Exists(target), "remove refuses when the name is a folder", name);
    RemoveDirectoryW(target.c_str());
    RemoveDirectoryW(folder.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: ModFileTests work-folder\n");
        return 2;
    }
    const std::wstring root = argv[1];

    // The embedded menu layout: an SGO with the label field.
    const auto layout = Embedded(MenuLayoutFile());
    Check(layout.size() > 16 && std::memcmp(layout.data(), "SGO\0", 4) == 0, "embedded layout is an SGO file");
    Check(Holds(layout, L"MSLabel"), "embedded layout names the MSLabel field");
    // The embedded HUD archive: an SSA archive that names the lamps the plugin points the HUD at.
    const auto hud = Embedded(HudArchiveFile());
    Check(hud.size() > 16 && std::memcmp(hud.data(), "SSA\0", 4) == 0, "embedded HUD archive is an SSA archive");
#ifndef MULTISLOT_PLACEHOLDER_ASSETS
    Check(Holds(hud, L"player_lamps.dds") && Holds(hud, L"player_lamp.dds") && Holds(hud, L"chat_Indigo.dds"),
          "embedded HUD archive holds the game's lamp, the plugin's lamps and the last colour's balloon");
#endif
    Check(std::wcscmp(MenuLayoutFile().path, L"UI\\LYT_MAINFRAME.SGO") == 0 &&
              std::wcscmp(HudArchiveFile().path, L"HUD\\ONLINEHUDTEXTURE.RAB") == 0,
          "the files go where the game reads them from (EDFModLoader maps Mods\\<path> over the archived <path>)");

    FileHandling(MenuLayoutFile(), root);
    FileHandling(HudArchiveFile(), root);

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("mod file handling verified\n");
    return 0;
}
