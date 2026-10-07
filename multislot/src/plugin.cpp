#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma warning(push)
#pragma warning(disable : 4201)
#include "PluginAPI.h"
#pragma warning(pop)

#include "code.h"
#include "armor.h"
#include "crashlog.h"
#include "default_ini.h"
#include "fakemembers.h"
#include "hostdataopen.h"
#include "weaponguard.h"
#include "userslots.h"
#include "hostdatanet.h"
#include "hostmode.h"
#include "joinlog.h"
#include "log.h"
#include "loaderproxy.h"
#include "lobbystate.h"
#include "hud.h"
#include "identity.h"
#include "midhook.h"
#include "modfile.h"
#include "mission.h"
#include "netlog.h"
#include "netaoi.h"
#include "netcompress.h"
#include "netfeature.h"
#include "netplayer_game.h"
#include "nettraffic.h"
#include "packetfit.h"
#include "patches.h"
#include "peertimeout.h"
#include "syncmarker.h"
#include "roomview.h"
#include "rooms.h"
#include "smoothing.h"
#include "updatecheck.h"
#include "spawn.h"
#include "hitauthgame.h"
#include "netfeature.h"
#include "widecmp.h"
#include "coop.h"
#include "netfeature.h"
#include "networld.h"
#include "src/config.h"
#include "src/dn_part.h"
#include "src/log.h"
#include "src/product.h"
#include "src/updater.h"

