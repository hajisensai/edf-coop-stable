// GameNetTests <game folder> <EDF6Coop.dll> <GameMachine.exe> <work folder> <scenario>
// Plays a room of several machines, each a process running the real EDF.dll and the real EDF6Coop.dll with
// fake_eos_net.cpp as EOS, and checks what each of them reports. Nothing here touches the installed game: EDF.dll
// is only read, and every machine's Mods folder is under the work folder. Without the game the test reports itself
// skipped (exit code 77).
#include <map>
#include <string>
#include <vector>

#include "net_shared.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct Spawned {
    std::string user;
    std::string role;
    PROCESS_INFORMATION process{};
    HANDLE output = nullptr;  // read end of its stdout
    std::string text;         // everything it printed
    DWORD exitCode = 0;
    std::multimap<std::string, std::string> results;  // RESULT <key> <value>
};

bool WriteFileText(const std::wstring& path, const std::string& text) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(file);
    return ok;
}

// work\<user>\Mods\Plugins\EDF6Coop.dll and its INI.
bool PrepareMachine(const std::wstring& work, const std::wstring& plugin, const std::string& ini) {
    const std::wstring mods = work + L"\\Mods", plugins = mods + L"\\Plugins";
    for (const auto& path : {work, mods, plugins}) CreateDirectoryW(path.c_str(), nullptr);
    DeleteFileW((plugins + L"\\EDF6Coop.log").c_str());
    if (ini.empty())  // a machine without EDF6Coop
        return DeleteFileW((plugins + L"\\EDF6Coop.dll").c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    return CopyFileW(plugin.c_str(), (plugins + L"\\EDF6Coop.dll").c_str(), FALSE) &&
           WriteFileText(plugins + L"\\EDF6Coop.ini", ini);
}

std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// A UDP port nothing on this machine uses now, for a scenario's direct-link host (its INI says @PORT@): other
// test runs on the same machine get ports of their own.
std::string FreeUdpPort() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const SOCKET s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in6 any{};
    any.sin6_family = AF_INET6;
    int len = sizeof(any);
    std::string port = "0";
    if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&any), &len) == 0)
        port = std::to_string(ntohs(any.sin6_port));
    if (s != INVALID_SOCKET) closesocket(s);
    return port;
}

std::string WithPort(std::string ini, const std::string& port) {
    for (std::size_t at = ini.find("@PORT@"); at != std::string::npos; at = ini.find("@PORT@", at)) ini.replace(at, 6, port);
    return ini;
}

bool Spawn(Spawned& machine, const std::wstring& exe, const std::wstring& gameFolder, const std::wstring& work,
           const std::string& section) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) return false;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    SetEnvironmentVariableA(gamenet::kSectionVariable, section.c_str());
    SetEnvironmentVariableA(gamenet::kUserVariable, machine.user.c_str());
    std::wstring command = L"\"" + exe + L"\" " + Wide(machine.role) + L" \"" + gameFolder + L"\" \"" + work + L"\"";
    // EDF6NET_DEBUGGER=<path of cdb.exe>: every machine runs under it, and a crash prints its stack.
    wchar_t debugger[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"EDF6NET_DEBUGGER", debugger, MAX_PATH))
        command = L"\"" + std::wstring(debugger) +
                  L"\" -lines -c \"g; kn 40; r; q\" " + command;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;
    startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                   work.c_str(), &startup, &machine.process);
    CloseHandle(write);
    machine.output = read;
    return ok != FALSE;
}

// Reads every machine's output until all of them ended or `timeoutMs` passed (then they are ended).
void Collect(std::vector<Spawned>& machines, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        bool running = false;
        for (auto& machine : machines) {
            DWORD available = 0;
            while (machine.output && PeekNamedPipe(machine.output, nullptr, 0, nullptr, &available, nullptr) && available) {
                char buffer[4096];
                DWORD read = 0;
                if (!ReadFile(machine.output, buffer, sizeof(buffer), &read, nullptr) || !read) break;
                machine.text.append(buffer, read);
            }
            if (WaitForSingleObject(machine.process.hProcess, 0) == WAIT_TIMEOUT) running = true;
        }
        if (!running) break;
        if (GetTickCount64() > deadline) {
            for (auto& machine : machines)
                if (WaitForSingleObject(machine.process.hProcess, 0) == WAIT_TIMEOUT) {
                    std::printf("FAIL: %s (%s) still running after %lu ms; ended\n", machine.user.c_str(),
                                machine.role.c_str(), timeoutMs);
                    ++failures;
                    TerminateProcess(machine.process.hProcess, 99);
                    WaitForSingleObject(machine.process.hProcess, 2000);
                }
        }
        Sleep(10);
    }
    for (auto& machine : machines) {
        DWORD available = 0;
        char buffer[4096];
        DWORD read = 0;
        while (PeekNamedPipe(machine.output, nullptr, 0, nullptr, &available, nullptr) && available &&
               ReadFile(machine.output, buffer, sizeof(buffer), &read, nullptr) && read)
            machine.text.append(buffer, read);
        GetExitCodeProcess(machine.process.hProcess, &machine.exitCode);
        CloseHandle(machine.output);
        CloseHandle(machine.process.hProcess);
        CloseHandle(machine.process.hThread);
        std::size_t at = 0;
        while ((at = machine.text.find("RESULT ", at)) != std::string::npos) {
            const std::size_t end = machine.text.find('\n', at);
            const std::string line = machine.text.substr(at + 7, end == std::string::npos ? std::string::npos : end - at - 7);
            const std::size_t space = line.find(' ');
            std::string value = space == std::string::npos ? "" : line.substr(space + 1);
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
            machine.results.emplace(line.substr(0, space), value);
            at = end == std::string::npos ? machine.text.size() : end;
        }
    }
}

