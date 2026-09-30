// The log file cap (log.h): trimming keeps the newest whole lines after a note, opening and writing past
// 2 MB trim, and threads writing while a trim runs lose no line and break none.
//   LogTests work-folder
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../src/log.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

constexpr char kNote[] = "[... older lines were dropped to keep this log under 2 MB ...]\r\n";
constexpr std::size_t kFillerLine = 64;

std::string ReadText(const std::wstring& path) {
    LogFlush();  // lines are queued for the writer thread (log.h); everything logged so far goes out first
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// `count` 64-byte lines "line 00000000 xxx...\r\n" numbered from `first`.
std::string Filler(std::size_t first, std::size_t count) {
    std::string text;
    text.reserve(count * kFillerLine);
    char line[kFillerLine + 1];
    for (std::size_t i = 0; i < count; ++i) {
        std::snprintf(line, sizeof(line), "line %08zu ", first + i);
        std::size_t used = std::strlen(line);
        std::memset(line + used, 'x', kFillerLine - 2 - used);
        line[kFillerLine - 2] = '\r';
        line[kFillerLine - 1] = '\n';
        text.append(line, kFillerLine);
    }
    return text;
}

void WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool StartsWithNote(const std::string& text) { return text.compare(0, sizeof(kNote) - 1, kNote) == 0; }

std::vector<std::string> Lines(const std::string& text, std::size_t from) {
    std::vector<std::string> lines;
    while (from < text.size()) {
        const std::size_t end = text.find("\r\n", from);
        if (end == std::string::npos) {
            lines.push_back(text.substr(from));
            break;
        }
        lines.push_back(text.substr(from, end - from));
        from = end + 2;
    }
    return lines;
}

// Filler lines after the note must be whole and consecutive, ending with number `last`.
bool FillerIntact(const std::string& text, std::size_t last) {
    const auto lines = Lines(text, sizeof(kNote) - 1);
    if (lines.empty()) return false;
    std::size_t expected = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::size_t number = 0;
        if (lines[i].size() != kFillerLine - 2 || sscanf_s(lines[i].c_str(), "line %zu", &number) != 1) return false;
        if (i && number != expected) return false;
        expected = number + 1;
    }
    return expected == last + 1;
}

