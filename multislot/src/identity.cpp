#include "identity.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdio>
#include <cstring>

#include "crashlog.h"
#include "log.h"

namespace multislot {
namespace {

// steam_api64.dll ships beside EDF.exe and is loaded before any menu is drawn. The flat API is what the
// DLL exports by name, so no interface vtable has to be guessed; only the version strings do, and the
// two below are the ones this build of the DLL carries.
constexpr const char* kSteamDll = "steam_api64.dll";
constexpr const char* kFriendsVersion = "SteamFriends017";
constexpr const char* kUserVersion = "SteamUser023";
// EOS renders its own ids nowhere in full, so the SDK has to do it for us.
constexpr const char* kEosDll = "EOSSDK-Win64-Shipping.dll";
constexpr std::size_t kProductUserIdChars = 32;

using GetHSteamUserFn = int(__cdecl*)();
using FindOrCreateFn = void*(__cdecl*)(int, const char*);
using GetPersonaNameFn = const char*(__cdecl*)(void*);
using GetSteamIdFn = std::uint64_t(__cdecl*)(void*);
using ProductUserIdIsValidFn = int(__cdecl*)(const void*);
using ProductUserIdToStringFn = int(__cdecl*)(const void*, char*, std::int32_t*);

bool told = false;        // the WHOAMI line has been written
int attempts = 0;         // how many frames have asked Steam; it is not up when the plugin loads
constexpr int kGiveUp = 3600;  // about a minute of menu frames, then stop asking and say so

// MSVC std::wstring: the first 16 bytes are the characters themselves while capacity <= 7, a pointer to
// them when it is more; then size, then capacity. MemberInfo's name is one of these (confirmed against
// its constructor 8F1E80, which sets capacity 7 and writes a wchar_t terminator).
struct WideString {
    union {
        wchar_t inlineChars[8];
        const wchar_t* heapChars;
    };
    std::size_t size;
    std::size_t capacity;
};

void LogSteamMissing(const char* why) {
    if (told) return;
    told = true;
    Log("WHOAMI could not be read: %s. Members are still named by their EOS ProductUserId", why);
}

}  // namespace

const char* ProductUserIdText(const void* id, char* out, std::size_t size) {
    if (!out || !size) return "";
    out[0] = 0;
    if (!id || size <= kProductUserIdChars) return out;
    return Probing([&]() -> const char* {
        __try {
            const HMODULE eos = GetModuleHandleA(kEosDll);
            if (!eos) return out;
            const auto isValid = reinterpret_cast<ProductUserIdIsValidFn>(
                reinterpret_cast<void*>(GetProcAddress(eos, "EOS_ProductUserId_IsValid")));
            const auto toString = reinterpret_cast<ProductUserIdToStringFn>(
                reinterpret_cast<void*>(GetProcAddress(eos, "EOS_ProductUserId_ToString")));
            if (!isValid || !toString || !isValid(id)) return out;
            auto length = static_cast<std::int32_t>(size);
            if (toString(id, out, &length) != 0) out[0] = 0;  // anything but EOS_Success
            out[size - 1] = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out[0] = 0;
        }
        return out;
    });
}

bool NameText(const void* wstring, char* out, std::size_t size) {
    if (!out || !size) return false;
    out[0] = 0;
    if (!wstring) return false;
    wchar_t chars[kMaxNameChars];
    std::size_t count = 0;
    return Probing([&]() -> bool {
        __try {
            const auto* text = static_cast<const WideString*>(wstring);
            count = text->size;
            if (!count || count > kMaxNameChars || text->capacity < count) return false;
            const wchar_t* source = text->capacity > 7 ? text->heapChars : text->inlineChars;
            if (!source) return false;
            std::memcpy(chars, source, count * sizeof(wchar_t));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        // C0 and C1 controls (CR and LF among them), DEL and the Unicode line and paragraph separators.
        for (std::size_t i = 0; i < count; ++i) {
            const wchar_t c = chars[i];
            if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029) chars[i] = L'?';
        }
        const int written = WideCharToMultiByte(CP_UTF8, 0, chars, static_cast<int>(count), out,
                                                static_cast<int>(size) - 1, nullptr, nullptr);
        if (written <= 0) return false;
        out[written] = 0;
        return true;
    });
}

void PollIdentity() {
    if (told) return;
    if (++attempts > kGiveUp) {
        LogSteamMissing("Steam did not answer");
        return;
    }
    Probing([&] {
        __try {
            const HMODULE steam = GetModuleHandleA(kSteamDll);
            if (!steam) return;  // not loaded yet; ask again next frame
            const auto user = reinterpret_cast<GetHSteamUserFn>(
                reinterpret_cast<void*>(GetProcAddress(steam, "SteamAPI_GetHSteamUser")));
            const auto findOrCreate = reinterpret_cast<FindOrCreateFn>(
                reinterpret_cast<void*>(GetProcAddress(steam, "SteamInternal_FindOrCreateUserInterface")));
            const auto personaName = reinterpret_cast<GetPersonaNameFn>(
                reinterpret_cast<void*>(GetProcAddress(steam, "SteamAPI_ISteamFriends_GetPersonaName")));
            const auto steamId = reinterpret_cast<GetSteamIdFn>(
                reinterpret_cast<void*>(GetProcAddress(steam, "SteamAPI_ISteamUser_GetSteamID")));
            if (!user || !findOrCreate || !personaName) {
                LogSteamMissing("steam_api64.dll does not export the calls this needs");
                return;
            }
            const int hUser = user();
            if (!hUser) return;  // signed out, or Steam is still starting
            void* friends = findOrCreate(hUser, kFriendsVersion);
            if (!friends) return;
            const char* name = personaName(friends);
            if (!name || !name[0]) return;
            std::uint64_t id = 0;
            if (steamId) {
                void* self = findOrCreate(hUser, kUserVersion);
                if (self) id = steamId(self);
            }
            told = true;
            Log("WHOAMI this log is \"%s\", SteamID %llu", name, static_cast<unsigned long long>(id));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogSteamMissing("Steam threw while being asked");
        }
    });
}

}  // namespace multislot