namespace multislot {
namespace {

// CMakeLists.txt's project VERSION, the one version of EDF6Coop: EDFModLoader is told it (PluginInfo), the
// updater compares it with the latest release, and the exported marker below carries it.
#define EDF6COOP_VERSION_TEXT \
    EDF6COOP_TEXT(MULTISLOT_VERSION_MAJOR) "." EDF6COOP_TEXT(MULTISLOT_VERSION_MINOR) "." EDF6COOP_TEXT(MULTISLOT_VERSION_PATCH)
constexpr const char* kVersion = EDF6COOP_VERSION_TEXT;
HMODULE self = nullptr;

// out: MAX_PATH characters. Refuses paths too long to also hold the rotated log name (log.cpp), instead of
// letting a *_s string function end the game over it.
bool SiblingPath(wchar_t* out, const wchar_t* extension) {
    const DWORD length = GetModuleFileNameW(self, out, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    const std::wstring module(out, length);
    const std::size_t dot = module.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring path = module.substr(0, dot) + extension;
    if (path.size() + 8 >= MAX_PATH) return false;  // room for the rotated log name's suffix
    out[path.copy(out, path.size())] = L'\0';
    return true;
}

std::wstring IniText(const wchar_t* ini, const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[64]{};
    GetPrivateProfileStringW(section, key, fallback, value, 64, ini);
    return value;
}

// A key of [HostData], `fallback` when it names none. Its name is kept in `name` for the menu.
int HostDataKey(const wchar_t* ini, const wchar_t* setting, const wchar_t* fallback, int fallbackKey, wchar_t (&name)[16]) {
    auto key = IniText(ini, L"HostData", setting, fallback);
    int vk = VirtualKey(key.c_str());
    if (vk <= 0) {
        Log("[HostData] %ls=%ls is not a known key; using %ls", setting, key.c_str(), fallback);
        vk = fallbackKey;
        key = fallback;
    }
    wcsncpy_s(name, key.c_str(), _TRUNCATE);
    return vk;
}

// [HostData] (hostdatanet.h). The keys' names stay readable for as long as the game runs (the menu shows them).
HostDataSettings ReadHostData(const wchar_t* ini, int* acceptKey, int* pageKey) {
    static wchar_t keyName[16]{}, pageKeyName[16]{};
    HostDataSettings settings;
    settings.share = GetPrivateProfileIntW(L"HostData", L"Share", 1, ini) != 0;
    const auto accept = IniText(ini, L"HostData", L"Accept", L"Always");
    if (_wcsicmp(accept.c_str(), L"Ask") == 0)
        settings.accept = HostAccept::Ask;
    else if (_wcsicmp(accept.c_str(), L"Never") == 0)
        settings.accept = HostAccept::Never;
    else if (_wcsicmp(accept.c_str(), L"Always") != 0)
        Log("[HostData] Accept=%ls is not Always, Ask or Never; using Always", accept.c_str());
    *acceptKey = HostDataKey(ini, L"AcceptKey", L"F1", VK_F1, keyName);
    *pageKey = HostDataKey(ini, L"WeaponPageKey", L"F6", VK_F6, pageKeyName);
    settings.keyName = keyName;
    settings.pageKeyName = pageKeyName;
    settings.page = IniText(ini, L"HostData", L"Page", L"");
    settings.iniPath = ini;
    return settings;
}

RoomViewSettings ReadRoomView(const wchar_t* ini) {
    RoomViewSettings settings;
    const auto button = IniText(ini, L"RoomScreen", L"PageButton", L"RightStick");
    const int offset = PadButtonOffset(button.c_str());
    if (offset < 0) Log("PageButton=%ls is not a known button; using RightStick", button.c_str());
    settings.padButton = static_cast<std::uint32_t>(offset < 0 ? 0xB8 : offset);
    const auto key = [&](const wchar_t* name, const wchar_t* fallback, int fallbackKey) {
        const auto text = IniText(ini, L"RoomScreen", name, fallback);
        const int vk = VirtualKey(text.c_str());
        if (vk < 0) Log("%ls=%ls is not a known key; using %ls", name, text.c_str(), fallback);
        return vk < 0 ? fallbackKey : vk;
    };
    // 0.6.0: F2 became the 8Player MOD key, so the page key moved (PageKey=F2 was the old default).
    if (IniText(ini, L"RoomScreen", L"PageKey", L"").size())
        Log("[RoomScreen] PageKey is no longer used: PageKeys (default F3,Tab) switches member pages");
    const auto pageText = IniText(ini, L"RoomScreen", L"PageKeys", L"F3,Tab");
    wchar_t skipped[64]{};
    int pageKeys[kMaxPageKeys]{};
    ParseKeyList(pageText.c_str(), pageKeys, kMaxPageKeys, skipped, 64);
    if (skipped[0]) Log("PageKeys: %ls left out (not a known key, or F2, which is the 8Player MOD key)", skipped);
    std::memcpy(settings.pageKeys, pageKeys, sizeof(pageKeys));
    settings.dummies = GetPrivateProfileIntW(L"RoomScreen", L"DummyMembers", 0, ini) != 0;
    settings.dummyAddKey = key(L"DummyAddKey", L"F6", VK_F6);
    settings.dummyRemoveKey = key(L"DummyRemoveKey", L"F7", VK_F7);
    if (ResolveDummyKeys(settings) && settings.dummies)
        Log("Dummy keys may not be F2 or a page key: using add %d, remove %d (virtual keys, 0 = none)",
            settings.dummyAddKey, settings.dummyRemoveKey);
    BuildPageHint(settings);
    return settings;
}

bool SupportedImage(const unsigned char* base) {
    return Probing([&]() -> bool {
        __try {
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return false;
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                   nt->FileHeader.TimeDateStamp == kImageTimeDateStamp && nt->OptionalHeader.SizeOfImage == kImageSize;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
}

struct Redirect {
    CallSite site;
    void* handler;
};

struct Hook {
    MidSite site;
    MidHandler handler;
};

struct SlotWrite {
    PointerSlot slot;
    void* handler;
};

// All or nothing: a half-applied set could publish a 5-slot room that unmodded players can join,
// read a capacity from a call that was never redirected, or page a member list the builder never sees.
// The mission start message's site, which the mission phase hooks too: the weapon guard first, then the mission
// phase's own handler.
constexpr std::uint32_t kMissionStart = 0x790887;
MidHandler missionStartHandler = nullptr;
void MissionStartHandler(CpuContext* context) {
    MissionWeaponsHandler(context);
    missionStartHandler(context);
}

bool Apply(unsigned char* base, bool mission, bool hudColours, bool spawns, int ghosts, bool diagnostics, bool armor,
           bool recovery, bool keepRoom, bool hostData, float smoothing, ThunkPage& thunks) {
    auto patches = GuestPatches();
    const auto sessionPatches = SessionPatches();
    patches.insert(patches.end(), sessionPatches.begin(), sessionPatches.end());
    // Bounds of the room size an imm8 cannot hold (widecmp.h): the room tables always, the mission phase with it.
    std::vector<WideCompare> compares = SessionCompares();
    if (mission) {
        const auto missionCompares = MissionCompares();
        compares.insert(compares.end(), missionCompares.begin(), missionCompares.end());
    }
    std::vector<Hook> hooks;
    for (const auto& site : HostModeHooks()) hooks.push_back({site, HostModeHookHandler(site.rva)});
    if (armor)
        for (const auto& site : ArmorHooks()) hooks.push_back({site, ArmorHookHandler(site.rva)});
    if (diagnostics)
        for (const auto& site : DiagnosticHooks()) hooks.push_back({site, JoinLogHookHandler(site.rva)});
    if (keepRoom)
        for (const auto& site : PeerTimeoutHooks()) hooks.push_back({site, &PeerJoinedHandler});
    if (hostData)
        for (const auto& site : HostDataHooks()) hooks.push_back({site, &HostDataOpenHandler});
    for (const auto& site : UserSlotHooks()) hooks.push_back({site, UserSlotHookHandler(site.rva)});
    const auto guard = WeaponGuardHooks();
    hooks.push_back({guard[0], &RoomWeaponsHandler});
    if (!mission) hooks.push_back({guard[1], &MissionWeaponsHandler});
    // Netcode W4 (networld.h): its sites are checked on their own, so a mismatch turns only it off.
    for (const auto& site : VerifiedWorldHooks(base)) hooks.push_back({site, WorldHookHandler(site.rva)});
    if (mission) {
        const auto missionPatches = MissionPatches();
        patches.insert(patches.end(), missionPatches.begin(), missionPatches.end());
        for (const auto& site : MissionHooks()) {
            if (site.rva != kMissionStart) {
                hooks.push_back({site, MissionHookHandler(site.rva)});
                continue;
            }
            missionStartHandler = MissionHookHandler(site.rva);
            hooks.push_back({site, &MissionStartHandler});
        }
        if (hudColours) {
            const auto hudPatches = HudColourPatches();
            patches.insert(patches.end(), hudPatches.begin(), hudPatches.end());
            for (const auto& site : HudColourHooks()) hooks.push_back({site, HudColourHookHandler(site.rva)});
        }
        // The HUD's tables hold kHudTablePlayers with the archive and four without it; the index wraps around them
        // either way, so no room size reads past them.
        SetHudTableSize(hudColours ? kHudTablePlayers : kVanillaPlayers);
        for (const auto& site : HudIndexWrapHooks()) hooks.push_back({site, MissionHookHandler(site.rva)});
        if (spawns)
            for (const auto& site : SpawnHooks()) hooks.push_back({site, SpawnHookHandler(site.rva)});
        if (ghosts > 0)
            for (const auto& site : GhostHooks()) hooks.push_back({site, GhostHookHandler(site.rva)});
        for (const auto& site : PacketFitHooks()) hooks.push_back({site, PacketFitHookHandler(site.rva)});
        for (const auto& site : BvmPlayerTableHooks()) hooks.push_back({site, BvmPlayerTableHandler(site.rva)});
    }
    std::vector<Redirect> redirects;
    const auto guest = GuestCalls();
    redirects.push_back({guest[0], reinterpret_cast<void*>(&RoomCountAndCapacity)});
    redirects.push_back({guest[1], reinterpret_cast<void*>(&RoomFullCount)});
    const auto room = RoomViewCalls();
    redirects.push_back({room[0], reinterpret_cast<void*>(&BuildPanelsHook)});
    redirects.push_back({room[1], reinterpret_cast<void*>(&BuildPanelsHook)});
    redirects.push_back({room[2], reinterpret_cast<void*>(&UpdateVoiceIconsHook)});
    // Always, not only with dummy members: the member list is cut to this build's room size (fakemembers.h).
    for (const auto& call : MemberListCalls()) redirects.push_back({call, MemberListCallHandler(call.rva)});
    if (diagnostics)
        for (const auto& call : DiagnosticCalls()) redirects.push_back({call, JoinLogCallHandler(call.rva)});
    if (recovery)
        for (const auto& call : RecoveryCalls()) redirects.push_back({call, reinterpret_cast<void*>(&FinalHelloHook)});
    if (keepRoom)
        for (const auto& call : PeerTimeoutCalls())
            redirects.push_back({call, reinterpret_cast<void*>(&PeerTimeoutLeaveCheck)});
    if (mission) {
        for (const auto& call : MissionCalls()) redirects.push_back({call, MissionCallHandler(call.rva)});
        for (const auto& call : PacketFitCalls()) redirects.push_back({call, PacketFitCallHandler(call.rva)});
        if (ghosts > 0)
            for (const auto& call : GhostCalls()) redirects.push_back({call, GhostCallHandler(call.rva)});
    }
    // Netcode rewrite W1: the controller's plaintext datagrams, for their class and the per-type traffic log.
    for (const auto& call : NetTrafficCalls()) redirects.push_back({call, NetTrafficCallHandler(call.rva)});
    for (const auto& call : NetCompressCalls()) redirects.push_back({call, NetCompressCallHandler(call.rva)});
    std::vector<SlotWrite> slots{{RoomViewSlot(), reinterpret_cast<void*>(&RoomOnUpdateHook)},
                                 {MainFrameSlot(), reinterpret_cast<void*>(&MainFrameOnUpdateHook)}};
    if (mission)
        for (const auto& missionSlot : MissionSlots()) slots.push_back({missionSlot, MissionSlotHandler(missionSlot.rva)});

    for (const auto& patch : patches) {
        if (patch.rva + patch.original.size() > kImageSize || !Matches(base + patch.rva, patch)) {
            Log("REFUSED: EDF+%X (%s) is not the expected code; another mod or a game update? Nothing was changed",
                patch.rva, patch.name);
            return false;
        }
    }
    for (const auto& hook : hooks) {
        const auto& site = hook.site;
        const Patch verify{site.name, site.rva, site.original, site.original};
        if (!hook.handler || site.original.size() < 5 || site.displacedOffset + site.displacedSize > site.original.size() ||
            site.rva + site.original.size() > kImageSize || !Matches(base + site.rva, verify)) {
            Log("REFUSED: EDF+%X (%s) is not the expected code; nothing was changed", site.rva, site.name);
            return false;
        }
    }
    for (const auto& site : compares) {
        const Patch verify{site.name, site.rva, site.original, site.original};
        if (!DecodeWideCompare(site).valid || site.rva + site.original.size() > kImageSize || !Matches(base + site.rva, verify)) {
            Log("REFUSED: EDF+%X (%s) is not the expected compare; nothing was changed", site.rva, site.name);
            return false;
        }
    }
    for (const auto& redirect : redirects) {
        if (!redirect.handler || !CallTargets(base + redirect.site.rva, redirect.site.rva, redirect.site.target)) {
            Log("REFUSED: EDF+%X (%s) does not call EDF+%X; nothing was changed", redirect.site.rva, redirect.site.name,
                redirect.site.target);
            return false;
        }
    }
    for (const auto& write : slots) {
        const auto& slot = write.slot;
        if (!write.handler || !SlotTargets(base + slot.rva, reinterpret_cast<std::uint64_t>(base), slot.target)) {
            Log("REFUSED: EDF+%X (%s) does not point at EDF+%X; nothing was changed", slot.rva, slot.name, slot.target);
            return false;
        }
    }
    if (!thunks.Allocate(base)) {
        Log("REFUSED: no memory for call stubs near EDF.dll; nothing was changed");
        return false;
    }
    // Redirections are written first: the room info rewrite reads the value they return.
    std::vector<Patch> writes;
    for (const auto& redirect : redirects) {
        const unsigned char* stub = thunks.Add(redirect.handler);
        auto bytes = stub ? CallBytes(base + redirect.site.rva, stub) : std::vector<std::uint8_t>{};
        if (bytes.empty()) {
            Log("REFUSED: call stub for %s out of reach; nothing was changed", redirect.site.name);
            thunks.Release();
            return false;
        }
        writes.push_back({redirect.site.name, redirect.site.rva, {base + redirect.site.rva, base + redirect.site.rva + 5}, std::move(bytes)});
    }
    for (const auto& hook : hooks) {
        const auto& site = hook.site;
        const unsigned char* at = base + site.rva;
        const auto resume = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(at + site.original.size()));
        const unsigned char* thunk = EmitMidThunk(thunks, hook.handler, at + site.displacedOffset, site.displacedSize, resume);
        auto bytes = thunk ? JumpBytes(at, thunk, site.original.size()) : std::vector<std::uint8_t>{};
        if (bytes.empty()) {
            Log("REFUSED: hook thunk for %s out of reach; nothing was changed", site.name);
            thunks.Release();
            return false;
        }
        writes.push_back({site.name, site.rva, site.original, std::move(bytes)});
    }
    for (const auto& site : compares) {
        const unsigned char* at = base + site.rva;
        const unsigned char* cave = EmitWideCompare(thunks, site, at);
        auto bytes = cave ? JumpBytes(at, cave, site.original.size()) : std::vector<std::uint8_t>{};
        if (bytes.empty()) {
            Log("REFUSED: compare cave for %s out of reach; nothing was changed", site.name);
            thunks.Release();
            return false;
        }
        writes.push_back({site.name, site.rva, site.original, std::move(bytes)});
    }
    // The remote-player correction factor, emitted next to the stubs while the page is still writable.
    if (smoothing > 0.0f && smoothing != kVanillaSmoothing) {
        Patch factor{};
        if (SmoothingPatch(base, thunks, smoothing, factor)) {
            writes.push_back(std::move(factor));
        } else {
            Log("REFUSED: the remote player correction factor site (EDF+%X) is not what this build expects, "
                "or there was no room for the constant; nothing was changed",
                kSmoothingSite);
            thunks.Release();
            return false;
        }
    }
    if (!thunks.Seal()) {
        Log("REFUSED: could not make call stubs executable (error %lu); nothing was changed", GetLastError());
        thunks.Release();
        return false;
    }
    if (!thunks.Unwindable())
        Log("Call stubs: their unwind data could not be registered, so a crash inside a hook handler shows a call "
            "stack that ends at the stub");
    writes.insert(writes.end(), patches.begin(), patches.end());
    for (const auto& write : slots) {
        const auto& slot = write.slot;
        std::vector<std::uint8_t> handler(sizeof(std::uint64_t));
        const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(write.handler));
        std::memcpy(handler.data(), &address, sizeof(address));
        writes.push_back({slot.name, slot.rva, {base + slot.rva, base + slot.rva + sizeof(std::uint64_t)}, std::move(handler)});
    }

    for (std::size_t i = 0; i < writes.size(); ++i) {
        const auto& write = writes[i];
        if (!WriteCode(base + write.rva, write.replacement.data(), write.replacement.size())) {
            Log("ERROR: could not write EDF+%X (%s), error %lu; restoring %zu earlier writes", write.rva, write.name,
                GetLastError(), i);
            for (std::size_t j = i; j-- > 0;)
                WriteCode(base + writes[j].rva, writes[j].original.data(), writes[j].original.size());
            thunks.Release();
            return false;
        }
    }
    // One line instead of one per site: the sites are the same on every start, a refused or failed one is logged
    // by name above, and the stub address places a crash inside a stub or hook thunk.
    Log("Patched EDF.dll: %zu sites (%zu redirected calls, %zu hooks, %zu widened compares, %zu code patches, %zu vtable "
        "slots); stubs and hook thunks at %p (%zu bytes)", writes.size(), redirects.size(), hooks.size(), compares.size(),
        patches.size(), slots.size(), static_cast<const void*>(thunks.Base()), thunks.Used());
    return true;
}

// A file under Mods (modfile.h) exists only while the plugin is active and uses it: otherwise the game gets its
// own file back. `area` starts the log lines, `contents` says what the file is, `without` what is missing while
// it is not ours. True when our file is in place.
bool KeepModFile(const ModFile& file, bool active, const char* area, const char* contents, const char* without) {
    wchar_t plugin[MAX_PATH]{};
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(self, plugin, MAX_PATH);
    if (!length || length >= MAX_PATH || !ModFilePath(file, plugin, path, MAX_PATH)) {
        if (active) Log("%s: the plugin is not in Mods\\Plugins, so %s", area, without);
        return false;
    }
    if (!active) {
        const FileRemoval removal = RemoveModFile(file, path);
        if (removal == FileRemoval::Removed)
            Log("%s: removed Mods\\%ls; the game's own file is used", area, file.path);
        else if (removal == FileRemoval::Failed)
            Log("%s: could not remove Mods\\%ls (error %lu)", area, file.path, GetLastError());
        return false;
    }
#ifdef MULTISLOT_PLACEHOLDER_ASSETS
    // A CI build (CMake MULTISLOT_CI) has no files of the game's to embed, only placeholders that would break the
    // game; they are never written to a game folder.
    Log("%s: this is a CI test build with placeholder files; Mods\\%ls (%s) is not written, so %s", area, file.path,
        contents, without);
    return false;
#else
    switch (InstallModFile(file, path)) {
        case FileInstall::Written: Log("%s: wrote Mods\\%ls (%s)", area, file.path, contents); return true;
        case FileInstall::Updated: Log("%s: updated Mods\\%ls", area, file.path); return true;
        case FileInstall::Current: return true;
        case FileInstall::Foreign:
            Log("%s: Mods\\%ls belongs to another mod and was left alone; %s", area, file.path, without);
            return false;
        case FileInstall::Failed:
            Log("%s: could not write Mods\\%ls (error %lu); %s", area, file.path, GetLastError(), without);
            return false;
    }
    return false;
#endif
}

bool KeepMenuLayout(bool active) {
    char contents[64]{}, without[64]{};
    _snprintf_s(contents, _TRUNCATE, "the menu layout plus the Player MOD label");
    _snprintf_s(without, _TRUNCATE, "the Player MOD label is not shown (F2 still works)");
    return KeepModFile(MenuLayoutFile(), active, "Menu", contents, without);
}

bool KeepHudArchive(bool active) {
    return KeepModFile(HudArchiveFile(), active, "HUD",
                       "the online HUD textures plus a lamp and a chat balloon for every player colour",
                       "players 5+ share the HUD colours of players 1-4");
}

void RemoveModFiles() {
    KeepMenuLayout(false);
    KeepHudArchive(false);
}

ThunkPage thunks;

}  // namespace
}  // namespace multislot

namespace {
// A member we play with over a direct link runs a build of the same direct-link protocol, which reads a split
// start message whether or not Epic's lobby still shows its marker; the others are asked the lobby.
bool ReadsSplitSync(const void* remote) {
    return dn::readsSplitSyncDirectly(remote) || multislot::PeerReadsSplitSync(remote);
}

// [MultiSlot] RoomSize, the size of the rooms this machine hosts (hostmode.h). An INI from 2.2 or earlier has
// EightPlayerRooms instead, whose ON meant its build's size; the INI does not say which build that was, and eight
// is the one most played, so ON becomes eight (F2 reaches the others).
int ReadRoomSize(const wchar_t* iniPath) {
    using namespace multislot;
    const int size = static_cast<int>(GetPrivateProfileIntW(L"MultiSlot", L"RoomSize", static_cast<UINT>(-1), iniPath));
    if (size == -1) {
        if (GetPrivateProfileIntW(L"MultiSlot", L"EightPlayerRooms", 0, iniPath) == 0) return 0;
        Log("[MultiSlot] EightPlayerRooms=1 from an earlier version: rooms you host are for 8 players (F2 switches)");
        return 8;
    }
    if (ValidRoomSize(size)) return size;
    Log("[MultiSlot] RoomSize=%d is not 0 or 2..%d; hosting normal rooms", size, kMaxPlayers);
    return 0;
}

// The room part: rooms of up to kMaxPlayers, the missions and room screen for them, and the crash log.
// Returns whether it runs; when it does not, it has changed nothing and started nothing.
bool LoadRooms(const wchar_t* iniPath) {
    using namespace multislot;
    const bool enabled = GetPrivateProfileIntW(L"MultiSlot", L"Enabled", 1, iniPath) != 0;
    const int roomSize = ReadRoomSize(iniPath);
    const bool crashLog = GetPrivateProfileIntW(L"MultiSlot", L"CrashLog", 1, iniPath) != 0;
    // The room part's only contact outside the game: one read of the VR mod's release page,
    // so the menu can say when a newer package exists. Nothing is downloaded (updatecheck.h).
    const bool checkUpdates = GetPrivateProfileIntW(L"Update", L"CheckEDF6VR", 1, iniPath) != 0;
    // How fast a remote player's drawn position catches up (smoothing.h). A percentage, so the INI holds
    // a whole number; 0 leaves the game's own 5 alone. Local and display-only: it changes nothing that is
    // sent, and a player without it is unaffected.
    const int smoothingPercent = GetPrivateProfileIntW(L"Smoothing", L"RemotePlayerPercent", 0, iniPath);
    const float smoothing = smoothingPercent > 0 && smoothingPercent <= 100
                                ? static_cast<float>(smoothingPercent) / 100.0f
                                : 0.0f;
    // A minidump of the first access violation inside EDF.dll or the plugin, one file overwritten each launch. The
    // 2026-09-27 host crash is a null whose source is a stack local, which no log line can show.
    // Off unless the INI asks for it: a dump carries process memory, which can include the room name and
    // chat text, and that is not something to switch on for everyone who installs the package.
    const bool crashDump = GetPrivateProfileIntW(L"MultiSlot", L"CrashDump", 0, iniPath) != 0;
    // On by default since 1.2.2, so reports from real rooms come with the lobby and P2P lines (the log caps itself).
    const bool netLog = GetPrivateProfileIntW(L"MultiSlot", L"NetLog", 1, iniPath) != 0;
    const bool recovery = GetPrivateProfileIntW(L"MultiSlot", L"HandshakeRecovery", 1, iniPath) != 0;
    // An established member stays in the room when a newcomer's P2P handshake with it times out (peertimeout.h).
    const bool keepRoom = GetPrivateProfileIntW(L"MultiSlot", L"KeepRoomOnPeerTimeout", 1, iniPath) != 0;
    const bool hostData = GetPrivateProfileIntW(L"HostData", L"Enabled", 1, iniPath) != 0;
    SetDetailLog(netLog);
    if (!enabled) {
        Log("[MultiSlot] Enabled=0: rooms, missions and the room screen are left untouched");
        RemoveModFiles();
        return false;
    }
    if (IniText(iniPath, L"MultiSlot", L"MaxPlayers", L"").size())
        Log("[MultiSlot] MaxPlayers is no longer used: the Player MOD setting is switched on a menu screen "
            "outside a room ([MultiSlot] Key and PadButton)");
    RoomViewSettings roomView = ReadRoomView(iniPath);
    const bool mission = GetPrivateProfileIntW(L"Mission", L"Extend", 1, iniPath) != 0;
    int ghosts = static_cast<int>(GetPrivateProfileIntW(L"Test", L"GhostPlayers", 0, iniPath));
    if (ghosts < 0 || ghosts > kMaxPlayers - 1) {
        Log("GhostPlayers=%d is outside 0..%d; using %d", ghosts, kMaxPlayers - 1, ghosts < 0 ? 0 : kMaxPlayers - 1);
        ghosts = ghosts < 0 ? 0 : kMaxPlayers - 1;
    }
    if (!mission) ghosts = 0;
    const bool spawns = mission && GetPrivateProfileIntW(L"Mission", L"ExtraEnemies", 1, iniPath) != 0;
    // 8Player MOD: switched outside a room only, because inside one the same inputs page through the members.
    const auto hostKeyText = IniText(iniPath, L"MultiSlot", L"Key", L"F2");
    int hostModeKey = VirtualKey(hostKeyText.c_str());
    if (hostModeKey < 0) {
        Log("[MultiSlot] Key=%ls is not a known key; using F2", hostKeyText.c_str());
        hostModeKey = VK_F2;
    }
    // The left stick is free outside a room (the page button is the right one, and copy armor only works
    // inside a room), so it can switch the setting there without taking anything away.
    const auto hostPadText = IniText(iniPath, L"MultiSlot", L"PadButton", L"LeftStick");
    int hostPadOffset = PadButtonOffset(hostPadText.c_str());
    if (hostPadOffset < 0) {
        Log("[MultiSlot] PadButton=%ls is not a known button; using LeftStick", hostPadText.c_str());
        hostPadOffset = 0xB0;
    }
    if (hostPadOffset && static_cast<std::uint32_t>(hostPadOffset) == roomView.padButton) {
        Log("[MultiSlot] PadButton may not be the page button; using none");
        hostPadOffset = 0;
    }
    const auto hostModePad = static_cast<std::uint32_t>(hostPadOffset);
    wchar_t hostModeHint[24]{};
    {
        wchar_t keyName[8]{};
        if (hostModeKey) KeyName(hostModeKey, keyName, 8);
        const wchar_t* padName = PadButtonShortName(hostModePad);
        _snwprintf_s(hostModeHint, _TRUNCATE, L"%ls%ls%ls", keyName, keyName[0] && padName[0] ? L"/" : L"", padName);
    }

    // "copy armor": with neither a key nor a pad button the three armor sites are left alone entirely.
    // It works inside a room, where the page inputs also work, so it may share neither of them.
    int copyArmorKey = 0;
    std::uint32_t copyArmorPad = 0;
    int copyArmorIgnore = 0;
    int copyArmorCaps[kSoldierTypeCount]{};
    wchar_t copyArmorHint[24]{};
    if (GetPrivateProfileIntW(L"CopyArmor", L"Enabled", 1, iniPath) != 0) {
        const auto text = IniText(iniPath, L"CopyArmor", L"Key", L"F4");
        const int vk = VirtualKey(text.c_str());
        if (vk < 0) Log("[CopyArmor] Key=%ls is not a known key; using F4", text.c_str());
        copyArmorKey = vk < 0 ? VK_F4 : vk;
        if (copyArmorKey == hostModeKey) {
            Log("[CopyArmor] Key may not be the Player MOD key; using F4");
            copyArmorKey = copyArmorKey == VK_F4 ? 0 : VK_F4;
        }
        for (const int page : roomView.pageKeys)
            if (page && page == copyArmorKey) {
                Log("[CopyArmor] Key may not be a page key; using F4");
                copyArmorKey = copyArmorKey == VK_F4 ? 0 : VK_F4;
            }
        const auto padText = IniText(iniPath, L"CopyArmor", L"PadButton", L"LeftStick");
        const int padOffset = PadButtonOffset(padText.c_str());
        if (padOffset < 0) Log("[CopyArmor] PadButton=%ls is not a known button; using LeftStick", padText.c_str());
        copyArmorPad = static_cast<std::uint32_t>(padOffset < 0 ? 0xB0 : padOffset);
        if (copyArmorPad && copyArmorPad == roomView.padButton) {
            Log("[CopyArmor] PadButton may not be the page button; using none");
            copyArmorPad = 0;
        }
        // Someone barely different from you is no help to copy, so they are passed over for the next one up.
        copyArmorIgnore = static_cast<int>(GetPrivateProfileIntW(L"CopyArmor", L"IgnoreWithin", 300, iniPath));
        if (copyArmorIgnore < 0) {
            Log("[CopyArmor] IgnoreWithin=%d is below zero; using 0", copyArmorIgnore);
            copyArmorIgnore = 0;
        }
        // The most this may give, per class: a rescue for someone behind, not a way to someone else's armor.
        const struct {
            const wchar_t* key;
            int fallback;
        } caps[kSoldierTypeCount] = {{L"MaxRanger", 4000}, {L"MaxWingDiver", 2500}, {L"MaxAirRaider", 3000},
                                     {L"MaxFencer", 4000}};
        for (int i = 0; i < kSoldierTypeCount; ++i) {
            copyArmorCaps[i] = static_cast<int>(GetPrivateProfileIntW(L"CopyArmor", caps[i].key, caps[i].fallback, iniPath));
            if (copyArmorCaps[i] >= 0) continue;
            Log("[CopyArmor] %ls=%d is below zero; using no ceiling", caps[i].key, copyArmorCaps[i]);
            copyArmorCaps[i] = 0;
        }
        if (copyArmorPad && copyArmorPad == roomView.padButton) {
            Log("[CopyArmor] PadButton may not be the page button; using none");
            copyArmorPad = 0;
        }
        wchar_t keyName[8]{};
        if (copyArmorKey) KeyName(copyArmorKey, keyName, 8);
        const wchar_t* padName = PadButtonShortName(copyArmorPad);
        _snwprintf_s(copyArmorHint, _TRUNCATE, L"%ls%ls%ls", keyName, keyName[0] && padName[0] ? L"/" : L"",
                     padName);
    }
    // 0.4.x let the INI multiply the 4-player factor; 0.5.0 follows fixed rules every player shares. Those builds had
    // at most 32 slots, so no INI has a key past Scale32.
    for (int players = kVanillaPlayers + 1; players <= 32; ++players) {
        wchar_t key[16];
        _snwprintf_s(key, _TRUNCATE, L"Scale%d", players);
        if (IniText(iniPath, L"Mission", key, L"").size()) {
            Log("[Mission] Scale5..Scale32 are no longer used: 5+ players follow fixed rules (see README)");
            break;
        }
    }

    const HMODULE game = GetModuleHandleW(L"EDF.dll");
    const auto base = reinterpret_cast<unsigned char*>(game);
    if (!game || !SupportedImage(base)) {
        Log("REFUSED: EDF.dll is not the supported build (TimeDateStamp %08X, SizeOfImage %X); nothing was changed",
            kImageTimeDateStamp, kImageSize);
        RemoveModFiles();
        return false;
    }
    if (roomView.dummies && copyArmorKey) {
        const auto clash = [&](int& key, const char* which) {
            if (key != copyArmorKey) return;
            Log("[RoomScreen] Dummy%s key is the copy armor key; leaving it unbound", which);
            key = 0;
        };
        clash(roomView.dummyAddKey, "Add");
        clash(roomView.dummyRemoveKey, "Remove");
    }
    InitRooms(base);
    InitRoomView(base, roomView);
    InitFakeMembers(base);
    InitMission(base, ghosts);
    InitPacketFit(base);
    InitNetTraffic(base);
    InitNetFeature(iniPath);
    InitNetCompress(base, NetFeatureEnabledLocally(NetFeature::Compression));
    if (const UINT budget = GetPrivateProfileIntW(L"Test", L"SplitSyncBudget", 0, iniPath); budget && mission) {
        SetSyncBudget(budget);
        Log("TEST SplitSyncBudget=%u: start messages you host keep at most that many bytes inline and send the "
            "other loadout records beside them, even in small rooms (0 = normal)", budget);
    }
    InitWeaponGuard(base);
    // Netcode W4 (networld.h): enemy random state interval and the insect pose interval (two immediate operands).
    InitWorld(base, iniPath);
    InitJoinLog(base);
    InitFinalHello(base);
    InitPeerTimeout(base);
    InitArmor(base, copyArmorKey, copyArmorPad, copyArmorHint, copyArmorIgnore, copyArmorCaps);
    InitHostMode(base, iniPath, roomSize, hostModeKey, hostModePad, hostModeHint);
    // The HUD archive goes in first: the HUD is patched for its lamps and balloons only when it is ours. Before the
    // game loads it, so a mission never meets patches without their textures. Without Extend the HUD is the game's.
    const bool hudColours = KeepHudArchive(mission);
    if (!Apply(base, mission, hudColours, spawns, ghosts, netLog, copyArmorKey || copyArmorPad, recovery, keepRoom,
               hostData, smoothing, thunks)) {
        RemoveModFiles();
        return false;
    }
    KeepMenuLayout(true);
    // Hit authority (hitauthgame.h, docs/net-re/damage.md): hooks and a thunk page of its own, so a site that is
    // not the expected code leaves only it off and the game decides hits as it always did.
    InstallHitAuthority(base);
    Log("Joining: normal rooms and MultiSlot rooms of every size from this 1024-slot EDF6Coop on are joinable, and the room "
        "list shows both whatever the setting");
    Log("Rooms: %d user slots, packet sessions and voice chat HUD records (P2P links to every member of a %d-player "
        "room; 4 or fewer: the extra ones stay empty)", kMaxPlayers, kMaxPlayers);
    char hosting[32]{};
    if (roomSize)
        _snprintf_s(hosting, _TRUNCATE, "%dPlayer MOD ON", roomSize);
    else
        _snprintf_s(hosting, _TRUNCATE, "Player MOD OFF");
    Log("Hosting: %s (%ls on a menu screen outside a room steps OFF/8/10/12/16/24/32/48/64/128/256/512/1024; inside one "
        "those page through "
        "the members instead): OFF = normal 4-player rooms anyone can join, N = MultiSlot rooms for N players, "
        "hidden from players without this mod",
        hosting, hostModeHint);
    if (copyArmorKey || copyArmorPad)
        Log("Copy armor: %ls in a room follows the lowest armor in it that is more than %d from yours (your own "
            "class if anyone else plays it, otherwise the room), up to %d/%d/%d/%d for Ranger/Wing Diver/Air "
            "Raider/Fencer; it never lowers, never reaches the room's own display and never saves",
            copyArmorHint, copyArmorIgnore, copyArmorCaps[0], copyArmorCaps[1], copyArmorCaps[2], copyArmorCaps[3]);
    else
        Log("Copy armor: off, the armor sites are untouched");
    Log("Room screen: 4 member panels per page, switched with %ls; fake members %s", roomView.pageHint[0] ? roomView.pageHint : L"(nothing)",
        roomView.dummies ? "ON (test mode: F6 adds one to the room's member list, F7 removes one)" : "off");
    if (mission) {
        Log("Mission: players 5-%d get loadout sidecars, player slots 5-%d and spawn points (4 or fewer: unchanged)",
            kMaxPlayers, kMaxPlayers);
        if (hudColours)
            Log("HUD: one colour per player for players 1-%d - status lamp, chat balloon and radar marker (1-4 the game's "
                "own); players %d+ share those colours in turn", kHudTablePlayers, kHudTablePlayers + 1);
        else
            Log("HUD: players 5-%d share the colours of players 1-4", kMaxPlayers);
        Log("Mission: 5+ players online - enemy durability, damage and speed stay at the 4-player values");
        if (spawns) {
            // The factor is (players + 1) / 5 (spawn.cpp), written out so the log says what every machine does.
            char factors[400]{};
            int used = 0;
            for (int players = kVanillaPlayers + 1; players <= kMaxPlayers && used >= 0; ++players) {
                const int written = _snprintf_s(factors + used, sizeof(factors) - used, _TRUNCATE, "%sx%d.%d (%d)",
                                                used ? " " : "", (players + 1) / 5, (players + 1) * 2 % 10, players);
                used = written < 0 ? -1 : used + written;
            }
            Log("Mission: 5+ players online - enemy counts %s, rounded; nests, anchors, ships and other fixed "
                "objects unchanged", factors);
        } else
            Log("Mission: ExtraEnemies=0, enemy counts unchanged (every player in a room needs the same setting)");
    } else
        Log("Mission: Extend=0, mission code untouched (only rooms of up to four players can start safely)");
    if (ghosts > 0)
        Log("Test: GhostPlayers=%d - a mission started alone online gets %d idle copies of you as extra players", ghosts, ghosts);
    // First, so its wrappers sit next to EOS: what a room update publishes is read from the lobby, and a
    // machine never stays behind in a lobby it closed or left (lobbystate.h).
    SetRoomMemberCountSource(&GameRoomMemberCount);
    if (InstallLobbyState(game, &RedirectGameImport))
        Log("Lobby state: room updates keep the lobby's own kind and size; a close of a lobby someone else owns, "
            "or one that fails, leaves it");
    else
        Log("Lobby state: UNAVAILABLE - room updates publish the game's own values (%d players)", kVanillaPlayers);
    // Host data's receive wrapper sits next to EOS, before packetfit's and the net log's: its packets never reach
    // them (hostdatanet.h). Its lobby attributes go out once the lobby glue below is in.
    if (hostData) {
        int acceptKey = 0, pageKey = 0;
        HostDataSettings settings = ReadHostData(iniPath, &acceptKey, &pageKey);
        wchar_t folder[MAX_PATH]{};
        if (SiblingPath(folder, L".dll")) {
            for (int up = 0; up < 3; ++up)
                if (wchar_t* slash = wcsrchr(folder, L'\\')) *slash = 0;
            settings.gameFolder = folder;
        }
        if (!settings.gameFolder.empty() && StartHostData(game, &RedirectGameImport, settings)) {
            SetWeaponFeature(acceptKey, pageKey, &HostDataMenuFrame);
            Log("Host data: on; Share=%d, Accept=%ls, AcceptKey=%ls, WeaponPageKey=%ls (weapon and vehicle files only, "
                "checked against the SHA-256 their member published, kept in Mods\\Plugins\\EDF6Coop.hostdata)",
                settings.share ? 1 : 0,
                settings.accept == HostAccept::Ask ? L"Ask" : settings.accept == HostAccept::Always ? L"Always" : L"Never",
                settings.keyName, settings.pageKeyName);
        } else {
            Log("Host data: UNAVAILABLE - the host's files cannot be fetched in rooms");
        }
    }
    // Before the net log: its wrappers go in front of these, so they still see the game as their caller.
    const int imports = mission ? InstallPacketFit(game, &RedirectGameImport) : 0;
    // One set of lobby wrappers carries the split marker and host data's attributes (syncmarker.h). The marker says
    // this machine reads a split message: only true once both P2P imports are ours.
    const bool lobbyGlue = (imports == 2 || hostData) && InstallSyncMarker(game, &RedirectGameImport, imports == 2);
    const bool marker = imports == 2 && lobbyGlue;
    // Netcode rewrite: our netcode protocol and features go into our lobby entry with the marker (netfeature.h).
    StartNetFeature(lobbyGlue);
    // I1: a host lets the room's real size (up to 1024, lobbystate.h) in over the direct link, not Epic's 64; state
    // datagrams follow interest management within each path's budget (netaoi.h).
    dn::setRoomCapacitySource([]() -> std::uint32_t { return static_cast<std::uint32_t>(std::max(0, CurrentLobbyCapacity())); });
    // Member slots (userslots.h): a host sends its game's slots, every other game adds each member in the host's slot.
    dn::setGameSlotsSource(&GameSlotTable);
    SetUserSlotSources([](const std::string& member) { return dn::hostSlotOf(member); }, &ProductUserIdText);
    // [Test] RoomCapacity: the room's size where the test network's lobby cannot say it (gamenet joinfull).
    if (const UINT testCapacity = GetPrivateProfileIntW(L"Test", L"RoomCapacity", 0, iniPath)) {
        static UINT capacity = 0;
        capacity = testCapacity;
        dn::setRoomCapacitySource([]() -> std::uint32_t { return capacity; });
        Log("TEST RoomCapacity=%u: the room holds that many whatever its lobby says", testCapacity);
    }
    InstallNetInterest(GetPrivateProfileIntW(L"Netcode", L"Interest", 1, iniPath) != 0);
    // I1: a start message too large even for stubs sends every record in bulk while the room reads fragments.
    static const bool dropRecordsBulk = GetPrivateProfileIntW(L"Test", L"DropRecordsBulk", 0, iniPath) != 0;
    if (dropRecordsBulk) Log("TEST DropRecordsBulk=1: the records of start messages this machine hosts are never sent");
    SetBulkRecords([] { return NetFeatureActive(NetFeature::Fragments); },
                   [](const void* remote, std::uint16_t tag, const void* data, std::size_t size) {
                       char id[40]{};
                       if (dropRecordsBulk) return true;  // [Test]: "sent", and lost
                       return remote && *ProductUserIdText(remote, id, sizeof(id)) && SendBulk(id, tag, data, size);
                   },
                   [](const void* peer, std::size_t* total) -> std::uint64_t {
                       char id[40]{};
                       return peer && *ProductUserIdText(peer, id, sizeof(id)) ? dn::bulkIncoming(id, kRecordsBulkTag, total)
                                                                               : 0;
                   });
    SetBulkHandlerForTag(kRecordsBulkTag, [](const std::string&, std::uint16_t, const std::uint8_t* data, std::size_t size) {
        TakeRecordsBulk(data, size);
    });
    if (marker) SetSplitSyncReaders(&ReadsSplitSync);
    if (mission) {
        if (imports == 2)
            Log("Mission sync: a start message too large for one EOS packet (%zu bytes; eight players made 1180) keeps "
                "what fits and sends the other loadout records beside it; smaller ones are unchanged. It only goes to "
                "members whose lobby entry says they read it (MultiSlot 1.5.15 or later); lobby marker %s",
                kEosMaxPacket, marker ? "on" : "UNAVAILABLE, so such a start message is sent to nobody");
        else
            Log("Mission sync: only %d of the 2 EOS P2P imports could be redirected; missions of eight or more players "
                "cannot start", imports);
    }
    if (netLog || recovery) {
        const int netImports = InstallNetLog(game, netLog, recovery);
        if (netLog)
            Log("Net log: %d EOS imports redirected (NetLog=0 turns the detailed log off; the log file keeps its newest 2 MB)", netImports);
        else
            Log("Recovery transport: %d EOS import redirected; detailed network logging off", netImports);
    } else {
        Log("HandshakeRecovery=0: off");
    }
    if (keepRoom)
        Log("KeepRoomOnPeerTimeout=1: when the P2P handshake with someone who joined after you times out, you stay in "
            "the room and they stay unconnected (the game makes every member but the host leave); someone whose own "
            "join fails still leaves");
    else
        Log("KeepRoomOnPeerTimeout=0: a timed-out P2P handshake makes this machine leave the room, as the game does");
    if (smoothing > 0.0f && smoothing != kVanillaSmoothing)
        Log("Remote players: their drawn position closes %d%% of the gap per update instead of %d%%, so a "
            "correction shrinks to a tenth in about %d ms instead of %d ms. Display only - nothing sent "
            "changes, and players without this are unaffected",
            smoothingPercent, static_cast<int>(kVanillaSmoothing * 100.0f), SmoothingSettleMs(smoothing),
            SmoothingSettleMs(kVanillaSmoothing));
    else
        Log("Remote players: the game's own position smoothing is untouched "
            "([Smoothing] RemotePlayerPercent in the INI raises it; 0 = leave alone)");
    // W2 player sync (netplayer_game.h): its own slots and checks; refused alone, the rest stays as it is.
    if (!InstallPlayerSync(base, ReadPlayerSyncSettings(iniPath)))
        Log("PLAYER sync: not installed; every player keeps the game's own sync");
    // EDF.dll ends the game with TerminateProcess, so the shutdown marker needs that import wrapped, or
    // it never fires and every start wrongly reports the last one as cut (1.5.2-1.5.8 did exactly that:
    // 15 such lines in one friend's log, 9 in another's, none of them real).
    if (InstallExitMarker(game))
        Log("Exit marker: a normal quit now writes SHUTDOWN, so PREVIOUS RUN means the game really died");
    else
        Log("Exit marker: TerminateProcess could not be wrapped; PREVIOUS RUN cannot be trusted");
    // The game folder is two levels above Mods\\Plugins, where this DLL sits.
    {
        wchar_t gameFolder[MAX_PATH]{};
        if (SiblingPath(gameFolder, L".dll")) {
            for (int up = 0; up < 3; ++up)
                if (wchar_t* slash = wcsrchr(gameFolder, L'\\')) *slash = 0;
            StartUpdateCheck(gameFolder, checkUpdates);
        }
    }
    if (crashLog) {
        wchar_t dumpPath[MAX_PATH]{};
        const bool wantDump = crashDump && SiblingPath(dumpPath, L"-crash.dmp");
        InstallCrashLog(game, wantDump ? dumpPath : nullptr);
        if (CrashDumpArmed())
            Log("Crash log armed; CrashDump=1, so the first access violation in EDF.dll or the plugin also writes %ls "
                "(one per launch, overwritten each time). It holds process memory, so only send it on "
                "purpose", dumpPath);
        else if (crashDump)
            Log("Crash log armed; CrashDump=1 but no dump can be written (DbgHelp.dll or the path was "
                "not usable)");
        else
            Log("Crash log armed; no crash dump (CrashDump is off by default because a dump holds "
                "process memory; set CrashDump=1 in the INI to help chase a crash)");
    }
    return true;  // stays loaded: call stubs, import wrappers and the exception handler point here
}
}  // namespace


// The version as the updater checks it inside a downloaded file (exported, so the linker keeps it).
// package.ps1 and sign-update.ps1 check it.
extern "C" __declspec(dllexport) const char EDF6CoopVersion[] = EDF6COOP_MARKER_PREFIX EDF6COOP_VERSION_TEXT;
// And as each 2.2.x updater checks it, one per room size it was built for (product.h), each followed by a NUL,
// so every earlier install updates to this build.
#define EDF6COOP_LEGACY_MARKER(n) "EDF6COOP_" #n "P_VERSION=" EDF6COOP_VERSION_TEXT "\0"
extern "C" __declspec(dllexport) const char EDF6CoopLegacyVersions[] = EDF6COOP_LEGACY_SIZES(EDF6COOP_LEGACY_MARKER);

namespace {

// This DLL's path while its version is on trial after an update (never destroyed): a game that ends the normal
// way says so, so that quitting early is not taken for a crash (dn::noteCleanExit).
std::wstring* onTrial = nullptr;

// Everything up to the parts: the log, the plugins EDF6Coop replaces, the settings file, and the update state.
// Then the direct-link part, then the room part (the order the two separate plugins loaded in, which is the
// order their wrappers of the game's EOS imports were built in). Returns whether anything runs.
bool LoadCoop(PluginInfo* info) {
    using namespace multislot;
    if (!info) return false;
    wchar_t iniPath[MAX_PATH]{}, logPath[MAX_PATH]{}, dllPath[MAX_PATH]{};
    if (!SiblingPath(iniPath, L".ini") || !SiblingPath(logPath, L".log") || !SiblingPath(dllPath, L".dll"))
        return false;
    LogOpen(logPath);
    dn::logToSink(&ForwardDirectNetLine);
    dn::statusToSink(&SetPluginStatus);
    info->infoVersion = PluginInfo::MaxInfoVer;
    info->name = "EDF6Coop";
    info->version = PLUG_VER(MULTISLOT_VERSION_MAJOR, MULTISLOT_VERSION_MINOR, MULTISLOT_VERSION_PATCH, 0);

    Log("==== EDF6Coop %s ====", kVersion);
    const auto loader = GetModuleHandleW(L"winmm.dll");
    for (const auto name : {"timeBeginPeriod", "timeEndPeriod", "PlaySoundW"})
        Log("LOADER %s proxy=%s", name, LoaderProxyStyle(loader ? reinterpret_cast<const void*>(GetProcAddress(loader, name)) : nullptr));
    // Whether the game that wrote the lines above this one was closed or died. Without it a log that simply
    // stops says nothing, which is where two of the 2026-09-19 reports ran out of evidence.
    if (PreviousRun() == LastRun::Cut)
        Log("PREVIOUS RUN ended without a shutdown line: the game was killed, hung, or died without reaching "
            "the crash handler. Lines above this one are its last");

    // Before any file is touched: outside the game nothing is renamed or written.
    const HMODULE game = GetModuleHandleW(L"EDF.dll");
    if (!game) {
        Log("REFUSED: EDF.dll is not loaded, so this is not the game; nothing was changed");
        return false;
    }
    const std::wstring plugin = dllPath, dir = plugin.substr(0, plugin.find_last_of(L"\\/") + 1);
    if (!RetireReplacedPlugins(dir)) return false;
    PrepareSettings(dir, iniPath, kDefaultIni);
    const dn::Config settings = dn::loadConfig(iniPath);
    // Before anything that could fail in a new version: a version whose previous run crashed is replaced by the
    // one before it, and this session runs without the plugin.
    const dn::RunState run = dn::beginRun(plugin, kVersion);
    if (run == dn::RunState::RolledBack) return false;

    const dn::PartState direct = dn::startPart(settings, dir, game, GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll"));
    const bool rooms = LoadRooms(iniPath);
    // Last, so its wrappers sit in front of both parts': it answers a room Epic knows nothing of. The room
    // part reads room-list entries through the direct-link part before any entry of its own can be there.
    if (direct.eosHooked) {
        multislot::RouteLobbyInfo(&dn::lobbyInfoCopy, &dn::lobbyInfoRelease);
        dn::startRejoin(game);
    }
    if (!direct.running && !rooms) {
        Log("Neither part runs (see above why): the game goes on without EDF6Coop");
        // Not a failed run of this version: the next start goes on with its trial instead of rolling it back.
        if (run == dn::RunState::Trial) dn::noteCleanExit(plugin, kVersion);
        return false;
    }
    if (run == dn::RunState::Trial) {
        onTrial = new std::wstring(plugin);
        dn::startHealthWatch(plugin, kVersion);
        // Without the EOS hooks no EOS tick says the game is up; the trial then counts from here.
        if (!direct.eosHooked) dn::noteGameRunning();
    }
    dn::startAutoUpdate(plugin, kVersion, settings.autoUpdate);
    if (!settings.autoUpdate) Log("UPDATE automatic updates are off ([Update] AutoUpdate=0)");
    return true;  // stays loaded: call stubs, import wrappers, threads and the exception handler point here
}

}  // namespace

// Read-only, for the game-code tests (tests/gamenet): where the game's patched code finds the loadout record of
// player `index` - its own GameStatus records for 1-4, a sidecar of this plugin from 5 on.
extern "C" __declspec(dllexport) const std::uint8_t* EDF6Coop_LoadoutRecord(int index) {
    return multislot::LoadoutRecord(index);
}

// Read-only, for the game-code tests (tests/gamenet): the slot the room's host's game has `member` in, as this machine
// follows it (-1: not following the host's slots, or the host's list does not have it).
extern "C" __declspec(dllexport) int EDF6Coop_HostSlot(const char* member) {
    return member ? dn::hostSlotOf(member) : -1;
}

// For the game-code tests (tests/gamenet): sends `size` bytes to `remote` in bulk (netfeature.h SendBulk).
extern "C" __declspec(dllexport) bool EDF6Coop_SendBulk(const char* remote, std::uint16_t tag, const void* data,
                                                        std::size_t size) {
    return remote && multislot::SendBulk(remote, tag, data, size);
}

extern "C" __declspec(dllexport) bool EDFMLAPI EML6_Load(PluginInfo* info) {
    // Every line so far was written by this thread, so the startup report is on disk already.
    const bool loaded = LoadCoop(info);
    if (loaded) {
        // Staying for the life of the process: from here on the writer thread keeps up, and the game's threads
        // never wait on the disk.
        multislot::LogStartWriter();
    } else {
        // EDFModLoader unloads a plugin that refuses (FreeLibrary as soon as this returns). 1.5.13 had started
        // the writer thread with the first line, and it woke up inside the unmapped DLL about 200 ms later and
        // took the game down with it. No thread was started, and the queue file is let go here.
        dn::logToSink(nullptr);
        multislot::LogClose();
    }
    return loaded;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        multislot::self = instance;
        DisableThreadLibraryCalls(instance);
    }
    // `reserved` is null when the DLL is unloaded (FreeLibrary after a refusal) and set when the process ends:
    // only the second is the game exiting, through ExitProcess, which is how EDF6 quits from its menu (measured).
    if (reason == DLL_PROCESS_DETACH) {
        // At exit the direct link's worker thread is already killed (possibly holding a lock) and static objects
        // are about to be destroyed, while EDF.dll may still call EOS through the hooks. Never wait here.
        dn::detachPart();
        if (reserved && onTrial) dn::noteCleanExit(*onTrial, multislot::kVersion);
        // The last thing the log gets from this run. Both are raw Win32 with no heap and no CRT, which is what
        // makes them safe this late.
        if (reserved)
            multislot::LogShutdown("the game exited");
        else
            multislot::LogUnloaded("the plugin was unloaded (see above why); the game goes on without it");
    }
    return TRUE;
}
