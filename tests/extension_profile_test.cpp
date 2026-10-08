#include "../src/lobby_marker.h"
#include <cstdio>
#include <string>
namespace {
int failures = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
template<typename T> T function(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    HMODULE eos = LoadLibraryA(argv[1]);
    if (!eos) return 2;
    const auto reset = function<void(*)(const char*)>(eos, "FakeEos_Reset");
    const auto lobby = function<void(*)(const char*, const char*)>(eos, "FakeEos_EnterLobby");
    const auto user = function<EOS_ProductUserId(*)(const char*)>(eos, "FakeEos_User");
    const auto tick = function<void(*)(void*)>(eos, "EOS_Platform_Tick");
    const auto text = function<const char*(*)(const char*, const char*)>(eos, "FakeEos_Text");
    const auto setText = function<void(*)(const char*, const char*, const char*)>(eos, "FakeEos_SetText");
    const auto clear = function<void(*)(const char*)>(eos, "FakeEos_ClearTexts");
    const auto add = function<void(*)(const char*)>(eos, "FakeEos_AddMember");
    const auto copyFails = function<void(*)(int)>(eos, "FakeEos_SetCopyFails");
    if (!reset || !lobby || !user || !tick || !text || !setText || !clear || !add || !copyFails) return 2;
    reset("self"); lobby("room", "self");
    dn::LobbyMarker marker;
    CHECK(marker.init(eos));
    marker.entered(reinterpret_cast<EOS_HLobby>(1), "room", user("self"), true);
    marker.setIdentity("0123456789abcdef0123456789abcdef");
    marker.tick(); tick(nullptr);
    auto published = [&](const char* key) { const char* value = text("self", key); return value ? std::string(value) : ""; };
    // EOS rejects empty text attributes atomically: disabled extensions must not
    // break vanilla Coop's identity/host-address publication.
    CHECK(published("EDF6DN_EXT") == "disabled");
    CHECK(published("EDF6DN_ID") == "0123456789abcdef0123456789abcdef");
    CHECK(!marker.extensionCompatible());
    HMODULE af = LoadLibraryA(argv[2]);
    if (!af) return 2;
    const auto ready = function<void(*)(bool)>(af, "FakeAF_SetReady");
    const auto version = function<void(*)(unsigned)>(af, "FakeAF_SetVersion");
    if (!ready || !version) return 2;
    marker.memberJoined(); marker.tick(); tick(nullptr);
    CHECK(published("EDF6DN_EXT") == "af-support/1");
    CHECK(marker.extensionCompatible());
    version(2); CHECK(!marker.extensionCompatible()); version(1);
    add("peer");
    CHECK(!marker.extensionCompatible()); // older Coop/absent extension
    setText("peer", "EDF6DN_EXT", "af-support/2");
    CHECK(!marker.extensionCompatible());
    setText("peer", "EDF6DN_EXT", "af-support/1");
    CHECK(marker.extensionCompatible());
    clear("peer");
    CHECK(!marker.extensionCompatible()); // no cached capability after it disappears
    setText("peer", "EDF6DN_EXT", "af-support/1");
    copyFails(1); CHECK(!marker.extensionCompatible()); copyFails(0);
    ready(false); CHECK(!marker.extensionCompatible());
    marker.memberJoined(); marker.tick(); tick(nullptr);
    CHECK(published("EDF6DN_EXT") == "disabled");
    ready(true); CHECK(!marker.extensionCompatible()); // own current marker still disabled
    marker.memberJoined(); marker.tick(); tick(nullptr);
    CHECK(marker.extensionCompatible());
    marker.left(); CHECK(!marker.extensionCompatible());
    FreeLibrary(af); FreeLibrary(eos);
    printf("extension profile: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
