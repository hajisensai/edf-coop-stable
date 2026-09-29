#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace multislot {
namespace {
wchar_t logPath[MAX_PATH]{};
std::atomic<bool> detailLog{false};

// The two marks that say where one run of the game ends and the next begins, as plugin.cpp and
// LogShutdown write them. Checked against the real banner by the tests. They count only as the first thing
// after a line's own timestamp: text inside a line (a member's name, an EOS message) cannot fake one.
constexpr const char* kBannerMark = "==== EDF6MultiSlot ";
constexpr const char* kShutdownMark = "SHUTDOWN ";
// "[2026-09-20 00:09:00.000] ": what every line the plugin writes starts with.
constexpr std::size_t kStampLength = 26;
LastRun lastRun = LastRun::Unknown;
std::atomic<bool> shutdownWritten{false};

// A line that repeats (some EOS SDK warnings arrive every frame) is written once and then counted, so one
// noisy source cannot push everything else out of the log.
SRWLOCK repeatLock = SRWLOCK_INIT;
char lastBody[512]{};
std::size_t lastBodyLength = 0;
std::uint64_t repeats = 0;

// Lines are queued here and written to the file by a writer thread, so the game's threads never wait on the
// disk: before 1.5.13 every line opened, appended to and closed the file on the thread that logged it - the
// game's UI thread and the EOS callbacks among them - and a trim copied 1.5 MB right there. The queue is
// static (no heap), so a line can still be queued from an exception handler.
constexpr std::size_t kQueueBytes = 256 * 1024;
SRWLOCK queueLock = SRWLOCK_INIT;
char queue[kQueueBytes];
std::size_t queued = 0;

// Held while a batch goes to the file and while the file is trimmed, so batches land in the order they were
// queued. The thread holding it is remembered: a crash reported from inside a write or a trim writes without
// it instead of deadlocking on the lock it already holds.
SRWLOCK drainLock = SRWLOCK_INIT;
std::atomic<DWORD> drainThread{0};
char batch[kQueueBytes];  // used only under drainLock

// The writer thread wakes when the queue is half full, and otherwise drains it this often.
constexpr DWORD kDrainIntervalMs = 200;
std::atomic<HANDLE> wakeWriter{nullptr};
std::atomic<bool> writerStarted{false};
// False until the writer thread exists; without one every line is written by the thread that logs it.
std::atomic<bool> writerRunning{false};
std::atomic<std::size_t> fileOpens{0};

// Used only under drainLock: trimming needs no heap.
char trimBuffer[256 * 1024];
constexpr char kDroppedNote[] = "[... older lines were dropped to keep this log under 2 MB ...]\r\n";

bool ReadAt(HANDLE file, LONGLONG offset, char* buffer, DWORD size, DWORD& read) {
    LARGE_INTEGER at{};
    at.QuadPart = offset;
    read = 0;
    return SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, buffer, size, &read, nullptr);
}

bool WriteAt(HANDLE file, LONGLONG offset, const char* buffer, DWORD size) {
    LARGE_INTEGER at{};
    at.QuadPart = offset;
    DWORD written = 0;
    return SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && WriteFile(file, buffer, size, &written, nullptr) && written == size;
}

// Caller holds the exclusive lock. Copies the newest part to the front of the same file, so a viewer that
// keeps the log open does not stop the trim.
bool TrimLocked(const wchar_t* path, LONGLONG cap, LONGLONG keep) {
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool trimmed = false;
    if (GetFileSizeEx(file, &size) && size.QuadPart > cap && keep > 0 && keep < size.QuadPart) {
        // Start at the first line that begins inside the kept part, so the log starts with a whole line.
        const LONGLONG scan = size.QuadPart - keep - 1;
        LONGLONG from = scan + 1;
        DWORD read = 0;
        if (ReadAt(file, scan, trimBuffer, sizeof(trimBuffer), read)) {
            const auto newline = static_cast<const char*>(std::memchr(trimBuffer, '\n', read));
            if (newline) from = scan + (newline - trimBuffer) + 1;
        }
        const DWORD note = static_cast<DWORD>(sizeof(kDroppedNote) - 1);
        if (WriteAt(file, 0, kDroppedNote, note)) {
            LONGLONG to = note;
            while (from < size.QuadPart && ReadAt(file, from, trimBuffer, sizeof(trimBuffer), read) && read > 0 &&
                   WriteAt(file, to, trimBuffer, read)) {
                from += read;
                to += read;
            }
            // Everything before `to` is a whole log even if a read or write failed on the way: cut there.
            LARGE_INTEGER end{};
            end.QuadPart = to;
            trimmed = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
        }
    }
    CloseHandle(file);
    return trimmed;
}

