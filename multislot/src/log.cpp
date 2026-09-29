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

// The marks that say where one run of the game ends and the next begins, as plugin.cpp, LogShutdown and
// LogUnloaded write them. Checked against the real banner by the tests. They count only as the first thing
// after a line's own timestamp: text inside a line (a member's name, an EOS message) cannot fake one.
constexpr const char* kBannerMark = "==== EDF6MultiSlot ";
constexpr const char* kShutdownMark = "SHUTDOWN ";
constexpr const char* kUnloadedMark = "UNLOADED ";
// "[2026-09-20 00:09:00.000] ": what every line the plugin writes starts with.
constexpr std::size_t kStampLength = 26;
LastRun lastRun = LastRun::Unknown;
std::atomic<bool> endWritten{false};

// A line that repeats (some EOS SDK warnings arrive every frame) is written once and then counted, so one
// noisy source cannot push everything else out of the log.
SRWLOCK repeatLock = SRWLOCK_INIT;
char lastBody[512]{};
std::size_t lastBodyLength = 0;
std::uint64_t repeats = 0;

// Lines are queued here and written to the file by a writer thread, so the game's threads never wait on the
// disk: before 1.5.13 every line opened, appended to and closed the file on the thread that logged it - the
// game's UI thread and the EOS callbacks among them - and a trim copied 1.5 MB right there. The queue is a ring
// behind a LogQueueHeader (log.h), in the mapping of <log>.queue when there is one and in the static ramHeader
// and ramRing otherwise (no heap either way, so a line can still be queued from an exception handler). The
// pointers change only under queueLock and drainLock both.
SRWLOCK queueLock = SRWLOCK_INIT;
LogQueueHeader ramHeader{};
char ramRing[kLogQueueBytes];
LogQueueHeader* header = &ramHeader;
char* ring = ramRing;
// The mapping of <log>.queue, used only under drainLock.
HANDLE queueFile = INVALID_HANDLE_VALUE;
HANDLE queueMapping = nullptr;
void* queueView = nullptr;
constexpr std::size_t kQueueFileBytes = sizeof(LogQueueHeader) + kLogQueueBytes;

// Held while queued lines go to the file and while the file is trimmed, so batches land in the order they were
// queued. The thread holding it is remembered: a crash reported from inside a write or a trim writes without
// it instead of deadlocking on the lock it already holds.
SRWLOCK drainLock = SRWLOCK_INIT;
std::atomic<DWORD> drainThread{0};

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