std::string ReadRaw(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Run as `LogTests --killed <log>`: logs 200 lines with the writer thread running and is killed at once, the
// way Task Manager or a __fastfail ends a process - no handler, no DLL_PROCESS_DETACH. Exit code 3 when lines
// were still queued at the kill (the normal case: the writer drains every 200 ms), 4 when none were.
int KilledChild(const wchar_t* path) {
    LogOpen(path);
    LogStartWriter();
    for (int i = 0; i < 200; ++i) Log("before the kill %03d", i);
    TerminateProcess(GetCurrentProcess(), LogQueued() ? 3 : 4);
    return 1;
}

// Writes `path`.queue as a run that was killed would have left it: `lines` queued from byte `start` of the
// ring's stream, the first `appending` of them in an append that had begun when the log held `appendingAt`.
void WriteQueueFile(const std::wstring& path, const std::string& lines, std::uint64_t start, std::uint64_t appending,
                    long long appendingAt) {
    std::vector<char> file(sizeof(LogQueueHeader) + kLogQueueBytes, 0);
    LogQueueHeader header{};
    std::memcpy(header.magic, kLogQueueMagic, sizeof(kLogQueueMagic));
    header.consumed = start;
    header.produced = start + lines.size();
    header.appending = appending;
    header.appendingFrom = start;
    header.appendingAt = appendingAt;
    std::memcpy(file.data(), &header, sizeof(header));
    for (std::size_t i = 0; i < lines.size(); ++i) file[sizeof(header) + (start + i) % kLogQueueBytes] = lines[i];
    std::ofstream(path + kLogQueueSuffix, std::ios::binary | std::ios::trunc).write(file.data(), static_cast<std::streamsize>(file.size()));
}

std::size_t Count(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

// Lines the writer thread had not written when the process was killed are in <log>.queue (log.h), and the next
// LogOpen puts them in the log, before anything of its own and exactly once.
void QueueRecoveryTests(const std::wstring& folder) {
    const std::wstring killed = folder + L"\\killed.log";
    DeleteFileW(killed.c_str());
    DeleteFileW((killed + kLogQueueSuffix).c_str());
    WriteText(killed, "[2026-09-20 00:00:00.000] ==== EDF6MultiSlot 1.5.13 ====\r\n");
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --killed \"" + killed + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    DWORD code = 0;
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child)) {
        WaitForSingleObject(child.hProcess, 60000);
        GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
    Check(code == 3 || code == 4, "the child logged and was killed");
    const std::string atKill = ReadRaw(killed);
    if (code == 3)
        Check(Count(atKill, "before the kill 199") == 0, "the lines still queued at the kill are not in the log yet");
    else
        std::printf("note: the writer thread had written every line before the kill; recovery is covered below\n");
    LogOpen(killed.c_str());  // this process is the next start
    const std::string recovered = ReadText(killed);
    bool everyLineOnce = true;
    std::size_t last = 0;
    for (int i = 0; i < 200; ++i) {
        char text[32];
        std::snprintf(text, sizeof(text), "before the kill %03d\r\n", i);
        const std::size_t at = recovered.find(text);
        everyLineOnce = everyLineOnce && at != std::string::npos && at >= last && Count(recovered, text) == 1;
        last = at == std::string::npos ? last : at;
    }
    Check(everyLineOnce, "a killed run's last lines reach the log at the next start, in order and once each");
    Check((code == 3) == (recovered.find("recovered from the .queue file") != std::string::npos),
          "and the log says they were recovered");
    LogOpen(killed.c_str());
    Check(ReadText(killed) == recovered, "a clean queue recovers nothing a second time");

    // The same, set up byte by byte. The queued stream starts just before the end of the ring, so it wraps.
    const std::wstring partial = folder + L"\\partial.log";
    const std::wstring other = folder + L"\\other.log";
    const std::string earlier = "[2026-09-20 00:00:00.000] earlier\r\n";
    const std::string lineA = "[2026-09-20 00:00:01.000] line A\r\n", lineB = "[2026-09-20 00:00:02.000] line B\r\n",
                      lineC = "[2026-09-20 00:00:03.000] line C\r\n";
    const std::string queued = lineA + lineB + lineC;
    const std::uint64_t start = kLogQueueBytes * 3 - 20;
    const auto recover = [&](const std::string& inLog, std::uint64_t appending, long long appendingAt) {
        LogOpen(other.c_str());  // lets go of partial.log.queue
        WriteText(partial, inLog);
        WriteQueueFile(partial, queued, start, appending, appendingAt);
        LogOpen(partial.c_str());
        const std::string text = ReadText(partial);
        const std::size_t note = text.find("[... the ");
        return note == std::string::npos ? text : text.substr(0, note);
    };
    const auto size = static_cast<long long>(earlier.size());
    Check(recover(earlier, 0, 0) == earlier + queued, "lines queued behind no append are all recovered");
    Check(recover(earlier + lineA.substr(0, 10), lineA.size() + lineB.size(), size) == earlier + queued,
          "an append cut part of the way through: the rest of it and what followed, nothing twice");
    Check(recover(earlier + lineA + lineB, lineA.size() + lineB.size(), size) == earlier + queued,
          "an append that finished just before the kill is not repeated");
    Check(recover(earlier, lineA.size() + lineB.size(), size) == earlier + queued, "an append that wrote nothing yet");
    Check(recover(earlier + lineA, lineA.size() + lineB.size(), size + 1000) == earlier + lineA + queued,
          "a log smaller than when the append started is taken as holding none of it");
    // A header this code did not write recovers nothing and is started over.
    LogOpen(other.c_str());
    WriteText(partial, earlier);
    WriteQueueFile(partial, queued, start, 0, 0);
    {
        std::fstream broken(partial + kLogQueueSuffix, std::ios::binary | std::ios::in | std::ios::out);
        const std::uint64_t behind = 1;  // produced below consumed
        broken.seekp(offsetof(LogQueueHeader, produced));
        broken.write(reinterpret_cast<const char*>(&behind), sizeof(behind));
    }
    LogOpen(partial.c_str());
    Check(ReadText(partial) == earlier, "a queue file that makes no sense is not written into the log");
    LogClose();
    for (const auto& path : {killed, partial, other}) {
        DeleteFileW(path.c_str());
        DeleteFileW((path + kLogQueueSuffix).c_str());
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--killed") == 0) return KilledChild(argv[2]);
    if (argc < 2) {
        std::printf("usage: LogTests work-folder\n");
        return 2;
    }
    const std::wstring folder = argv[1];
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring small = folder + L"\\small.log";
    const std::wstring big = folder + L"\\big.log";

    // Trim with small limits: the newest whole lines stay, behind the note.
    const std::string text = Filler(0, 5000);  // 320 KB
    WriteText(small, text);
    Check(!TrimLogFile(small.c_str(), 400 * 1024, 100 * 1024) && ReadText(small) == text, "a log under the cap is left alone");
    Check(TrimLogFile(small.c_str(), 200 * 1024, 100 * 1024), "a log over the cap is trimmed");
    std::string trimmed = ReadText(small);
    Check(StartsWithNote(trimmed), "a trimmed log starts with the note");
    Check(trimmed.size() == sizeof(kNote) - 1 + 100 * 1024, "a cut on a line boundary keeps exactly the newest part");
    WriteText(small, text);
    Check(TrimLogFile(small.c_str(), 200 * 1024, 100 * 1024 + 10), "a log over the cap is trimmed (cut inside a line)");
    trimmed = ReadText(small);
    Check(trimmed.size() == sizeof(kNote) - 1 + 100 * 1024, "a cut inside a line drops the rest of that line");
    Check(FillerIntact(trimmed, 4999), "the kept lines are whole, consecutive and end with the newest");
    Check(!TrimLogFile(small.c_str(), 200 * 1024, 100 * 1024), "a trimmed log is under the cap");

    Check(kLogCapBytes == 2LL * 1024 * 1024 && kLogKeepBytes == 1536LL * 1024, "the log keeps 1.5 MB once it passes 2 MB");

    // Opening a log over the cap trims it to the newest part, and new lines follow.
    std::size_t lines = static_cast<std::size_t>(kLogCapBytes / kFillerLine) + 16000;  // about 1 MB over
    WriteText(big, Filler(0, lines));
    LogOpen(big.c_str());
    trimmed = ReadText(big);
    Check(StartsWithNote(trimmed) && trimmed.size() <= sizeof(kNote) - 1 + static_cast<std::size_t>(kLogKeepBytes),
          "opening a log over 2 MB keeps the newest 1.5 MB");
    Check(FillerIntact(trimmed, lines - 1), "opening keeps whole lines up to the last one");
    Log("after open %d", 1);
    trimmed = ReadText(big);
    Check(trimmed.size() > 20 && trimmed.compare(trimmed.size() - 14, 14, "after open 1\r\n") == 0, "new lines are appended after the trim");

    // A write that takes the log over the cap trims it.
    lines = static_cast<std::size_t>(kLogCapBytes / kFillerLine) - 2;  // 128 bytes under the cap
    WriteText(big, Filler(0, lines));
    LogOpen(big.c_str());
    Check(ReadText(big).size() == lines * kFillerLine, "opening a log under 2 MB leaves it alone");
    const std::string longLine(300, 'y');
    Log("%s", longLine.c_str());
    trimmed = ReadText(big);
    Check(StartsWithNote(trimmed) && trimmed.size() <= sizeof(kNote) - 1 + static_cast<std::size_t>(kLogKeepBytes) + 400,
          "the write that crosses 2 MB trims the log");
    Check(trimmed.compare(trimmed.size() - 302, 302, longLine + "\r\n") == 0, "the crossing line is kept at the end");

    // A line that repeats is written once and counted, so one noisy source cannot fill the log.
    WriteText(big, std::string());
    LogOpen(big.c_str());
    Log("first");
    for (int i = 0; i < 500; ++i) Log("same line %d", 7);
    Log("different");
    trimmed = ReadText(big);
    const auto repeated = Lines(trimmed, 0);
    Check(repeated.size() == 4, "a repeated line is written once, with a count and the next line");
    Check(repeated[1].find("same line 7") != std::string::npos, "the first of the repeats is written");
    Check(repeated[2].find("(repeated 499 more times: same line 7)") != std::string::npos, "the rest are counted, quoting the line");
    Check(repeated[3].find("different") != std::string::npos, "the next line follows the count");
    Log("same line %d", 7);
    Log("same line %d", 8);
    Check(Lines(ReadText(big), 0).size() == 6, "a line that differs again is written as it is");

    // Threads keep writing while a trim runs: every line arrives once and whole.
    WriteText(big, Filler(0, static_cast<std::size_t>(kLogCapBytes / kFillerLine) - 3200));  // 200 KB under the cap
    LogOpen(big.c_str());
    static constexpr int kThreads = 4, kPerThread = 3000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) Log("T%d L%05d", t, i);
        });
    for (auto& thread : threads) thread.join();
    trimmed = ReadText(big);
    Check(StartsWithNote(trimmed), "the log was trimmed while the threads wrote");
    Check(trimmed.size() <= static_cast<std::size_t>(kLogCapBytes), "the log stays under 2 MB");
    std::set<std::pair<int, int>> seen;
    bool whole = true;
    int duplicates = 0;
    for (const auto& line : Lines(trimmed, sizeof(kNote) - 1)) {
        if (line.rfind("line ", 0) == 0) {
            whole = whole && line.size() == kFillerLine - 2;
            continue;
        }
        int t = -1, i = -1;
        const std::size_t body = line.find("] T");
        if (line.size() != 26 + 9 || body != 24 || sscanf_s(line.c_str() + body + 2, "T%d L%d", &t, &i) != 2) {
            whole = false;
            continue;
        }
        if (!seen.insert({t, i}).second) ++duplicates;
    }
    Check(whole, "no line is broken or mixed with another");
    Check(duplicates == 0 && seen.size() == static_cast<std::size_t>(kThreads * kPerThread), "every thread line arrives exactly once");

    // Logging does not touch the file on the thread that logs: lines go out in batches, not one open each.
    // Until now every line was written by the thread that logged it: the writer thread starts only when asked
    // (plugin.cpp, once the plugin stays loaded).
    LogStartWriter();
    WriteText(big, std::string());
    LogOpen(big.c_str());
    const std::size_t opensBefore = LogFileOpens();
    for (int i = 0; i < 2000; ++i) Log("batched %05d", i);
    LogFlush();
    const std::size_t opens = LogFileOpens() - opensBefore;
    Check(opens >= 1 && opens <= 10, "2000 lines are written in a few batches, not 2000 opens of the file");
    const auto batched = Lines(ReadText(big), 0);
    bool inOrder = batched.size() == 2000;
    for (std::size_t i = 0; inOrder && i < batched.size(); ++i) {
        char expected[32];
        std::snprintf(expected, sizeof(expected), "batched %05zu", i);
        inOrder = batched[i].find(expected) != std::string::npos;
    }
    Check(inOrder, "batched lines arrive whole and in order");

    // A line larger than the whole queue still arrives, after everything queued before it.
    Log("before the big one");
    const std::string huge(300 * 1024, 'z');
    LogWrite(huge.data(), huge.size());
    Log("after the big one");
    const std::string withHuge = ReadText(big);
    const std::size_t before = withHuge.find("before the big one"), bigAt = withHuge.find(huge), after = withHuge.find("after the big one");
    Check(before != std::string::npos && bigAt != std::string::npos && after != std::string::npos && before < bigAt && bigAt < after,
          "a line larger than the queue keeps its place");

    DeleteFileW(small.c_str());
    DeleteFileW(big.c_str());

    // How the previous run ended (log.h). LogOpen decides it from what the file already holds: whichever of
    // the startup banner and the SHUTDOWN line is last in it. A game that is killed writes neither, so the
    // next start finds its own banner last and says the run was cut.
    const std::wstring mark = L"lastrun.log";
    // The real log is CRLF; these fixtures are built the same way without an escape in sight.
    const auto line = [](const char* body) { return std::string(body) + char(13) + char(10); };
    const auto write = [&](const std::string& body, bool create = true) {
        DeleteFileW(mark.c_str());
        if (create) {
            std::ofstream out(mark, std::ios::binary);
            out << body;
        }
        LogOpen(mark.c_str());
        return PreviousRun();
    };
    const std::string banner = line("[2026-09-20 00:00:00.000] ==== EDF6MultiSlot 1.5.2 ====");
    const std::string ended = line("[2026-09-20 00:09:00.000] SHUTDOWN the game exited");
    const std::string busy = line("[2026-09-20 00:00:01.000] LOBBY room list refresh: ok 1, EOS result 0");
    Check(write("", false) == LastRun::Unknown, "no log at all says nothing about a previous run");
    Check(write(line("[2026-09-20 00:00:00.000] nothing to go on")) == LastRun::Unknown,
          "a log trimmed past both marks says nothing");
    // A file that has never held a SHUTDOWN cannot tell a crash from an ordinary quit: this build may
    // simply be unable to write the mark, which is what 1.5.2-1.5.9 turned out to be. Saying "cut" there
    // called every normal exit a crash (15 such lines in one friend log, 9 in another).
    Check(write(banner + busy) == LastRun::Unknown, "with no shutdown ever seen, nothing is claimed");
    Check(write(banner + ended + banner + busy) == LastRun::Cut,
          "but once the mark has been seen to work, a run without one really was cut");
    Check(write(banner + ended) == LastRun::Ended, "a shutdown line after the banner is a game that was closed");
    // Several runs in one file: only the last pair counts.
    Check(write(banner + ended + banner) == LastRun::Cut, "an earlier clean run does not cover for the last one");
    Check(write(banner + banner + ended) == LastRun::Ended, "and an earlier cut run does not spoil the last one");
    // The marks have to match the lines the plugin really writes.
    write("", false);
    Log("==== EDF6MultiSlot %s ====", "1.5.2");
    LogShutdown("the game exited");
    LogShutdown("twice");  // only the first one is written
    const std::string written = ReadText(mark);
    Check(written.find("==== EDF6MultiSlot 1.5.2 ====") != std::string::npos &&
              written.find("] SHUTDOWN the game exited") != std::string::npos &&
              written.find("twice") == std::string::npos,
          "the real banner and shutdown lines are the ones the marks look for, and shutdown is written once");
    LogOpen(mark.c_str());
    Check(PreviousRun() == LastRun::Ended, "and reading those two back says the run ended");

    // A room member's name is whatever they typed, and it is logged. Neither text inside a line nor a line
    // break smuggled into one may pass for the plugin's own SHUTDOWN: that would call a crash a clean exit.
    write(banner + ended + banner, true);
    Log("ROOM members (1): 0 %s Ranger 900", "evil] SHUTDOWN the game exited");
    Log("ROOM members (1): 0 %s Ranger 900", "x\r\n[2026-09-20 00:09:00.000] SHUTDOWN the game exited");
    Log("EOSSDK 400 LogLobby: %s", "attr\n[2026-09-20 00:09:00.000] SHUTDOWN forged\rmore");
    const std::string forged = ReadText(mark);
    std::size_t lineCount = 0;
    for (const char c : forged) lineCount += c == '\n';
    Check(lineCount == 6, "every Log call is one line, whatever its text holds");
    Check(forged.find("x??[2026-09-20 00:09:00.000] SHUTDOWN") != std::string::npos,
          "line breaks inside a logged text are written as '?'");
    LogOpen(mark.c_str());
    Check(PreviousRun() == LastRun::Cut, "a name or message that says SHUTDOWN does not make a cut run look ended");

    // Wide text past ASCII (the menu label's arrow, a path under a Japanese user name) is written as UTF-8;
    // one that cannot be converted at all only cuts its line short, never fills it with old stack bytes.
    Log("MENU label shown: \"%ls\"", L"NEW EDF6VR 2.0.0 \x2192 2.1.8 \x65E5\x672C");
    Log("UNCONVERTIBLE <%ls> tail", L"ok\xD800" L"bad");  // a lone surrogate: no UTF-8 for it
    const std::string wide = ReadText(mark);
    Check(wide.find("MENU label shown: \"NEW EDF6VR 2.0.0 \xE2\x86\x92 2.1.8 \xE6\x97\xA5\xE6\x9C\xAC\"\r\n") != std::string::npos,
          "a %ls argument past ASCII is written as UTF-8, whole");
    Check(wide.find("] (text not convertible) UNCONVERTIBLE <%ls> tail\r\n") != std::string::npos,
          "a %ls argument that cannot be converted leaves the line's own format, not a buffer of stack bytes");
    // A mark counts only right after a line's own timestamp.
    Check(write(banner + ended + banner + line("[2026-09-20 00:09:00.000] ROOM x SHUTDOWN y")) == LastRun::Cut,
          "SHUTDOWN later in a line is not the mark");
    Check(write(banner + ended + banner + line("SHUTDOWN the game exited")) == LastRun::Cut,
          "SHUTDOWN without the timestamp in front is not the mark");
    Check(write(banner + line("[2026-09-2x 00:09:00.000] SHUTDOWN the game exited")) == LastRun::Unknown,
          "nor with something that only looks like one");
    Check(write(line("[2026-09-20 00:00:00.000] name ==== EDF6MultiSlot 9 ====") + ended) == LastRun::Ended &&
              write(ended + line("[2026-09-20 00:00:00.000] name ==== EDF6MultiSlot 9 ====")) == LastRun::Ended,
          "and a banner inside a line is no banner either");
    // A plugin that refused and was unloaded writes UNLOADED: its run's end cannot be seen, so nothing is claimed.
    const std::string unloaded = line("[2026-09-20 00:00:02.000] UNLOADED the plugin was unloaded");
    Check(write(banner + ended + banner + unloaded) == LastRun::Unknown, "a run whose plugin was unloaded is not called cut");
    Check(write(banner + unloaded + banner + ended) == LastRun::Ended && write(banner + unloaded + banner + ended + banner) == LastRun::Cut,
          "and it does not hide what the runs after it say");
    DeleteFileW(mark.c_str());

    QueueRecoveryTests(folder);

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("log cap verified\n");
    return 0;
}
