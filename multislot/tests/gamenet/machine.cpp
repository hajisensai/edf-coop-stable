// GameMachine.exe <role> <game folder> <work folder>
// Loads the game's EDF.dll from the game folder (only read), with fake_eos_net.cpp as EOSSDK-Win64-Shipping.dll (it
// sits next to this program, which the loader searches first), then work\Mods\Plugins\EDF6Coop.dll through
// EML6_Load as EDFModLoader does, then runs the role. The driver (gamenet_test.cpp) prepared the work folder and
// the network, and reads "RESULT" lines from this program's output.
#include "machine.h"

#include <cstring>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4201)
#include "PluginAPI.h"
#pragma warning(pop)
#include "net_shared.h"

namespace gamenet {

void* GameImport(const Machine& machine, const char* name) {
    const auto base = reinterpret_cast<const std::uint8_t*>(machine.game);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "EOSSDK-Win64-Shipping.dll")) continue;
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<void* const*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (!std::strcmp(reinterpret_cast<const char*>(byName->Name), name)) return *slots;
        }
    }
    return nullptr;
}

}  // namespace gamenet

namespace {

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out += static_cast<char>(c < 0x80 ? c : '?');
    return out;
}

// Prints the plugin's log, so a failing test shows what the plugin said.
void DumpLog(const gamenet::Machine& machine) {
    const HANDLE file = CreateFileW(machine.LogPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        gamenet::Result("log", "missing (error %lu)", GetLastError());
        return;
    }
    BY_HANDLE_FILE_INFORMATION about{};
    GetFileInformationByHandle(file, &about);
    std::vector<char> text((static_cast<std::size_t>(about.nFileSizeHigh) << 32 | about.nFileSizeLow) + 1);
    DWORD read = 0;
    ReadFile(file, text.data(), static_cast<DWORD>(text.size() - 1), &read, nullptr);
    CloseHandle(file);
    gamenet::Result("log", "%lu bytes", read);
    std::printf("---- %s: EDF6Coop.log ----\n%.*s---- end of log ----\n", machine.user.c_str(), static_cast<int>(read),
                text.data());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::printf("usage: GameMachine <role> <game folder> <work folder>\n");
        return 2;
    }
    gamenet::Machine machine;
    const std::string role = Narrow(argv[1]);
    const std::wstring gameFolder = argv[2];
    machine.work = argv[3];
    char user[64]{};
    GetEnvironmentVariableA(gamenet::kUserVariable, user, sizeof(user));
    machine.user = user;

    // The two DLLs of its own EDF.dll imports come from the game folder, loaded by their full paths so that nothing
    // else does: the folder also holds EDFModLoader's winmm.dll, which EDF.dll's winmm import would otherwise find
    // there (and which then runs in this process). The system's DLLs come from the system, EOS from next to this
    // program.
    for (const wchar_t* own : {L"\\steam_api64.dll", L"\\umbra_sandlot.dll"})
        if (!LoadLibraryExW((gameFolder + own).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            std::printf("FAIL: %ls cannot be loaded from the game folder (error %lu)\n", own + 1, GetLastError());
            return 1;
        }
    machine.game = LoadLibraryW((gameFolder + L"\\EDF.dll").c_str());
    if (!machine.game) {
        std::printf("FAIL: EDF.dll cannot be loaded from %s (error %lu)\n", Narrow(gameFolder).c_str(), GetLastError());
        return 1;
    }
    // Only this test executable accepts a fake AF profile. Production never reads this variable.
    wchar_t profilePath[32768]{};
    const DWORD profileLength = GetEnvironmentVariableW(L"EDF6NET_AF_PROFILE", profilePath, 32768);
    if (profileLength) {
        if (profileLength >= 32768 || !LoadLibraryW(profilePath)) {
            std::printf("FAIL: AF test profile cannot be loaded (error %lu)\n", GetLastError());
            return 1;
        }
        gamenet::Result("af-profile", "loaded");
    }
    // A seat without EDF6Coop (no DLL in its Mods\Plugins) plays the game as it ships.
    const std::wstring pluginPath = machine.work + L"\\Mods\\Plugins\\EDF6Coop.dll";
    const bool plain = GetFileAttributesW(pluginPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    machine.plugin = plain ? nullptr : LoadLibraryW(pluginPath.c_str());
    using Load = bool(EDFMLAPI*)(PluginInfo*);
    const auto load = machine.plugin ? reinterpret_cast<Load>(GetProcAddress(machine.plugin, "EML6_Load")) : nullptr;
    PluginInfo info{};
    if (plain) {
        gamenet::Result("plugin", "none (the game as it ships)");
    } else if (!load || !load(&info)) {
        std::printf("FAIL: EDF6Coop did not load (error %lu)\n", GetLastError());
        DumpLog(machine);
        return 1;
    }
    const int code = gamenet::RunRole(machine, role);
    // The plugin's log writer runs on its own thread: wait until the file stops growing.
    for (LONGLONG last = -1, stable = 0; stable < 3;) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const LONGLONG size = GetFileAttributesExW(machine.LogPath().c_str(), GetFileExInfoStandard, &data)
                                  ? (static_cast<LONGLONG>(data.nFileSizeHigh) << 32 | data.nFileSizeLow)
                                  : 0;
        stable = size == last ? stable + 1 : 0;
        last = size;
        Sleep(100);
    }
    DumpLog(machine);
    using Unimplemented = std::size_t (*)(char*, std::size_t);
    const auto unimplemented = reinterpret_cast<Unimplemented>(
        GetProcAddress(GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll"), "FakeNet_Unimplemented"));
    char names[4096]{};
    if (unimplemented && unimplemented(names, sizeof(names)))
        for (char *next = nullptr, *line = strtok_s(names, "\n", &next); line; line = strtok_s(nullptr, "\n", &next))
            gamenet::Result("unimplemented-eos", "%s", line);
    // The driver goes by this, not by the exit code alone: under a debugger the exit code is the debugger's.
    gamenet::Result("exit", "%d", code);
    std::fflush(stdout);
    // The game's static objects are not made to be torn down outside the game; neither is the plugin.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
