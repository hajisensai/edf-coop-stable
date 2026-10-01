// EDF6Coop: the direct link (src/) and the room/mission part (multislot/src/) as one plugin, EDF6Coop.dll.
// Each room size is a build of its own (MULTISLOT_MAX_PLAYERS: 8, 10 or 12), and its release asset and version
// marker name the size, so the updater of one size can never install another.
#pragma once

#ifndef MULTISLOT_MAX_PLAYERS
#define MULTISLOT_MAX_PLAYERS 8  // the direct-link tests build without CMake
#endif

#define EDF6COOP_TEXT2(x) #x
#define EDF6COOP_TEXT(x) EDF6COOP_TEXT2(x)

namespace coop {

constexpr const char* kName = "EDF6Coop";
constexpr const wchar_t* kNameW = L"EDF6Coop";
// The plugin's own file; settings and log sit next to it as EDF6Coop.ini and EDF6Coop.log.
constexpr const wchar_t* kDllFileW = L"EDF6Coop.dll";
// "EDF6Coop-12p": the release assets of this room size are <kVariant>.dll and <kVariant>.dll.sig.
constexpr const char* kVariant = "EDF6Coop-" EDF6COOP_TEXT(MULTISLOT_MAX_PLAYERS) "p";
// Followed by x.y.z and a NUL inside every build (plugin.cpp exports it); the updater accepts a download only
// when it carries this marker for the signed version, which also pins the room size.
#define EDF6COOP_MARKER_PREFIX "EDF6COOP_" EDF6COOP_TEXT(MULTISLOT_MAX_PLAYERS) "P_VERSION="
constexpr const char* kVersionMarkerPrefix = EDF6COOP_MARKER_PREFIX;
// The plugins EDF6Coop replaces. Running next to either would hook the same game code twice.
constexpr const wchar_t* kReplacedPlugins[] = {L"EDF6DirectNet.dll", L"EDF6MultiSlot.dll"};
// Their settings files, read once to carry the settings into EDF6Coop.ini (left in place afterwards).
constexpr const wchar_t* kReplacedSettings[] = {L"EDF6DirectNet.ini", L"EDF6MultiSlot.ini"};

}  // namespace coop
