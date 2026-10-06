// Enemy target authority (src/networld.h) against a fake game: the decision table, the handler on fake objects
// laid out the way GameObjectBase is, and the hook itself on a copy of the game's instructions at the site, with
// the thunk running the displaced `cmp`. With the path of EDF.dll as argument, only the site's bytes against the
// game's code (exit 77 when the file is missing).
//   NetWorldTests [EDF.dll]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "../src/code.h"
#include "../src/midhook.h"
#include "../src/mission.h"
#include "../src/netfeature.h"
#include "../src/networld.h"
#include "../src/patches.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

std::uint64_t Address(const void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); }
void Put16(std::uint8_t* at, std::uint16_t value) { std::memcpy(at, &value, sizeof(value)); }
void Put32(std::uint8_t* at, std::uint32_t value) { std::memcpy(at, &value, sizeof(value)); }
void Put64(std::uint8_t* at, std::uint64_t value) { std::memcpy(at, &value, sizeof(value)); }
void PutFloat(std::uint8_t* at, float value) { std::memcpy(at, &value, sizeof(value)); }

// The site's bytes in the EDF.dll file at `path`; empty when the file or the RVA is not there.
std::vector<std::uint8_t> FileBytes(const char* path, std::uint32_t rva, std::size_t size) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 0x400) return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kImageTimeDateStamp) return {};
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (rva < section->VirtualAddress || rva + size > section->VirtualAddress + section->SizeOfRawData) continue;
        const std::size_t offset = section->PointerToRawData + (rva - section->VirtualAddress);
        if (offset + size > file.size()) return {};
        return {file.begin() + static_cast<std::ptrdiff_t>(offset), file.begin() + static_cast<std::ptrdiff_t>(offset + size)};
    }
    return {};
}

int CheckAgainstGame(const char* path) {
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        std::printf("no EDF.dll at %s: skipped\n", path);
        return 77;
    }
    for (const auto& site : WorldHooks()) {
        const auto bytes = FileBytes(path, site.rva, site.original.size());
        Check(bytes == site.original, site.name);
    }
    // The jb right after the hooked cmp goes where networld.h says (0F 82 rel32 at kRetargetSite + 6), and the
    // owner's message 1 follows the choice: `lea rcx, [rdi+0x120]; call 7748F0` at 54C15D.
    const auto jb = FileBytes(path, kRetargetSite + 6, 6);
    Check(jb.size() == 6 && jb[0] == 0x0F && jb[1] == 0x82, "the site is followed by jb rel32");
    if (jb.size() == 6) {
        std::int32_t rel = 0;
        std::memcpy(&rel, jb.data() + 2, sizeof(rel));
        Check(kRetargetSite + 12 + static_cast<std::uint32_t>(rel) == kRetargetSkip, "the jb skips to 54C22C");
    }
    // The counter, period and target fields the decision reads, at the instructions that use them.
    const auto counter = FileBytes(path, 0x54BF1D, 6);  // mov ecx, [rdi+0x510]
    Check(counter == std::vector<std::uint8_t>{0x8B, 0x8F, 0x10, 0x05, 0x00, 0x00}, "mov ecx, [rdi+0x510] before the site");
    const auto store = FileBytes(path, 0x54C130, 7);  // mov [rdi+0x518], rdx
    Check(store == std::vector<std::uint8_t>{0x48, 0x89, 0x97, 0x18, 0x05, 0x00, 0x00}, "the choice is stored at +0x518");
    const auto sends = FileBytes(path, 0x54C17A, 7);  // cmp byte [rdi+0x591], 0
    Check(sends == std::vector<std::uint8_t>{0x80, 0xBF, 0x91, 0x05, 0x00, 0x00, 0x00}, "the owner sends when +0x591 is set");
    const auto quiet = FileBytes(path, 0x54C183, 7);  // cmp byte [rdi+0x2E8], 0
    Check(quiet == std::vector<std::uint8_t>{0x80, 0xBF, 0xE8, 0x02, 0x00, 0x00, 0x00}, "and +0x2E8 is clear");
    const auto owned = FileBytes(path, 0x54C171, 7);  // test byte [rdi+0x128], 1
    Check(owned == std::vector<std::uint8_t>{0xF6, 0x87, 0x28, 0x01, 0x00, 0x00, 0x01}, "only the owner sends");
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("world hook sites match EDF.dll\n");
    return 0;
}

