#pragma once
// One machine of a game-code test (GameMachine.exe): the real EDF.dll, the real EDF6Coop.dll loaded the way
// EDFModLoader loads it, and fake_eos_net.cpp as EOS. What a machine does once both are in is its role (roles.cpp).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gamenet {

struct Machine {
    HMODULE game = nullptr;    // EDF.dll
    HMODULE plugin = nullptr;  // EDF6Coop.dll
    std::wstring work;         // this machine's game folder (Mods\Plugins\EDF6Coop.dll, its INI and log)
    std::string user;          // EOS ProductUserId text

    template <typename T>
    T At(std::uintptr_t rva) const {
        return reinterpret_cast<T>(reinterpret_cast<std::uintptr_t>(game) + rva);
    }
    std::wstring LogPath() const { return work + L"\\Mods\\Plugins\\EDF6Coop.log"; }
};

// A line the driver reads from this machine's output: "RESULT <key> <value>".
inline void Result(const char* key, const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("RESULT %s %s\n", key, text);
    std::fflush(stdout);
}

// The EOS function EDF.dll calls through its import table for `name` (after EDF6Coop redirected it, its wrapper):
// calling it is calling EOS the way the game does.
void* GameImport(const Machine& machine, const char* name);

// Runs `role`; returns the process exit code (0 when every check of it passed).
int RunRole(Machine& machine, const std::string& role);

}  // namespace gamenet
