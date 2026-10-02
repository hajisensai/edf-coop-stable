#pragma once
#include <cstddef>
#include <cstdint>

namespace multislot {

// Files the plugin keeps under Mods while it is active, which EDFModLoader loads in place of the game's archived
// ones. Each is built from the game's own file by a tool and embedded in the plugin. The plugin writes them after
// patching and removes them again (when they are ours) if it is disabled, the game build is not supported or the
// patches that use them are not applied, so the game's own files come back. Another mod's file under the same
// name is never replaced or deleted.
struct KnownFile {
    std::size_t size;
    std::uint64_t fnv1a;
};

struct ModFile {
    const wchar_t* path;  // under Mods, e.g. L"UI\\LYT_MAINFRAME.SGO"
    const unsigned char* bytes;
    std::size_t size;
    // Every version a release has written, so a later version can update it and a disabled plugin can remove
    // it. When the asset changes, add its entry and keep the old ones (a test checks).
    const KnownFile* known;
    std::size_t knownCount;
};

// Mods\UI\LYT_MAINFRAME.SGO: the game's menu frame layout (Root.cpk UI/LYT_MAINFRAME.SGO) plus one text field,
// MSLabel, that shows the 8Player MOD label (tools/make_menu_label.py).
const ModFile& MenuLayoutFile();
// Mods\HUD\ONLINEHUDTEXTURE.RAB: the game's online HUD textures (Root.cpk HUD/ONLINEHUDTEXTURE.RAB), every one
// as the game has it, plus a lamp and a chat balloon for every colour of hudcolours.h (tools/make_hud_colours.py).
// Only added files, so a game without the plugin still shows its own HUD with it.
const ModFile& HudArchiveFile();

// True for a file this or an earlier version of the plugin wrote.
bool OurModFile(const ModFile& file, const unsigned char* data, std::size_t size);

// <...>\Mods\<file.path> for a plugin at <...>\Mods\Plugins\<name>.dll; false elsewhere.
bool ModFilePath(const ModFile& file, const wchar_t* pluginPath, wchar_t* out, std::size_t outChars);

enum class FileInstall { Written, Current, Updated, Foreign, Failed };
// Writes the file unless the same file is there already; a file from another mod is left alone.
FileInstall InstallModFile(const ModFile& file, const wchar_t* path);

enum class FileRemoval { Removed, Absent, Foreign, Failed };
// Deletes the file only when it is one of ours (and its folder, if that leaves it empty).
FileRemoval RemoveModFile(const ModFile& file, const wchar_t* path);

}  // namespace multislot
