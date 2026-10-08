// Private mapped EDF.dll + production AF export; never calls EML6_Load or starts the game.
#include "../src/extension_api.h"
#include "../src/room_view.h"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>

namespace {
int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s (Win32=%lu)\n", what, GetLastError()); std::exit(1); }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::printf("usage: WorldAdmissionNativeTests <EDF.dll> <production EDF6VehicleCrew.dll>\n"); return 2; }
    const auto game = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    check(game != nullptr, "private game image mapped without imports, game initialization or UI");
    const auto base = reinterpret_cast<unsigned char*>(game);
    const unsigned char load[]{0x48, 0x8B, 0x35, 0xA7, 0x35, 0x9A, 0x01};
    const unsigned char store[]{0x89, 0xAE, 0x40, 0x16, 0, 0};
    check(std::memcmp(base + 0x70F51A, load, sizeof(load)) == 0 &&
          std::memcmp(base + 0x70F566, store, sizeof(store)) == 0, "real native location setter signatures");
    std::array<unsigned char, 0x1650> manager{};
    DWORD protection = 0;
    check(VirtualProtect(base + 0x20B2AC8, sizeof(void*), PAGE_READWRITE, &protection) != 0, "private manager slot writable");
    void* saved = nullptr;
    std::memcpy(&saved, base + 0x20B2AC8, sizeof(saved));
    void* network = manager.data();
    std::memcpy(base + 0x20B2AC8, &network, sizeof(network));
    const auto af = LoadLibraryW(argv[2]);
    check(af != nullptr, "production AF loaded without EML6_Load");
    const auto query = reinterpret_cast<EDF6AFGetMissionAdmissionStateFn>(GetProcAddress(af, "EDF6AF_GetMissionAdmissionState"));
    check(query != nullptr, "production admission ABI is present");
    const auto set = reinterpret_cast<void (__fastcall*)(void*, int)>(base + 0x70F500);
    auto admission = [&] {
        auto snapshot = std::make_unique<EDF6AFMissionAdmissionState>();
        snapshot->size = sizeof(*snapshot);
        check(query(EDF6AF_MISSION_ADMISSION_VERSION, sizeof(*snapshot), snapshot.get()) == 1, "production snapshot query");
        check(snapshot->size == sizeof(*snapshot) && snapshot->reserved == 0 &&
              snapshot->participantCount <= EDF6AF_MISSION_MAX_PARTICIPANTS, "production snapshot layout and bounds");
        dn::WorldAdmission world{static_cast<dn::WorldPhase>(snapshot->phase), snapshot->worldEpoch, {}, true};
        for (uint32_t i = 0; i < snapshot->participantCount; ++i) world.participants.emplace_back(snapshot->participants[i].id);
        auto message = dn::roomMessage({"host", "fresh"}, {});
        dn::appendWorldAdmission(message, world);
        return dn::parseWorldAdmission(message);
    };
    set(nullptr, 3);
    check(dn::freshWorldEntryAllowed(admission()), "initial real MENU_ROOM releases a fresh client before AF preload");
    set(nullptr, 5);
    check(!dn::freshWorldEntryAllowed(admission()), "real GAME_LOADING blocks before world creation");
    set(nullptr, 4);
    check(!dn::freshWorldEntryAllowed(admission()), "real GAME_PLAYING cannot release a fresh client with no sealed world");
    set(nullptr, 3);
    check(dn::freshWorldEntryAllowed(admission()), "native return to MENU_ROOM unlocks after mission end");
    set(nullptr, 5);
    check(!dn::freshWorldEntryAllowed(admission()), "next mission locks admission again");
    set(nullptr, 2);
    check(dn::freshWorldEntryAllowed(admission()), "real MENU_LOBBY allows normal room entry");
    set(nullptr, 0);
    check(!dn::freshWorldEntryAllowed(admission()), "BOOTING is not mistaken for Lobby");
    std::memcpy(base + 0x20B2AC8, &saved, sizeof(saved));
    VirtualProtect(base + 0x20B2AC8, sizeof(void*), protection, &protection);
    std::printf("%d native/production-ABI checks passed; no game or plugin initialization\n", checks);
    return 0;
}
