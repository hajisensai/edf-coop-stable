#pragma once
#include <cstddef>
#include <cstdint>

namespace multislot {

// Append-only text log. Safe to call from an exception handler: no heap, no CRT file locks.
// Lines are queued and a writer thread appends them to the file (at the latest every 200 ms), so logging never
// waits on the disk on the thread that logs - the game's UI thread and the EOS callbacks among them. A burst that
// fills the 256 KB queue is written by the thread that logs rather than dropped. Until LogStartWriter every line
// is written by the thread that logs it.
//
// The queue is a mapping of <log>.queue (LogQueueHeader), so a queued line is in that file's pages at once, and
// the system writes a mapped file's pages out even when the process is killed. A death that skips every handler
// - __fastfail, a kill from Task Manager, TerminateProcess from another module - used to lose the last 200 ms of
// lines, which are the ones such a death is diagnosed by; the next LogOpen now appends them to the log first.
// Without the file (another game has it open, or it cannot be made) the queue is in memory, as before.
void LogOpen(const wchar_t* path);
void LogWrite(const char* text, std::size_t length);
// One call is one line: control characters in the formatted text (a member's name, an EOS message) are
// written as '?', so nothing logged can end the line early or start one that reads as the plugin's own.
void Log(const char* format, ...);
// Writes everything queued so far to the file on the calling thread, and returns when it is there. Crash
// reports use it before anything that may not finish (a dump), and so do tests before reading the file.
void LogFlush();
// Starts the writer thread. Only once the plugin has committed to staying loaded (plugin.cpp EML6_Load): a
// thread running this module's code must never outlive it, and EDFModLoader unloads a plugin that refuses.
void LogStartWriter();
// Writes everything queued and lets go of <log>.queue, for a plugin that is about to be unloaded. Lines logged
// afterwards are still written, by the thread that logs them.
void LogClose();
// How many times the log file has been opened for writing (tests: queued lines go out in batches).
std::size_t LogFileOpens();
// Bytes logged and not yet written to the log file (tests).
std::size_t LogQueued();

// <log>.queue: this header, then a ring of kLogQueueBytes. Byte n of everything queued is ring[n %
// kLogQueueBytes]; [consumed, produced) has been logged and is not yet known to be in the log. While an append
// is under way, `appending` bytes from `appendingFrom` are going to the log, which held `appendingAt` bytes when
// it started, so the next start can tell how much of an interrupted append got there.
struct LogQueueHeader {
    char magic[8];
    std::uint64_t produced;
    std::uint64_t consumed;
    std::uint64_t appending;
    std::uint64_t appendingFrom;
    std::int64_t appendingAt;
};
constexpr std::size_t kLogQueueBytes = 256 * 1024;
constexpr char kLogQueueMagic[8] = {'M', 'S', 'L', 'O', 'G', 'Q', '1', 0};
inline constexpr wchar_t kLogQueueSuffix[] = L".queue";

// The log is one file that never grows much past kLogCapBytes: when it is opened or a write takes it over the
// cap, the oldest lines are dropped so that about the newest kLogKeepBytes remain, starting at a line boundary
// after a note that lines were dropped. Measured 2026-09-18: a start is about 3 KB, 30-40 minutes in a
// five-player room with NetLog=1 about 30-80 KB, a crash report 1-2 KB. 2 MB keeps the last few evenings (the
// crashed start survives many restarts), stays small enough to attach in a chat (Discord's free limit is
// 10 MB), and a trim during play copies at most 1.5 MB.
constexpr long long kLogCapBytes = 2LL * 1024 * 1024;
constexpr long long kLogKeepBytes = 1536LL * 1024;
// Trims the file at `path` that way when it is larger than `cap` (the log's own writes wait meanwhile);
// true when it did.
bool TrimLogFile(const wchar_t* path, long long cap, long long keep);

// How the run before this one ended, worked out in LogOpen by looking at what the file already holds:
// whichever of the startup banner and the SHUTDOWN line comes last in it. Two machines died mid-mission on
// 2026-09-19 with no exception and no shutdown, and nothing in the file could say whether they had crashed
// or simply been closed - this is what answers that next time.
enum class LastRun {
    Unknown,   // no log yet, it was trimmed past both marks, or the plugin was unloaded in that run: say nothing
    Ended,     // a SHUTDOWN line after the last banner: the game was closed normally
    Cut,       // a banner with no SHUTDOWN after it: killed, hung, or died without reaching the handler
};
LastRun PreviousRun();
// Written from DLL_PROCESS_DETACH when the process ends, or from the TerminateProcess wrapper. Raw Win32, no
// heap and no CRT, so it is safe that late.
void LogShutdown(const char* why);
// Written from DLL_PROCESS_DETACH when the plugin is unloaded while the game goes on (it refused, or it is
// disabled): the UNLOADED line. Nothing of this run's end is seen after it, so the next start says nothing
// about it rather than calling it cut - and SHUTDOWN, which 1.5.13 wrote here, would claim the game had exited.
void LogUnloaded(const char* why);

// Research lines that repeat during play (lobby and P2P events, room capacity checks, enemy count scaling) are
// written only with [MultiSlot] NetLog=1 (the default since 1.2.2); startup, menu and crash lines always are.
void SetDetailLog(bool on);
bool DetailLog();

}  // namespace multislot
