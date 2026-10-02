// The direct-link part of EDF6Coop: direct UDP links between the room's players, reliable game traffic and
// held disconnects, all through EDF.dll's EOS imports. The plugin entry (multislot/src/plugin.cpp) starts it
// once the game is known to be there, before the room part, so the room part's import wrappers sit in front
// of these, as when the two were separate plugins loaded in name order.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

#include "config.h"

namespace dn {

struct PartState {
    bool running = false;    // it started something, which stays for the life of the process
    bool eosHooked = false;  // the game's EOS calls go through it: its EOS ticks report the game running
};
// Starts the part with `settings` (loadConfig of EDF6Coop.ini); `dir` is the plugin folder, with a trailing
// backslash. `eos` may be null (no EOS SDK in the process): then nothing starts.
PartState startPart(const Config& settings, const std::wstring& dir, HMODULE game, HMODULE eos);
// DLL_PROCESS_DETACH: EDF.dll may still call EOS through the hooks while static objects go away.
void detachPart();

// Coming back into the room we were last in without Epic (eos_hooks.h installVirtualRoomHooks): after the room
// part installed its wrappers, so these sit in front of them. False: unavailable (the part does not run).
bool startRejoin(HMODULE game);
// EOS_LobbyDetails_CopyInfo / EOS_LobbyDetails_Info_Release that also answer the room list entries of
// startRejoin, for code that reads the game's LobbyDetails handles itself. Only once startRejoin succeeded.
int32_t lobbyInfoCopy(void* details, const void* options, void** info);
void lobbyInfoRelease(void* info);
// Whether `remote` (an EOS_ProductUserId) plays with us over a direct link now: a build of the same direct-link
// protocol, which reads a split mission start message.
bool readsSplitSyncDirectly(const void* remote);

}  // namespace dn