HANDLE OpenForAppend() {
    fileOpens.fetch_add(1);
    return CreateFileW(logPath, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

LONGLONG SizeOf(HANDLE file) {
    LARGE_INTEGER size{};
    return GetFileSizeEx(file, &size) ? size.QuadPart : -1;
}

// Appends `text` to the log and returns the file's size afterwards (0 when it could not be written).
LONGLONG AppendToFile(const char* text, std::size_t length) {
    const HANDLE file = OpenForAppend();
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    const LONGLONG size = SizeOf(file);
    CloseHandle(file);
    return size < 0 ? 0 : size;
}

// Caller holds drainLock. Appends queued bytes [from, to) of `queue` to the log and returns the file's size
// afterwards (0 when it could not be written: those lines are gone, as they always were, since a log that
// cannot be written must not stall the game). The append is announced in the header first, and `consumed`
// moves past it only after, so a kill anywhere in between leaves the next start (RecoverQueue) able to tell
// how much of it reached the file.
LONGLONG AppendQueued(LogQueueHeader* queue, const char* bytes, std::uint64_t from, std::uint64_t to) {
    const HANDLE file = OpenForAppend();
    if (file != INVALID_HANDLE_VALUE) {
        const LONGLONG before = SizeOf(file);
        if (before >= 0) {
            queue->appendingFrom = from;
            queue->appendingAt = before;
            // Stores into the mapped header must reach it in this order; only the compiler could reorder them.
            std::atomic_signal_fence(std::memory_order_seq_cst);
            queue->appending = to - from;
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        const std::size_t at = static_cast<std::size_t>(from % kLogQueueBytes);
        const std::size_t length = static_cast<std::size_t>(to - from);
        const std::size_t first = length < kLogQueueBytes - at ? length : kLogQueueBytes - at;
        DWORD written = 0;
        WriteFile(file, bytes + at, static_cast<DWORD>(first), &written, nullptr);
        if (length > first) WriteFile(file, bytes, static_cast<DWORD>(length - first), &written, nullptr);
    }
    const LONGLONG size = file == INVALID_HANDLE_VALUE ? 0 : SizeOf(file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    // Under queueLock: the threads that log read `consumed` to see how much room is left.
    AcquireSRWLockExclusive(&queueLock);
    queue->consumed = to;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    queue->appending = 0;
    ReleaseSRWLockExclusive(&queueLock);
    return size < 0 ? 0 : size;
}

// Caller holds drainLock (or is the thread that does). Writes out everything queued so far.
void DrainHeld() {
    for (;;) {
        AcquireSRWLockExclusive(&queueLock);
        LogQueueHeader* const queue = header;
        const char* const bytes = ring;
        const std::uint64_t from = queue->consumed, to = queue->produced;
        ReleaseSRWLockExclusive(&queueLock);
        if (from == to) return;
        // Only this thread takes from the front of the queue, and the threads that log only add behind `to`.
        if (AppendQueued(queue, bytes, from, to) > kLogCapBytes) TrimLocked(logPath, kLogCapBytes, kLogKeepBytes);
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

// The thread that holds drainLock logging again can only be a crash reported from inside a write or a trim:
// the queue and the trim it interrupted are still in use, so it must not drain or trim again.
bool InsideDrain() { return drainThread.load() == GetCurrentThreadId(); }

// Caller holds queueLock. Adds `text` behind what `queue` holds; the ring wraps.
void Enqueue(LogQueueHeader* queue, char* target, const char* text, std::size_t length) {
    const std::size_t at = static_cast<std::size_t>(queue->produced % kLogQueueBytes);
    const std::size_t first = length < kLogQueueBytes - at ? length : kLogQueueBytes - at;
    std::memcpy(target + at, text, first);
    std::memcpy(target, text + first, length - first);
    // The bytes first, then the count that says they are there: a kill in between loses only this line.
    std::atomic_signal_fence(std::memory_order_seq_cst);
    queue->produced += length;
}

// Caller holds queueLock. Moves what `from` still has queued into the ring of `to`.
void MoveQueued(LogQueueHeader* from, const char* fromBytes, LogQueueHeader* to, char* toBytes) {
    for (std::uint64_t at = from->consumed; at < from->produced;) {
        const std::size_t offset = static_cast<std::size_t>(at % kLogQueueBytes);
        std::size_t length = static_cast<std::size_t>(from->produced - at);
        if (length > kLogQueueBytes - offset) length = kLogQueueBytes - offset;
        Enqueue(to, toBytes, fromBytes + offset, length);
        at += length;
    }
    from->consumed = from->produced;
    from->appending = 0;
}

// Caller holds drainLock. What a run that was killed left queued in `queue` goes to the log now, ahead of this
// run's first line, where it belongs. A header this code did not write is left alone.
void RecoverQueue(LogQueueHeader* queue, const char* bytes) {
    if (std::memcmp(queue->magic, kLogQueueMagic, sizeof(kLogQueueMagic)) != 0) return;
    std::uint64_t from = queue->consumed;
    const std::uint64_t to = queue->produced;
    if (to < from || to - from > kLogQueueBytes) return;
    // An append that was under way got part of the way, or all of it, into the file.
    if (queue->appending && queue->appendingFrom == from && queue->appending <= to - from) {
        LONGLONG size = -1;
        const HANDLE file = OpenForAppend();
        if (file != INVALID_HANDLE_VALUE) {
            size = SizeOf(file);
            CloseHandle(file);
        }
        const LONGLONG arrived = size - queue->appendingAt;
        if (arrived > 0) from += static_cast<std::uint64_t>(arrived) < queue->appending ? static_cast<std::uint64_t>(arrived) : queue->appending;
    }
    queue->consumed = from;
    queue->appending = 0;
    if (from == to) return;
    AppendQueued(queue, bytes, from, to);
    char note[160];
    const int length = _snprintf_s(note, sizeof(note), _TRUNCATE,
                                   "[... the %llu bytes above were logged by the run before, which ended before they "
                                   "were written; recovered from the .queue file ...]\r\n",
                                   static_cast<unsigned long long>(to - from));
    if (length > 0) AppendToFile(note, static_cast<std::size_t>(length));
}

// Caller holds drainLock, with logPath already set. Maps <log>.queue, writes out what a killed run left in it and
// makes it this run's queue; false (and the queue stays in memory) when it cannot be had.
bool OpenQueueFile() {
    const std::size_t length = wcslen(logPath), suffix = wcslen(kLogQueueSuffix);
    if (length + suffix >= MAX_PATH) return false;
    wchar_t path[MAX_PATH]{};
    wmemcpy(path, logPath, length);
    wmemcpy(path + length, kLogQueueSuffix, suffix + 1);
    // Not shared for writing: a second game started from the same folder keeps its queue in memory.
    const HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kQueueFileBytes), nullptr);
    void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kQueueFileBytes) : nullptr;
    if (!view) {
        if (mapping) CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    auto* queue = static_cast<LogQueueHeader*>(view);
    char* bytes = static_cast<char*>(view) + sizeof(LogQueueHeader);
    RecoverQueue(queue, bytes);
    // Every page is read in now, on the loading thread, so the first line queued into it later does not wait for
    // the disk on whatever game thread logs it.
    const volatile char* pages = static_cast<const volatile char*>(view);
    for (std::size_t at = 0; at < kQueueFileBytes; at += 4096) static_cast<void>(pages[at]);
    queue->produced = queue->consumed = 0;
    queue->appending = 0;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memcpy(queue->magic, kLogQueueMagic, sizeof(kLogQueueMagic));
    AcquireSRWLockExclusive(&queueLock);
    MoveQueued(header, ring, queue, bytes);  // lines logged while the file was being opened
    header = queue;
    ring = bytes;
    ReleaseSRWLockExclusive(&queueLock);
    queueFile = file;
    queueMapping = mapping;
    queueView = view;
    return true;
}

// Caller holds drainLock. Back to the queue in memory; what the file's queue still holds moves along.
void CloseQueueFile() {
    if (!queueView) return;
    AcquireSRWLockExclusive(&queueLock);
    ramHeader = LogQueueHeader{};
    MoveQueued(header, ring, &ramHeader, ramRing);
    header = &ramHeader;
    ring = ramRing;
    ReleaseSRWLockExclusive(&queueLock);
    UnmapViewOfFile(queueView);
    CloseHandle(queueMapping);
    CloseHandle(queueFile);
    queueView = nullptr;
    queueMapping = nullptr;
    queueFile = INVALID_HANDLE_VALUE;
}

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
                const char* unloaded = nullptr;
                const char* const end = text + read;
                for (const char* line = text; line < end;) {
                    const char* next = static_cast<const char*>(std::memchr(line, '\n', static_cast<std::size_t>(end - line)));
                    next = next ? next + 1 : end;
                    if (Marked(line, next, kBannerMark)) banner = line;
                    else if (Marked(line, next, kShutdownMark)) shutdown = line;
                    else if (Marked(line, next, kUnloadedMark)) unloaded = line;
                    line = next;
                }
                // A missing SHUTDOWN only means the run was cut if this build can write one at all. On
                // 2026-09-29 it still could not - DLL_PROCESS_DETACH does not run for this game, and
                // wrapping TerminateProcess did not catch it either - so a file that has never held the
                // mark says nothing, and claiming otherwise called every ordinary quit a crash. A run whose
                // plugin was unloaded (UNLOADED after its banner) could not see its own end: nothing either.
                const bool unseen = unloaded && (!banner || unloaded > banner) && (!shutdown || unloaded > shutdown);
                if (shutdown && !unseen) verdict = (!banner || shutdown > banner) ? LastRun::Ended : LastRun::Cut;
            }
            HeapFree(GetProcessHeap(), 0, text);
        }
    }
    CloseHandle(file);
    return verdict;
}