// Appends `text` to the log and returns the file's size afterwards (0 when it could not be written).
LONGLONG AppendToFile(const char* text, std::size_t length) {
    fileOpens.fetch_add(1);
    HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) size.QuadPart = 0;
    CloseHandle(file);
    return size.QuadPart;
}

// Caller holds drainLock (or is the thread that does). Writes out everything queued so far, one batch at a time.
void DrainHeld() {
    for (;;) {
        AcquireSRWLockExclusive(&queueLock);
        const std::size_t length = queued;
        if (length) std::memcpy(batch, queue, length);
        queued = 0;
        ReleaseSRWLockExclusive(&queueLock);
        if (!length || !logPath[0]) return;
        if (AppendToFile(batch, length) > kLogCapBytes) TrimLocked(logPath, kLogCapBytes, kLogKeepBytes);
    }
}

// Runs `work` under drainLock, or straight away on the thread that already holds it.
template <typename Work>
void WithDrainLock(Work work) {
    const DWORD self = GetCurrentThreadId();
    if (drainThread.load() == self) {
        work();
        return;
    }
    AcquireSRWLockExclusive(&drainLock);
    drainThread.store(self);
    work();
    drainThread.store(0);
    ReleaseSRWLockExclusive(&drainLock);
}

void Drain() { WithDrainLock(&DrainHeld); }

DWORD WINAPI WriterMain(void*) {
    const HANDLE wake = wakeWriter.load();
    for (;;) {
        WaitForSingleObject(wake, kDrainIntervalMs);
        Drain();
    }
}

// Started by the first line, not at load: LogOpen may run where a new thread must not be waited on, and
// nothing here ever waits for this one (LogFlush drains on the calling thread). When it cannot be started,
// writerRunning stays false and every line is written where it is logged, as before 1.5.13.
void StartWriter() {
    if (writerStarted.load() || writerStarted.exchange(true)) return;
    const HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) return;
    wakeWriter.store(wake);
    if (HANDLE thread = CreateThread(nullptr, 64 * 1024, &WriterMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
        CloseHandle(thread);
        writerRunning.store(true);
    }
}

// The thread that holds drainLock logging again can only be a crash reported from inside a write or a trim:
// the batch and the trim it interrupted are still in use, so it must not drain or trim again.
bool InsideDrain() { return drainThread.load() == GetCurrentThreadId(); }
}  // namespace

void SetDetailLog(bool on) { detailLog.store(on); }
bool DetailLog() { return detailLog.load(); }

bool TrimLogFile(const wchar_t* path, long long cap, long long keep) {
    bool trimmed = false;
    WithDrainLock([&] { trimmed = TrimLocked(path, cap, keep); });
    return trimmed;
}

void LogFlush() {
    if (InsideDrain()) return;  // a crash inside a write: its own lines went straight to the file (LogWrite)
    Drain();
}

std::size_t LogFileOpens() { return fileOpens.load(); }

// True when `line` starts with the timestamp Log and LogShutdown write, "[YYYY-MM-DD HH:MM:SS.mmm] ".
bool Stamped(const char* line, const char* end) {
    constexpr const char* kShape = "[0000-00-00 00:00:00.000] ";
    if (static_cast<std::size_t>(end - line) < kStampLength) return false;
    for (std::size_t i = 0; i < kStampLength; ++i) {
        const bool digit = kShape[i] == '0';
        if (digit ? (line[i] < '0' || line[i] > '9') : line[i] != kShape[i]) return false;
    }
    return true;
}

