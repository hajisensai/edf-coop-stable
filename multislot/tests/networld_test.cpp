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
#include "../src/worldrng.h"

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

std::uint64_t FileImageBase(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> head(0x400);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head.data());
    if (dos->e_lfanew <= 0 || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > head.size()) return 0;
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(head.data() + dos->e_lfanew)->OptionalHeader.ImageBase;
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

    // The random state receive (54D79E) sits in 54D770 after the type was read into r8d and lowered by one for each
    // type the game took: `call 12B4BB0; movsx r8d, al; test al, al; je; sub r8d, 1; je` before it, and
    // `sub r8d, 1; je; cmp r8d, 1; jne 54D86F` after, so r8d there is type - 1 and every type above 3 returns.
    const auto before = FileBytes(path, 0x54D780, 0x1E);
    const std::vector<std::uint8_t> expectBefore = {0x48, 0x8B, 0xCA, 0xE8, 0x28, 0x74, 0xD6, 0x00, 0x44, 0x0F,
                                                    0xBE, 0xC0, 0x84, 0xC0, 0x0F, 0x84, 0x53, 0x01, 0x00, 0x00,
                                                    0x41, 0x83, 0xE8, 0x01, 0x0F, 0x84, 0x25, 0x01, 0x00, 0x00};
    Check(before == expectBefore, "the type is read and lowered by one before the random state site");
    const auto after = FileBytes(path, 0x54D7A3, 0x10);
    const std::vector<std::uint8_t> expectAfter = {0x41, 0x83, 0xE8, 0x01, 0x74, 0x50, 0x41, 0x83,
                                                   0xF8, 0x01, 0x0F, 0x85, 0xBC, 0x00, 0x00, 0x00};
    Check(after == expectAfter, "and a type the game does not take returns after it (jne 54D86F)");
    // The message writers the owner uses and the reader of the type agree on the small int (12B4BB0 = 12B4660).
    Check(FileBytes(path, 0x12B4BB0, 5) == std::vector<std::uint8_t>{0xE9, 0xAB, 0xFA, 0xFF, 0xFF}, "12B4BB0 jumps to 12B4660");
    // The random state fields: 545B85/545B94 seed +0x3E8 and +0x490 online; 54EE88 reads the team at +0x314.
    Check(FileBytes(path, 0x545B85, 7) == std::vector<std::uint8_t>{0x48, 0x89, 0x87, 0xE8, 0x03, 0x00, 0x00}, "seed of +0x3E8");
    Check(FileBytes(path, 0x545B94, 7) == std::vector<std::uint8_t>{0x48, 0x89, 0x87, 0x90, 0x04, 0x00, 0x00}, "seed of +0x490");
    Check(FileBytes(path, 0x54EE88, 6) == std::vector<std::uint8_t>{0x39, 0x91, 0x14, 0x03, 0x00, 0x00}, "team at +0x314");
    // The classes left to the game: their NetworkObject vtable's slot 6 is the read that carries +0x490.
    const std::uint64_t imageBase = FileImageBase(path);
    const struct {
        std::uint32_t vtable, read;
    } replicated[] = {{0x17BB990, 0x408400}, {0x17BC840, 0x408400}, {0x17BD0C0, 0x41D6A0}, {0x17BD818, 0x41D6A0},
                      {0x17BE308, 0x4365A0}, {0x17BE9C8, 0x4365A0}, {0x17C8638, 0x4DDEB0}, {0x17B8A58, 0x3DEDA0}};
    for (const auto& r : replicated) {
        const auto slot = FileBytes(path, r.vtable + 6 * 8, 8);
        std::uint64_t value = 0;
        if (slot.size() == 8) std::memcpy(&value, slot.data(), 8);
        Check(imageBase && value == imageBase + r.read, "a class left to the game reads its random state itself");
        Check(RngReplicatedByGame(r.vtable), "and is on the list");
    }
    // The insect pose interval immediates.
    for (const auto& patch : InsectPosePatches(30))
        Check(FileBytes(path, patch.rva, patch.original.size()) == patch.original, patch.name);
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

    // Feature switch: the default is on; [Netcode] WorldAuthority=0 turns it off.
    Check(!NetFeatureEnabledLocally(NetFeature::WorldAuthority), "off before the INI is read");
    char folder[MAX_PATH]{};
    GetTempPathA(MAX_PATH, folder);
    char iniPath[MAX_PATH]{};
    std::snprintf(iniPath, sizeof(iniPath), "%snetworld_test_%lu.ini", folder, GetCurrentProcessId());
    wchar_t wideIni[MAX_PATH]{};
    MultiByteToWideChar(CP_ACP, 0, iniPath, -1, wideIni, MAX_PATH);
    DeleteFileW(wideIni);
    InitNetFeature(wideIni);
    Check(NetFeatureEnabledLocally(NetFeature::WorldAuthority), "on by default");
    WritePrivateProfileStringW(L"Netcode", L"WorldAuthority", L"0", wideIni);
    InitNetFeature(wideIni);
    Check(!NetFeatureEnabledLocally(NetFeature::WorldAuthority), "WorldAuthority=0 turns it off");
    WritePrivateProfileStringW(L"Netcode", L"WorldAuthority", L"1", wideIni);
    InitNetFeature(wideIni);
    Check(NetFeatureEnabledLocally(NetFeature::WorldAuthority), "WorldAuthority=1 turns it on");

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
        WritePrivateProfileStringW(L"Netcode", L"WorldAuthority", L"0", wideIni);
        InitNetFeature(wideIni);
        Check(RunHandler(enemy, 60) == 60, "WorldAuthority=0: the game's way");
        WritePrivateProfileStringW(L"Netcode", L"WorldAuthority", L"1", wideIni);
        InitNetFeature(wideIni);
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

    // --- Random state sync: the message, the decisions, the schedule ---
    {
        RngSync sync;
        sync.seq = 0x01020304u;
        sync.state = 0x1122334455667788ull;
        sync.state2 = 0x99AABBCCDDEEFF00ull;
        std::uint8_t bytes[kRngSyncBytes + 4]{};
        Check(WriteRngSync(sync, bytes, kRngSyncBytes - 1) == 0, "no room: nothing written");
        Check(WriteRngSync(sync, bytes, sizeof(bytes)) == kRngSyncBytes, "a message is 23 bytes");
        Check(bytes[0] == 'R' && bytes[1] == 'S' && bytes[2] == 1 && bytes[3] == 0x04 && bytes[7] == 0x88 && bytes[15] == 0x00,
              "little endian after magic and version");
        RngSync back;
        Check(ReadRngSync(bytes, kRngSyncBytes, back) && back.seq == sync.seq && back.state == sync.state &&
                  back.state2 == sync.state2,
              "it reads back");
        Check(!ReadRngSync(bytes, kRngSyncBytes - 1, back) && !ReadRngSync(bytes, kRngSyncBytes + 1, back),
              "a message of another size is refused");
        bytes[2] = 2;
        Check(!ReadRngSync(bytes, kRngSyncBytes, back), "another version is refused");
        bytes[2] = 1;
        bytes[0] = 'H';
        Check(!ReadRngSync(bytes, kRngSyncBytes, back), "another magic (a hit event) is refused");
        Check(!ReadRngSync(nullptr, kRngSyncBytes, back), "no data");

        RngSendView v;
        v.active = true;
        v.online = true;
        v.netFlags = 2;
        v.team = kEnemyTeam;
        v.networkVtableRva = 0x17C29C0;  // HumanBase family: not replicated by the game
        Check(ShouldSyncRng(v), "the owner of an enemy whose class does not sync sends");
        RngSendView w = v;
        w.active = false;
        Check(!ShouldSyncRng(w), "not while the room does not run it");
        w = v;
        w.online = false;
        Check(!ShouldSyncRng(w), "not offline");
        w = v;
        w.netFlags = 1;
        Check(!ShouldSyncRng(w), "not a copy");
        w.netFlags = 3;
        Check(!ShouldSyncRng(w), "not an object both flags call another machine's");
        w.netFlags = 0;
        Check(!ShouldSyncRng(w), "not an unregistered object");
        w = v;
        w.team = 0;
        Check(!ShouldSyncRng(w), "not a player or a friend");
        w = v;
        w.networkVtableRva = 0x17BB990;
        Check(!ShouldSyncRng(w), "not an ant: the game syncs it");
        w.networkVtableRva = 0;
        Check(!ShouldSyncRng(w), "not an object whose vtable is outside the game");

        RngSchedule schedule;
        Check(schedule.Due(0x1000, 1000, 2000), "the first time is due");
        Check(!schedule.Due(0x1000, 2999, 2000), "not again within the interval");
        Check(schedule.Due(0x1000, 3000, 2000), "due after it");
        Check(schedule.Due(0x2000, 3000, 2000), "every object on its own");
        Check(schedule.Due(0x3000, 100000, 2000) && schedule.size() == 1, "objects not seen for 30 s are forgotten");

        RngReceiver receiver;
        Check(receiver.Accept(0x1000, 10, 1000), "the first message of an object is taken");
        Check(!receiver.Accept(0x1000, 10, 1001), "the same again is stale");
        Check(!receiver.Accept(0x1000, 9, 1002), "an older one is stale");
        Check(receiver.Accept(0x1000, 11, 1003), "a newer one is taken");
        Check(receiver.Accept(0x2000, 1, 1003), "another object's sequence is its own");
        Check(receiver.Accept(0x3000, 0xFFFFFFFFu, 1004) && receiver.Accept(0x3000, 2, 1005), "sequences wrap");
        Check(receiver.Accept(0x1000, 5, 100000), "after 30 s without one the object starts over");

        Check(RngMessageType(13) && !RngMessageType(12) && !RngMessageType(14) && RngMessageType(0xFFFFFFFF0000000Dull),
              "r8d = type - 1 = 13 is ours, whatever the upper half");
        Check(WorldHookHandler(kRngReceiveSite) != nullptr, "the receive site has a handler");
        CpuContext other{};
        other.r8 = 12;  // type 13: hit authority's, not ours
        WorldHookHandler(kRngReceiveSite)(&other);
        const RngCounters counters = RngSyncCounters();
        Check(counters.applied == 0 && counters.malformed == 0 && counters.inactive == 0, "other types are left alone");

        Check(InsectPosePatches(0).empty() && InsectPosePatches(90).empty() && InsectPosePatches(2).empty() &&
                  InsectPosePatches(200).empty(),
              "the game's intervals stay for 0, 90 and values outside 3..89");
        const auto thirty = InsectPosePatches(30);
        Check(thirty.size() == 2 && thirty[0].rva == kInsectPoseFast && thirty[0].replacement[1] == 30 &&
                  thirty[1].rva == kInsectPoseSlow && thirty[1].replacement[1] == 30,
              "30 frames: both 40 and 90 become 30");
        const auto sixty = InsectPosePatches(60);
        Check(sixty.size() == 2 && sixty[0].replacement[1] == 40 && sixty[1].replacement[1] == 60,
              "60 frames: 40 stays, 90 becomes 60");
        for (const auto& patch : thirty)
            Check(patch.original.size() == patch.replacement.size() && patch.original[0] == patch.replacement[0],
                  "only the immediate changes");

        // The receive site's displaced store (rsp-relative) runs on the game's frame: f(x) stores x with it at
        // [rsp+0x30] and loads it back.
        //   push rsi; sub rsp, 0x40; mov rsi, rcx; [mov [rsp+0x30], rsi]; mov rax, [rsp+0x30]; add rsp, 0x40; pop rsi; ret
        const unsigned char storeFunction[] = {0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x89, 0xCE, 0x48, 0x89, 0x74, 0x24, 0x30,
                                               0x48, 0x8B, 0x44, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x40, 0x5E, 0xC3};
        auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        Check(code != nullptr, "code buffer");
        if (code) {
            std::memcpy(code, storeFunction, sizeof(storeFunction));
            const auto site = WorldHooks().back();
            unsigned char* at = code + 8;
            Check(site.rva == kRngReceiveSite && std::memcmp(at, site.original.data(), site.original.size()) == 0,
                  "the fake function holds the receive site's bytes");
            ThunkPage page;
            Check(page.Allocate(code), "thunk page near the code");
            unsigned char* thunk = EmitMidThunk(page, WorldHookHandler(site.rva), at + site.displacedOffset, site.displacedSize,
                                                Address(at + site.original.size()));
            const auto jump = thunk ? JumpBytes(at, thunk, site.original.size()) : std::vector<std::uint8_t>{};
            Check(!jump.empty() && page.Seal(), "receive hook installed");
            if (!jump.empty()) {
                std::memcpy(at, jump.data(), jump.size());
                const auto fn = reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(code);
                Check(fn(0x123456789ABCull) == 0x123456789ABCull, "the displaced store lands in the game's frame");
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