// LogShutdown and LogUnloaded: the last line this module writes, once.
void LogEnd(const char* mark, const char* why) {
    if (endWritten.exchange(true)) return;
    // This runs from DLL_PROCESS_DETACH, where at process exit every other thread has already been terminated
    // - one of them possibly while holding the log's locks - or from the TerminateProcess hook, where the writer
    // thread still runs. No repeat collapsing, no trim, no heap: a hang at exit would be blamed on the mod.
    if (!logPath[0]) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[256]{};
    const int length = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s%s\r\n",
                                   now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                                   now.wMilliseconds, mark, why);
    if (length <= 0) return;
    // Queued lines first, then this one, both under drainLock when it can be had, so a trim by the writer
    // thread can neither cut the line nor be under way when it is written. A thread terminated while holding
    // the lock never lets go, so after about 100 ms the line is written without it (what is still queued is
    // then written by the next start, from the .queue file).
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (TryAcquireSRWLockExclusive(&drainLock)) {
            drainThread.store(GetCurrentThreadId());
            // Tried, not waited for, for the same reason. Once it has been had, the second take in
            // AppendQueued cannot find it held by a dead thread: those are all gone before this runs.
            if (TryAcquireSRWLockExclusive(&queueLock)) {
                LogQueueHeader* const queue = header;
                const char* const bytes = ring;
                const std::uint64_t from = queue->consumed, to = queue->produced;
                ReleaseSRWLockExclusive(&queueLock);
                if (from != to) AppendQueued(queue, bytes, from, to);
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

std::size_t LogQueued() {
    AcquireSRWLockExclusive(&queueLock);
    const auto queued = static_cast<std::size_t>(header->produced - header->consumed);
    ReleaseSRWLockExclusive(&queueLock);
    return queued;
}

// Nothing here ever waits for this thread (LogFlush drains on the calling thread), and it runs until the
// process ends: plugin.cpp starts it only once the plugin stays. When it cannot be started, writerRunning
// stays false and every line is written where it is logged, as before 1.5.13.
void LogStartWriter() {
    if (writerStarted.load() || writerStarted.exchange(true)) return;
    const HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake) return;
    wakeWriter.store(wake);
    if (HANDLE thread = CreateThread(nullptr, 64 * 1024, &WriterMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
        CloseHandle(thread);
        writerRunning.store(true);
    }
}

void LogClose() {
    WithDrainLock([] {
        DrainHeld();
        CloseQueueFile();
    });
}

void LogOpen(const wchar_t* path) {
    // Lengths are checked by hand: *_s string functions end the process when something does not fit.
    const std::size_t length = wcslen(path);
    if (length >= MAX_PATH) return;
    WithDrainLock([&] {
        // Lines queued for the previous file go there first.
        DrainHeld();
        CloseQueueFile();
        wmemcpy(logPath, path, length + 1);
        // What the run before this one left queued is written before anything of this one.
        OpenQueueFile();
        // One file across sessions, so a crash report survives restarts; only its oldest lines go.
        TrimLocked(logPath, kLogCapBytes, kLogKeepBytes);
    });
    lastRun = ReadLastRun(logPath);
}

LastRun PreviousRun() { return lastRun; }

void LogShutdown(const char* why) { LogEnd(kShutdownMark, why); }

void LogUnloaded(const char* why) { LogEnd(kUnloadedMark, why); }

void LogWrite(const char* text, std::size_t length) {
    if (!logPath[0] || !length) return;
    if (InsideDrain()) {
        AppendToFile(text, length);  // not queued, no trim: the interrupted drain still owns both
        return;
    }
    if (length > kLogQueueBytes) {
        // Larger than the whole queue: behind everything already queued, straight to the file.
        WithDrainLock([&] {
            DrainHeld();
            if (AppendToFile(text, length) > kLogCapBytes) TrimLocked(logPath, kLogCapBytes, kLogKeepBytes);
        });
        return;
    }
    for (;;) {
        AcquireSRWLockExclusive(&queueLock);
        const std::uint64_t queued = header->produced - header->consumed;
        if (queued + length <= kLogQueueBytes) {
            Enqueue(header, ring, text, length);
            const bool wake = queued + length >= kLogQueueBytes / 2;
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
