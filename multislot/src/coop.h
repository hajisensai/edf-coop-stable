// What EDF6Coop does as one plugin before either part starts: it takes over from EDF6DirectNet.dll and
// EDF6MultiSlot.dll, and it keeps one settings file and one log for both parts.
#pragma once

#include <string>

namespace multislot {

// EDF6DirectNet.dll and EDF6MultiSlot.dll in `dir` (the plugin folder, trailing backslash) are renamed to
// <name>.disabled, so they no longer load. EDFModLoader loads Mods\Plugins in name order and EDF6Coop.dll
// comes first, so this normally happens before it reaches them. False when one of them is loaded in this game
// already or could not be renamed: running next to it would hook the same game code twice, so EDF6Coop must
// then stay off (the log says what to do).
bool RetireReplacedPlugins(const std::wstring& dir);

// Writes EDF6Coop.ini (`iniPath`) when there is none: the documented defaults of both parts as one file
// (`roomDefaults` is the room part's), with every setting found in the old EDF6DirectNet.ini and
// EDF6MultiSlot.ini carried over. The old files are only read. Also moves the old UPnP record to its new name.
void PrepareSettings(const std::wstring& dir, const std::wstring& iniPath, const char* roomDefaults);

// The direct-link part's log lines go into this log (dn::logToSink), marked "[DN] ".
void ForwardDirectNetLine(const char* line);

}  // namespace multislot