// A GameObjectBase-sized block with the fields the handler reads, and a target with a shared_ptr control block.
struct FakeObject {
    std::uint8_t bytes[0x600]{};
};
struct FakeControl {
    std::uint8_t bytes[0x10]{};
};

struct World {
    FakeObject enemy, target;
    FakeControl control;

    World() {
        Put16(enemy.bytes + kObjectNetFlags, 1);  // another machine's
        enemy.bytes[kObjectSendsTarget] = 1;
        Put32(enemy.bytes + kObjectRetargetPeriod, 60);
        SetTarget(true);
        Put32(target.bytes + kObjectState, 0);
        PutFloat(target.bytes + kObjectHealth, 100.0f);
        Put32(control.bytes + 8, 1);  // one use
    }
    void SetTarget(bool present) {
        Put64(enemy.bytes + kObjectTarget, present ? Address(target.bytes) : 0);
        Put64(enemy.bytes + kObjectTarget + 8, present ? Address(control.bytes) : 0);
    }
};

// Runs the hook handler as the thunk would: rdi = object, rcx = the frame counter before this frame's increment.
std::uint64_t RunHandler(std::uint64_t object, std::uint64_t counter) {
    CpuContext context{};
    context.rdi = object;
    context.rcx = counter;
    WorldHookHandler(kRetargetSite)(&context);
    return context.rcx;
}

// The game's instructions at the site in a function of its own: f(counter, object) returns 1 when the jb skips the
// choice, 0 when the game would choose.
//   push rdi; mov rdi, rdx; cmp ecx, [rdi+0x514]; jb +4; xor eax, eax; pop rdi; ret; mov eax, 1; pop rdi; ret
constexpr unsigned char kSiteFunction[] = {0x57, 0x48, 0x89, 0xD7, 0x3B, 0x8F, 0x14, 0x05, 0x00, 0x00, 0x72, 0x04,
                                           0x31, 0xC0, 0x5F, 0xC3, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x5F, 0xC3};