bool Marked(const char* line, const char* end, const char* mark) {
    const std::size_t length = std::strlen(mark);
    return Stamped(line, end) && static_cast<std::size_t>(end - line) >= kStampLength + length &&
           std::memcmp(line + kStampLength, mark, length) == 0;
}

// Looks at the file this session is about to append to and reports how the run before it ended. The file
// is capped at 2 MB, so it is read whole; this runs once, at load.
LastRun ReadLastRun(const wchar_t* path) {
    LastRun verdict = LastRun::Unknown;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return verdict;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 4LL * 1024 * 1024) {
        const auto bytes = static_cast<std::size_t>(size.QuadPart);
        if (char* text = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, bytes + 1))) {
            DWORD read = 0;
            if (ReadFile(file, text, static_cast<DWORD>(bytes), &read, nullptr) && read) {
                text[read] = 0;
                // Whichever mark is last in the file is the one that describes the previous run.
                const char* banner = nullptr;
                const char* shutdown = nullptr;
                const char* const end = text + read;
                for (const char* line = text; line < end;) {
                    const char* next = static_cast<const char*>(std::memchr(line, '\n', static_cast<std::size_t>(end - line)));
                    next = next ? next + 1 : end;
                    if (Marked(line, next, kBannerMark)) banner = line;
                    else if (Marked(line, next, kShutdownMark)) shutdown = line;
                    line = next;
                }
                // A missing SHUTDOWN only means the run was cut if this build can write one at all. On
                // 2026-09-29 it still could not - DLL_PROCESS_DETACH does not run for this game, and
                // wrapping TerminateProcess did not catch it either - so a file that has never held the
                // mark says nothing, and claiming otherwise called every ordinary quit a crash.
                if (shutdown) verdict = (!banner || shutdown > banner) ? LastRun::Ended : LastRun::Cut;
            }
            HeapFree(GetProcessHeap(), 0, text);
        }
    }
    CloseHandle(file);
    return verdict;
}

void LogOpen(const wchar_t* path) {
    // Lengths are checked by hand: *_s string functions end the process when something does not fit.
    const std::size_t length = wcslen(path);
    if (length >= MAX_PATH) return;
    // Lines queued for the previous file go there first.
    WithDrainLock([&] {
        DrainHeld();
        wmemcpy(logPath, path, length + 1);
        // One file across sessions, so a crash report survives restarts; only its oldest lines go.
        TrimLocked(logPath, kLogCapBytes, kLogKeepBytes);
    });
    lastRun = ReadLastRun(logPath);
}

LastRun PreviousRun() { return lastRun; }

void LogShutdown(const char* why) {
    if (shutdownWritten.exchange(true)) return;
    // This runs from DLL_PROCESS_DETACH, where every other thread has already been terminated - one of them
    // possibly while holding the log's locks - or from the TerminateProcess hook, where the writer thread still
    // runs. No repeat collapsing, no trim, no heap: a hang at exit would be blamed on the mod.
    if (!logPath[0]) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[256]{};
    const int length = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] SHUTDOWN %s\r\n",
                                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                                   now.wMilliseconds, why);
    if (length <= 0) return;
    // Queued lines first, then this one, both under drainLock when it can be had, so a trim by the writer
    // thread can neither cut the shutdown line nor be under way when it is written. A thread terminated while
    // holding the lock never lets go, so after about 100 ms the line is written without it.
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (TryAcquireSRWLockExclusive(&drainLock)) {
            drainThread.store(GetCurrentThreadId());
            if (TryAcquireSRWLockExclusive(&queueLock)) {
                if (queued) AppendToFile(queue, queued);
                queued = 0;
                ReleaseSRWLockExclusive(&queueLock);
            }
            AppendToFile(line, static_cast<std::size_t>(length));
            drainThread.store(0);
            ReleaseSRWLockExclusive(&drainLock);
            return;
        }
        Sleep(2);
    }
    AppendToFile(line, static_cast<std::size_t>(length));
}

