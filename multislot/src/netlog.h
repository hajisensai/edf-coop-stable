#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace multislot {

// Wrappers on EDF.dll's EOS imports. Except for the explicitly scoped recovery send,
// calls and results pass through unchanged. Also routes the EOS SDK's Lobby/P2P/RTC log lines (which the
// game discards: its callback is an empty function) into the plugin log.
// Returns the number of import slots redirected.
int InstallNetLog(HMODULE game, bool diagnostics, bool recovery);

// Call site EDF+12D5B9B only: one final hello when leaving Link::OnInitial.
void FinalHelloHook(void* manager, const void* peer, const char* token);
// The game function FinalHelloHook passes the call on to. Set before the call site is redirected (plugin.cpp
// Apply writes the redirect; InstallNetLog runs only after that), so a hello sent in between reaches the game.
void InitFinalHello(const unsigned char* gameBase);

// EDF.dll ends the game by calling TerminateProcess on itself, which skips every DLL_PROCESS_DETACH,
// so the SHUTDOWN line added in 1.5.2 was never written once and every start reported the previous run
// as cut. Wrapping that import writes the line just before the process goes, and leaves a crash or a
// kill (which never reach it) correctly unmarked. True when the import was found and redirected.
bool InstallExitMarker(HMODULE game);

// Points EDF.dll's import `function` of `dll` at `replacement`. `*original` gets the previous target before the
// slot changes; a second redirect of the same slot wraps the first.
bool RedirectGameImport(HMODULE game, const char* dll, const char* function, void* replacement, void** original);

}  // namespace multislot