constexpr std::size_t kSiteOffset = 4;

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) return CheckAgainstGame(argv[1]);

    // The decision on its own.
    RetargetView view;
    view.online = true;
    view.netFlags = 1;
    view.ownerSendsTarget = true;
    view.period = 60;
    view.hasTarget = true;
    view.targetState = 0;
    view.targetHealth = 10.0f;
    Check(KeepOwnersTarget(view), "a remote copy with a live target from a sending owner keeps it");
    {
        RetargetView v = view;
        v.targetState = 2;
        Check(KeepOwnersTarget(v), "state 2 is selectable, as the search sees it");
        v.targetState = 1;
        Check(!KeepOwnersTarget(v), "a target the search would skip is chosen again");
        v = view;
        v.targetHealth = 0.0f;
        Check(!KeepOwnersTarget(v), "a dead target is chosen again");
        v = view;
        v.hasTarget = false;
        Check(!KeepOwnersTarget(v), "a copy without a target chooses one");
        v = view;
        v.netFlags = 2;
        Check(!KeepOwnersTarget(v), "the owner chooses for itself");
        v.netFlags = 0;
        Check(!KeepOwnersTarget(v), "an object without a network identity chooses for itself");
        v = view;
        v.ownerSendsTarget = false;
        Check(!KeepOwnersTarget(v), "a copy whose owner sends no target chooses for itself");
        v = view;
        v.online = false;
        Check(!KeepOwnersTarget(v), "offline nothing changes");
        v = view;
        v.period = 0;
        Check(!KeepOwnersTarget(v), "a period of 0 chooses every frame, as the game does");
    }

    // Feature switch: the default is on; [Netcode] EnemyTargets=0 turns it off.
    Check(!NetFeatureActive(NetFeature::EnemyTargets), "off before the INI is read");
    char folder[MAX_PATH]{};
    GetTempPathA(MAX_PATH, folder);
    char iniPath[MAX_PATH]{};
    std::snprintf(iniPath, sizeof(iniPath), "%snetworld_test_%lu.ini", folder, GetCurrentProcessId());
    wchar_t wideIni[MAX_PATH]{};
    MultiByteToWideChar(CP_ACP, 0, iniPath, -1, wideIni, MAX_PATH);
    DeleteFileW(wideIni);
    InitNetFeatures(wideIni);
    Check(NetFeatureActive(NetFeature::EnemyTargets), "on by default");
    WritePrivateProfileStringW(L"Netcode", L"EnemyTargets", L"0", wideIni);
    InitNetFeatures(wideIni);
    Check(!NetFeatureActive(NetFeature::EnemyTargets), "EnemyTargets=0 turns it off");
    WritePrivateProfileStringW(L"Netcode", L"EnemyTargets", L"1", wideIni);
    InitNetFeatures(wideIni);
    Check(NetFeatureActive(NetFeature::EnemyTargets), "EnemyTargets=1 turns it on");

    // Fake game for OnlineSession(): GameStatus pointer at 20B2890 and its online mode chain (mission.cpp).
    const std::size_t imageSize = kGameStatusPointer + 0x1000;
    auto* image = static_cast<unsigned char*>(VirtualAlloc(nullptr, imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(image != nullptr, "fake image");
    if (!image) return 1;
    std::vector<std::uint8_t> status(0x15100, 0), info(0x100, 0), entry(0x40, 0);
    Put64(image + kGameStatusPointer, Address(status.data()));
    std::uint64_t modes[1] = {Address(entry.data())};
    Put64(status.data() + 0x20, Address(modes));
    Put64(entry.data() + 0x10, Address(info.data()));
    const auto setOnline = [&](bool online) { info[0x68] = online ? 1 : 0; };
    InitMission(image);
    setOnline(true);
    Check(OnlineSession(), "fake online session");

    Check(WorldHookHandler(kRetargetSite) != nullptr, "the site has a handler");
    Check(WorldHookHandler(0x123456) == nullptr, "unknown site has no handler");
    {
        World world;
        const std::uint64_t enemy = Address(world.enemy.bytes);
        const std::uint64_t keptBefore = KeptTargets();
        Check(RunHandler(enemy, 60) == 0, "a choice frame of a remote copy is skipped");
        Check(KeptTargets() == keptBefore + 1, "and counted");
        Check(RunHandler(enemy, 75) == 0, "also past the period");
        Check(RunHandler(enemy, 59) == 59, "a frame that is no choice frame is left alone");

        Put32(world.control.bytes + 8, 0);
        Check(RunHandler(enemy, 60) == 60, "a target whose last use is gone is chosen again");
        Put32(world.control.bytes + 8, 1);
        PutFloat(world.target.bytes + kObjectHealth, -1.0f);
        const std::uint64_t chosenBefore = LocalChoices();
        Check(RunHandler(enemy, 60) == 60, "a dead target is chosen again");
        Check(LocalChoices() == chosenBefore + 1, "and counted as a choice of its own");
        PutFloat(world.target.bytes + kObjectHealth, 5.0f);
        Put32(world.target.bytes + kObjectState, 4);
        Check(RunHandler(enemy, 60) == 60, "an unselectable target is chosen again");
        Put32(world.target.bytes + kObjectState, 0);
        world.SetTarget(false);
        Check(RunHandler(enemy, 60) == 60, "no target: the copy chooses one");
        world.SetTarget(true);
        world.enemy.bytes[kObjectNoTargetSync] = 1;
        Check(RunHandler(enemy, 60) == 60, "an owner that sends nothing (+0x2E8): the copy chooses");
        world.enemy.bytes[kObjectNoTargetSync] = 0;
        world.enemy.bytes[kObjectSendsTarget] = 0;
        Check(RunHandler(enemy, 60) == 60, "an owner that sends nothing (+0x591): the copy chooses");
        world.enemy.bytes[kObjectSendsTarget] = 1;
        Put16(world.enemy.bytes + kObjectNetFlags, 2);
        Check(RunHandler(enemy, 60) == 60, "the owner's own object chooses");
        Put16(world.enemy.bytes + kObjectNetFlags, 1);
        setOnline(false);
        Check(RunHandler(enemy, 60) == 60, "offline nothing changes");
        setOnline(true);
        // A target pointer to memory that is not there: read under SEH, the game decides.
        Put64(world.enemy.bytes + kObjectTarget, 0x10);
        Check(RunHandler(enemy, 60) == 60, "an unreadable target leaves the game's choice");
        world.SetTarget(true);
        Check(RunHandler(0x10, 60) == 60, "an unreadable object leaves the game's choice");
        WritePrivateProfileStringW(L"Netcode", L"EnemyTargets", L"0", wideIni);
        InitNetFeatures(wideIni);
        Check(RunHandler(enemy, 60) == 60, "EnemyTargets=0: the game's way");
        WritePrivateProfileStringW(L"Netcode", L"EnemyTargets", L"1", wideIni);
        InitNetFeatures(wideIni);
        Check(RunHandler(enemy, 60) == 0, "on again");

        // The hook installed on the game's instructions: the thunk runs the handler, then the displaced cmp, then
        // the game's jb decides with the flags of that cmp.
        auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        Check(code != nullptr, "code buffer");
        if (code) {
            std::memcpy(code, kSiteFunction, sizeof(kSiteFunction));
            const auto site = WorldHooks().front();
            Check(std::memcmp(code + kSiteOffset, site.original.data(), site.original.size()) == 0,
                  "the fake function holds the site's bytes");
            using SiteFn = int (*)(std::uint64_t counter, std::uint64_t object);
            const auto fn = reinterpret_cast<SiteFn>(code);
            Check(fn(60, enemy) == 0 && fn(10, enemy) == 1, "unhooked: the game chooses at 60, not at 10");
            ThunkPage page;
            Check(page.Allocate(code), "thunk page near the code");
            unsigned char* at = code + kSiteOffset;
            const auto resume = Address(at + site.original.size());
            unsigned char* thunk = EmitMidThunk(page, WorldHookHandler(site.rva), at + site.displacedOffset,
                                                site.displacedSize, resume);
            const auto jump = thunk ? JumpBytes(at, thunk, site.original.size()) : std::vector<std::uint8_t>{};
            Check(!jump.empty() && page.Seal(), "hook installed");
            if (!jump.empty()) {
                std::memcpy(at, jump.data(), jump.size());
                Check(fn(60, enemy) == 1, "hooked: a remote copy with a live target skips the choice");
                Check(fn(10, enemy) == 1, "and a frame that is no choice frame still skips");
                Put16(world.enemy.bytes + kObjectNetFlags, 2);
                Check(fn(60, enemy) == 0, "hooked: the owner still chooses");
                Put16(world.enemy.bytes + kObjectNetFlags, 1);
                world.SetTarget(false);
                Check(fn(60, enemy) == 0, "hooked: a copy without a target still chooses");
            }
            page.Release();
            VirtualFree(code, 0, MEM_RELEASE);
        }
    }

    DeleteFileW(wideIni);
    VirtualFree(image, 0, MEM_RELEASE);
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("enemy target authority verified\n");
    return 0;
}
