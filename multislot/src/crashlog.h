#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace multislot {

// Records the first few crash-class exceptions (code, faulting address, registers and the
// unwound call stack as module+offset) in the plugin log. It never handles an exception, so
// the game behaves exactly as without it; the log is what a 5-player test run leaves behind.
//
// `dumpPath` (may be null) is where the first access violation inside EDF.dll or this plugin writes a
// minidump. The 2026-09-27 host crash (EDF+12B44FC, an empty shared_ptr<eos::Serialize> copied in the host's
// mission sync) is a null the log cannot explain: the object it came from is a stack local, so its
// frame has to be looked at. One dump per launch, that one file overwritten, and only for an access
// violation whose faulting instruction is in EDF.dll or in the plugin - a first-chance fault in another
// module (the VR mod records several the game survives) writes nothing, and neither does one of the
// plugin's own probes (Probing below). The dump is written by a thread of its own that exists from here
// on, while the faulting thread waits for it.
void InstallCrashLog(HMODULE game, const wchar_t* dumpPath);

// True when a minidump will be written for the first access violation in EDF.dll or the plugin.
bool CrashDumpArmed();

// Faults the plugin expects and catches itself: it reads game memory that may not be there and calls game
// functions under __try, and the crash handler, a vectored handler, sees every such fault before the __except
// does. Without being told, it reported a probe that faulted inside EDF.dll as the game's first access violation
// and spent the one crash dump on it. Per thread; a probe inside a probe counts once.
inline thread_local int probeDepth = 0;
// Runs `probe` - a function whose own __try catches the faults it expects - with the crash handler looking away.
template <typename Probe>
auto Probing(Probe probe) -> decltype(probe()) {
    struct Depth {
        Depth() { ++probeDepth; }
        ~Depth() { --probeDepth; }
    } depth;
    return probe();
}
// True while this thread is inside Probing (the crash handler; tests).
inline bool InProbe() { return probeDepth > 0; }

}  // namespace multislot
