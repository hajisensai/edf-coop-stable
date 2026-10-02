// EDF6Coop: the direct-link part (src/) and the room/mission part (multislot/src/) as one plugin, EDF6Coop.dll.
// Since 2.3.0 one build serves every room size (slots for 32 players, the size chosen by the host when creating a
// room); up to 2.2.x each size was a build of its own, whose updater fetches EDF6Coop-<n>p.dll and accepts it only
// when it carries EDF6COOP_<n>P_VERSION=x.y.z. A release publishes this build under each of those names too and it
// carries every one of those markers (plugin.cpp), so every earlier install updates to it.
#pragma once

#define EDF6COOP_TEXT2(x) #x
#define EDF6COOP_TEXT(x) EDF6COOP_TEXT2(x)
// The room sizes built separately up to 2.2.x: X(n) for each. Kept as long as such installs may still update.
#define EDF6COOP_LEGACY_SIZES(X) X(8) X(10) X(12) X(16) X(24) X(32)

namespace coop {

constexpr const char* kName = "EDF6Coop";
constexpr const wchar_t* kNameW = L"EDF6Coop";
// The plugin's own file; settings and log sit next to it as EDF6Coop.ini and EDF6Coop.log.
constexpr const wchar_t* kDllFileW = L"EDF6Coop.dll";
// The release assets this build updates from: <kVariant>.dll and <kVariant>.dll.sig.
constexpr const char* kVariant = "EDF6Coop";
// Followed by x.y.z and a NUL inside every build (plugin.cpp exports it); the updater accepts a download only
// when it carries this marker for the signed version. No earlier build has it, so none can be installed back.
#define EDF6COOP_MARKER_PREFIX "EDF6COOP_VERSION="
constexpr const char* kVersionMarkerPrefix = EDF6COOP_MARKER_PREFIX;
// The plugins EDF6Coop replaces. Running next to either would hook the same game code twice.
constexpr const wchar_t* kReplacedPlugins[] = {L"EDF6DirectNet.dll", L"EDF6MultiSlot.dll"};
// Their settings files, read once to carry the settings into EDF6Coop.ini (left in place afterwards).
constexpr const wchar_t* kReplacedSettings[] = {L"EDF6DirectNet.ini", L"EDF6MultiSlot.ini"};

}  // namespace coop