void LogWrite(const char* text, std::size_t length) {
    if (!logPath[0] || !length) return;
    if (InsideDrain()) {
        AppendToFile(text, length);  // no batch, no trim: the interrupted drain still owns both
        return;
    }
    StartWriter();
    if (length > kQueueBytes) {
        // Larger than the whole queue: behind everything already queued, straight to the file.
        WithDrainLock([&] {
            DrainHeld();
            if (AppendToFile(text, length) > kLogCapBytes) TrimLocked(logPath, kLogCapBytes, kLogKeepBytes);
        });
        return;
    }
    for (;;) {
        AcquireSRWLockExclusive(&queueLock);
        if (queued + length <= kQueueBytes) {
            std::memcpy(queue + queued, text, length);
            queued += length;
            const bool wake = queued >= kQueueBytes / 2;
            ReleaseSRWLockExclusive(&queueLock);
            if (!writerRunning.load()) {
                Drain();  // no writer thread: written here, as before 1.5.13
            } else if (wake) {
                SetEvent(wakeWriter.load());
            }
            return;
        }
        ReleaseSRWLockExclusive(&queueLock);
        // The queue is full (a burst the writer has not caught up with): drain it here rather than lose a line.
        Drain();
    }
}

void Log(const char* format, ...) {
    char line[1024];
    SYSTEMTIME now{};
    GetLocalTime(&now);
    int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", now.wYear, now.wMonth,
                           now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    if (used < 0) used = 0;
    va_list args;
    va_start(args, format);
    int body = _vsnprintf_s(line + used, sizeof(line) - used, _TRUNCATE, format, args);
    va_end(args);
    std::size_t length = body < 0 ? sizeof(line) - 1 : static_cast<std::size_t>(used + body);
    if (length > sizeof(line) - 3) length = sizeof(line) - 3;
    // One call, one line: text that comes from elsewhere (member names, EOS messages) may hold CR, LF or other
    // controls, which would end this line early and start one that reads as the plugin's own.
    for (std::size_t i = static_cast<std::size_t>(used); i < length; ++i) {
        const auto c = static_cast<unsigned char>(line[i]);
        if ((c < 0x20 && c != '\t') || c == 0x7F) line[i] = '?';
    }

    // The same line again only counts; the count is written when a different line follows.
    const std::size_t bodyLength = length - static_cast<std::size_t>(used);
    AcquireSRWLockExclusive(&repeatLock);
    if (bodyLength && bodyLength == lastBodyLength && std::memcmp(lastBody, line + used, bodyLength) == 0) {
        ++repeats;
        ReleaseSRWLockExclusive(&repeatLock);
        return;
    }
    const std::uint64_t pending = repeats;
    char repeatedBody[128]{};
    if (pending && lastBodyLength) {
        const std::size_t copied = lastBodyLength < sizeof(repeatedBody) - 1 ? lastBodyLength : sizeof(repeatedBody) - 1;
        std::memcpy(repeatedBody, lastBody, copied);
    }
    repeats = 0;
    lastBodyLength = bodyLength < sizeof(lastBody) ? bodyLength : 0;
    if (lastBodyLength) std::memcpy(lastBody, line + used, lastBodyLength);
    ReleaseSRWLockExclusive(&repeatLock);

    if (pending) {
        // The text of the repeated line is quoted, so the count still makes sense when the log is read
        // with one noisy source filtered out.
        char repeat[256];
        const int written = _snprintf_s(repeat, sizeof(repeat), _TRUNCATE,
                                        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] (repeated %llu more times: %.120s)\r\n",
                                        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                                        now.wMilliseconds, static_cast<unsigned long long>(pending), repeatedBody);
        if (written > 0) LogWrite(repeat, static_cast<std::size_t>(written));
    }
    line[length++] = '\r';
    line[length++] = '\n';
    LogWrite(line, length);
}

}  // namespace multislot
