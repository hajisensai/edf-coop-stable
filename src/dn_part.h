// The direct-link part of EDF6Coop: direct UDP links between the room's players, reliable game traffic and
// held disconnects, all through EDF.dll's EOS imports. The plugin entry (multislot/src/plugin.cpp) starts it
// once the game is known to be there, before the room part, so the room part's import wrappers sit in front
// of these, as when the two were separate plugins loaded in name order.
#pragma once
#include <windows.h>

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

}  // namespace dn