std::string Result(const Spawned& machine, const std::string& key) {
    const auto it = machine.results.find(key);
    return it == machine.results.end() ? std::string("<none>") : it->second;
}

}  // namespace

#include "scenarios.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 6) {
        std::printf("usage: GameNetTests <game folder> <EDF6Coop.dll> <GameMachine.exe> <work folder> <scenario>\n");
        return 2;
    }
    // Absolute: every machine runs in its own folder.
    const auto full = [](const wchar_t* path) {
        wchar_t out[MAX_PATH]{};
        return GetFullPathNameW(path, MAX_PATH, out, nullptr) ? std::wstring(out) : std::wstring(path);
    };
    const std::wstring gameFolder = full(argv[1]), plugin = full(argv[2]), exe = full(argv[3]), work = full(argv[4]);
    std::string scenario;
    for (const wchar_t* c = argv[5]; *c; ++c) scenario += static_cast<char>(*c);
    if (GetFileAttributesW((gameFolder + L"\\EDF.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("SKIPPED: no EDF.dll in %ls (set EDF6_GAME_DIR to the game folder to run this test)\n",
                    gameFolder.c_str());
        return 77;
    }
    const Scenario* chosen = FindScenario(scenario);
    if (!chosen) {
        std::printf("FAIL: no scenario %s\n", scenario.c_str());
        return 2;
    }
    const std::string section = "edf6net-" + std::to_string(GetCurrentProcessId()) + "-" + scenario;
    HANDLE sectionHandle = nullptr, lock = nullptr;
    gamenet::Network* network = gamenet::MapNetwork(section.c_str(), true, &sectionHandle, &lock);
    if (!network) {
        std::printf("FAIL: the network section cannot be created (error %lu)\n", GetLastError());
        return 1;
    }
    CreateDirectoryW(work.c_str(), nullptr);
    const std::wstring folder = work + L"\\" + Wide(scenario);
    CreateDirectoryW(folder.c_str(), nullptr);

    // Every machine lives in this job: when the test ends, however it ends, so do they.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    std::vector<Spawned> machines;
    SetEnvironmentVariableA("EDF6NET_MEMBERS", std::to_string(chosen->seats.size()).c_str());
    for (const char* variable : {gamenet::kDelayVariable, gamenet::kDropVariable, gamenet::kLobbyDelayVariable,
                                 "EDF6NET_CHATTER", "EDF6NET_RUSH"})
        SetEnvironmentVariableA(variable, nullptr);
    for (const auto& [variable, value] : chosen->network) SetEnvironmentVariableA(variable.c_str(), value.c_str());
    const std::string port = FreeUdpPort();  // a direct-link host's, when its INI asks for one
    for (const auto& seat : chosen->seats) {
        Spawned machine;
        machine.user = seat.user;
        machine.role = seat.role;
        const std::wstring home = folder + L"\\" + Wide(seat.user);
        Check(PrepareMachine(home, plugin, WithPort(seat.ini, port)), seat.user + ": work folder prepared");
        machines.push_back(machine);
        for (const auto& [variable, value] : seat.env) SetEnvironmentVariableA(variable.c_str(), value.c_str());
        SetEnvironmentVariableA("EDF6NET_SEAT", std::to_string(machines.size() - 1).c_str());
        if (!Spawn(machines.back(), exe, gameFolder, home, section)) {
            std::printf("FAIL: %s cannot be started (error %lu)\n", seat.user.c_str(), GetLastError());
            CloseHandle(job);  // ends the machines started already
            return 1;
        }
        for (const auto& [variable, value] : seat.env) SetEnvironmentVariableA(variable.c_str(), nullptr);
        AssignProcessToJobObject(job, machines.back().process.hProcess);
        ResumeThread(machines.back().process.hThread);  // started suspended, so it is in the job from its first instruction
    }
    Collect(machines, chosen->timeoutMs);
    const bool debugged = GetEnvironmentVariableW(L"EDF6NET_DEBUGGER", nullptr, 0) != 0;
    for (const auto& machine : machines) {
        std::printf("==== %s (%s) exited %lu ====\n%s\n", machine.user.c_str(), machine.role.c_str(), machine.exitCode,
                    machine.text.c_str());
        // Its own word (under EDF6NET_DEBUGGER the exit code is the debugger's), and an exit that agrees with it.
        Check(Result(machine, "exit") == "0" && (debugged || machine.exitCode == 0),
              machine.user + " (" + machine.role + ") passed its own checks");
        if (Result(machine, "plugin").rfind("none", 0) == 0) continue;  // the game alone writes no EDF6Coop.log
        Check(Result(machine, "log").find("bytes") != std::string::npos &&
                  machine.text.find("==== EDF6Coop ") != std::string::npos,
              machine.user + "'s EDF6Coop.log was read");
    }
    chosen->check(machines, *network);
    CloseHandle(job);
    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
