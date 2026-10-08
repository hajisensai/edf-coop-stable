cmake_minimum_required(VERSION 3.20)
# EDF6Coop: the room part (src/ here) and the direct-link part (../src) as one plugin, EDF6Coop.dll.
project(EDF6Coop VERSION 2.5.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# One build for every room size since 2.3.0: slots for kMaxPlayers (src/patches.h), the size of a room chosen by its
# host. The INI template names that number; it is read from patches.h so the two cannot disagree.
file(READ "${CMAKE_SOURCE_DIR}/src/patches.h" PATCHES_H)
if(NOT PATCHES_H MATCHES "constexpr int kMaxPlayers = ([0-9]+);")
    message(FATAL_ERROR "src/patches.h has no `constexpr int kMaxPlayers = N;`")
endif()
set(MULTISLOT_MAX_PLAYERS ${CMAKE_MATCH_1})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/patches.h")
# The version EDFModLoader is told (PluginInfo) and the one kVersion must start with (plugin.cpp) come from here.
add_compile_definitions(MULTISLOT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MULTISLOT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
                        MULTISLOT_VERSION_PATCH=${PROJECT_VERSION_PATCH})
set(MULTISLOT_DIST "${CMAKE_SOURCE_DIR}/dist")
# Words the INI template fills in (EDF6MultiSlot.ini): the last player number, and the last ghost count.
math(EXPR MULTISLOT_MAX_GHOSTS "${MULTISLOT_MAX_PLAYERS} - 1")

# PluginAPI.h from EDFModLoader (BlueAmulet, MIT), next to the other projects.
set(MODLOADER_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/EDFModLoader")

# The installed game this build reads from (read-only): EDF.dll for the patch-table and load tests, Root.cpk
# for tools/make_menu_label.py. Pass -DEDF6_GAME_DIR=... or set the EDF6_GAME_DIR environment variable.
if(DEFINED ENV{EDF6_GAME_DIR})
    set(EDF6_GAME_DIR_DEFAULT "$ENV{EDF6_GAME_DIR}")
else()
    set(EDF6_GAME_DIR_DEFAULT "${CMAKE_SOURCE_DIR}/..")
endif()
set(EDF6_GAME_DIR "${EDF6_GAME_DIR_DEFAULT}" CACHE PATH "EARTH DEFENSE FORCE 6 install folder (holds EDF.dll)")
set(WARNINGS /W4 /WX /utf-8 /permissive-)

# The default settings the plugin writes when no INI exists come from EDF6MultiSlot.ini itself.
file(READ "${CMAKE_SOURCE_DIR}/EDF6MultiSlot.ini" MULTISLOT_DEFAULT_INI)
# The INI is a template for the slot count (@MULTISLOT_MAX_PLAYERS@, @MULTISLOT_MAX_GHOSTS@).
string(CONFIGURE "${MULTISLOT_DEFAULT_INI}" MULTISLOT_DEFAULT_INI @ONLY)
configure_file(src/default_ini.h.in "${CMAKE_BINARY_DIR}/generated/default_ini.h" @ONLY)
# Files the plugin writes under Mods while it is active (src/modfile.h), made from the game's own files and so not
# in the repository: the menu frame layout with the 8Player MOD label field (tools/make_menu_label.py) and the
# online HUD textures with a lamp and chat balloon per player colour (tools/make_hud_colours.py).
# CI has no game: MULTISLOT_CI=ON builds the plugin and every test that does not need the game with placeholders in
# place of the missing files. A placeholder names itself (MULTISLOT CI PLACEHOLDER), the plugin never writes one into
# a game folder, and package.ps1 refuses a DLL that carries it, so such a build cannot ship.
option(MULTISLOT_CI "Build without the assets/ files (placeholders; the result cannot be packaged)" OFF)
set(MULTISLOT_PLACEHOLDER_ASSETS OFF)
set(BYTE "0x[0-9a-f][0-9a-f],")
# The bytes of `file` as a C array initializer, 16 to a line, in `out`; `placeholder` (hex) when it is missing.
function(embed_asset file tool placeholder out)
    if(EXISTS "${file}")
        file(READ "${file}" hex HEX)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
    elseif(MULTISLOT_CI)
        message(WARNING "MULTISLOT_CI: no ${file}, so the plugin embeds a placeholder. This build is for tests only; "
                        "package.ps1 refuses it")
        set(MULTISLOT_PLACEHOLDER_ASSETS ON PARENT_SCOPE)
        set(hex "${placeholder}")
    else()
        message(FATAL_ERROR "${file} is missing: run `python -B ${tool}` with EDF6_GAME_DIR set to the game folder "
                            "first (it reads Root.cpk there)")
    endif()
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "(${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE})"
           "\\1\n" bytes "${bytes}")
    set(${out} "${bytes}" PARENT_SCOPE)
endfunction()
string(HEX "MULTISLOT CI PLACEHOLDER - not the game's file, never packaged" MARKER_HEX)
# Shaped like the real files, so the file handling tests have something to work on: an SGO header, the marker and
# the label name in UTF-16; an SSA header and the marker.
string(HEX "SGO" SGO_HEX)
string(HEX "SSA" SSA_HEX)
embed_asset("${CMAKE_SOURCE_DIR}/assets/LYT_MAINFRAME.SGO" tools/make_menu_label.py
            "${SGO_HEX}00${MARKER_HEX}004d0053004c006100620065006c000000" MENU_LAYOUT_BYTES)
embed_asset("${CMAKE_SOURCE_DIR}/assets/ONLINEHUDTEXTURE.RAB" tools/make_hud_colours.py
            "${SSA_HEX}00${MARKER_HEX}00" HUD_ARCHIVE_BYTES)
if(MULTISLOT_PLACEHOLDER_ASSETS)
    add_compile_definitions(MULTISLOT_PLACEHOLDER_ASSETS=1)
endif()
configure_file(src/mod_assets.h.in "${CMAKE_BINARY_DIR}/generated/mod_assets.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/EDF6MultiSlot.ini")

# The direct-link part, compiled with its own flags (it is not built for /permissive- and defines what the room
# part's sources define themselves).
set(DIRECTNET_DIR "${CMAKE_SOURCE_DIR}/../src")
set(DIRECTNET_CORE log auth wire reliable direct_net netif config iat hold traffic updater upnp room_view
    fake_lobby extension_transport extension_bridge)
list(TRANSFORM DIRECTNET_CORE PREPEND "${DIRECTNET_DIR}/")
list(TRANSFORM DIRECTNET_CORE APPEND ".cpp")
set(DIRECTNET_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
add_library(DirectNetCore STATIC ${DIRECTNET_CORE})
target_compile_definitions(DirectNetCore PRIVATE ${DIRECTNET_DEFINES})
target_compile_options(DirectNetCore PRIVATE /W4 /WX /utf-8 /EHsc)
target_link_libraries(DirectNetCore PUBLIC ws2_32 iphlpapi bcrypt ole32 oleaut32 winhttp)
add_library(DirectNetPart STATIC "${DIRECTNET_DIR}/lobby_marker.cpp" "${DIRECTNET_DIR}/eos_hooks.cpp"
    "${DIRECTNET_DIR}/dn_part.cpp")
target_compile_definitions(DirectNetPart PRIVATE ${DIRECTNET_DEFINES})
target_compile_options(DirectNetPart PRIVATE /W4 /WX /utf-8 /EHsc)
target_link_libraries(DirectNetPart PUBLIC DirectNetCore)

add_library(EDF6Coop SHARED
    src/plugin.cpp src/coop.cpp src/patches.cpp src/rooms.cpp src/roomview.cpp src/fakemembers.cpp src/hostmode.cpp src/modfile.cpp src/hud.cpp
    src/mission.cpp src/spawn.cpp src/joinlog.cpp src/armor.cpp src/midhook.cpp src/code.cpp src/crashlog.cpp src/log.cpp
    src/netlog.cpp src/identity.cpp src/smoothing.cpp src/updatecheck.cpp src/peertimeout.cpp src/packetfit.cpp
    src/syncmarker.cpp src/lobbystate.cpp src/hostdata.cpp src/hostdataopen.cpp src/hostdatanet.cpp src/hostdataprompt.cpp src/weaponguard.cpp
    src/userslots.cpp src/networld.cpp src/worldrng.cpp src/hitauth.cpp src/hitauthgame.cpp src/netplayer.cpp src/netplayer_game.cpp src/netplayer_members.cpp
    src/widecmp.cpp src/interest.cpp src/vectoralloc.cpp)
target_link_libraries(EDF6Coop PRIVATE DirectNetPart winhttp)
# "src/config.h" and the like name the direct-link part's headers (both parts have a log.h).
target_include_directories(EDF6Coop PRIVATE ${MODLOADER_INCLUDE} "${CMAKE_BINARY_DIR}/generated" "${CMAKE_SOURCE_DIR}/..")
target_compile_options(EDF6Coop PRIVATE ${WARNINGS})
target_link_options(EDF6Coop PRIVATE /DYNAMICBASE /NXCOMPAT /INCREMENTAL:NO /PDBALTPATH:EDF6Coop.pdb)
set_target_properties(EDF6Coop PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MULTISLOT_DIST}")

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${MULTISLOT_DIST}/winmm.dll"
    "${CMAKE_BINARY_DIR}/loader-fix.json"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/fix_winmm_proxy.py"
        "${CMAKE_SOURCE_DIR}/third_party/EDFModLoader/winmm.dll"
        "${MULTISLOT_DIST}/winmm.dll"
        --manifest "${CMAKE_BINARY_DIR}/loader-fix.json"
    DEPENDS tools/fix_winmm_proxy.py tools/pe.py third_party/EDFModLoader/winmm.dll
    VERBATIM)
add_custom_target(LoaderProxyFix ALL DEPENDS "${MULTISLOT_DIST}/winmm.dll"
    "${CMAKE_BINARY_DIR}/loader-fix.json")
add_executable(LoaderProxyTests tests/loader_proxy_test.cpp)
add_dependencies(LoaderProxyTests LoaderProxyFix)
target_compile_options(LoaderProxyTests PRIVATE ${WARNINGS})
add_test(NAME LoaderProxyRace COMMAND LoaderProxyTests
    "${CMAKE_SOURCE_DIR}/third_party/EDFModLoader/winmm.dll" "${MULTISLOT_DIST}/winmm.dll")
add_executable(HandshakeTests tests/handshake_test.cpp src/joinlog.cpp src/identity.cpp src/log.cpp)
target_compile_options(HandshakeTests PRIVATE ${WARNINGS})
add_test(NAME HandshakeUserSnapshots COMMAND HandshakeTests "${CMAKE_BINARY_DIR}/handshake-diagnostics-test.log")
add_executable(PeerTimeoutTests tests/peertimeout_test.cpp src/peertimeout.cpp src/identity.cpp src/log.cpp)
target_compile_options(PeerTimeoutTests PRIVATE ${WARNINGS})
add_test(NAME PeerTimeoutKeepsRoom COMMAND PeerTimeoutTests "${CMAKE_BINARY_DIR}/peer-timeout-test.log")
add_executable(NetLogForwardTests tests/netlog_forward_test.cpp src/joinlog.cpp src/identity.cpp src/log.cpp src/packetfit.cpp)
target_compile_options(NetLogForwardTests PRIVATE ${WARNINGS})
add_test(NAME NetworkImportForwarding COMMAND NetLogForwardTests "${CMAKE_BINARY_DIR}/netlog-forward-test.log")
add_executable(PacketFitTests tests/packetfit_test.cpp src/packetfit.cpp src/log.cpp)
target_compile_options(PacketFitTests PRIVATE ${WARNINGS})
add_test(NAME MissionStartMessageFits COMMAND PacketFitTests "${CMAKE_BINARY_DIR}/packetfit-test.log")
# A stand-in EOS SDK under the real DLL's name, for the lobby glue (fake_eos.cpp).
add_library(FakeEosSdk SHARED tests/fake_eos.cpp)
set_target_properties(FakeEosSdk PROPERTIES OUTPUT_NAME EOSSDK-Win64-Shipping)
add_executable(SyncMarkerTests tests/syncmarker_test.cpp src/syncmarker.cpp src/packetfit.cpp src/identity.cpp src/log.cpp)
target_compile_options(SyncMarkerTests PRIVATE ${WARNINGS})
add_dependencies(SyncMarkerTests FakeEosSdk)
add_test(NAME SplitSyncLobbyMarker COMMAND SyncMarkerTests "${CMAKE_BINARY_DIR}/syncmarker-test.log"
    "$<TARGET_FILE:FakeEosSdk>")
add_executable(LobbyStateTests tests/lobbystate_test.cpp src/lobbystate.cpp src/hostmode.cpp src/roomview.cpp
    src/mission.cpp src/armor.cpp src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp src/identity.cpp
    src/joinlog.cpp src/updatecheck.cpp)
target_link_libraries(LobbyStateTests PRIVATE winhttp)
target_compile_options(LobbyStateTests PRIVATE ${WARNINGS})
add_dependencies(LobbyStateTests FakeEosSdk)
add_test(NAME LobbyStateAndGhostMembers COMMAND LobbyStateTests "${CMAKE_BINARY_DIR}/lobbystate-test.log"
    "$<TARGET_FILE:FakeEosSdk>")
set(EDF_DLL "${EDF6_GAME_DIR}/EDF.dll")
# The tests against the game's code are registered either way. Without EDF.dll they report themselves skipped
# (exit code 77), so ctest names them in its summary instead of a configure warning nobody reads in CI.
set(GAME_TEST_SKIPPED 77)
if(NOT EXISTS "${EDF_DLL}")
    message(WARNING "No EDF.dll in EDF6_GAME_DIR (${EDF6_GAME_DIR}): the tests against the game's code "
                    "(PatchTablesMatchEDF, PluginLoad_*, GameNet_*) will be reported as skipped")
endif()

add_executable(MultiSlotTests tests/tests.cpp src/hud.cpp src/patches.cpp src/rooms.cpp src/mission.cpp src/midhook.cpp src/code.cpp src/log.cpp src/smoothing.cpp
    src/userslots.cpp src/vectoralloc.cpp)
target_compile_options(MultiSlotTests PRIVATE ${WARNINGS})
add_test(NAME PatchTablesMatchEDF COMMAND MultiSlotTests "${EDF_DLL}")
set_tests_properties(PatchTablesMatchEDF PROPERTIES SKIP_RETURN_CODE ${GAME_TEST_SKIPPED})

# Hit authority (src/hitauth.h): the event, the decision, the owner's checks; and against EDF.dll, the hook sites,
# the game facts the rule rests on and the event's round trip through the game's own stream code.
add_executable(HitAuthTests tests/hitauth_test.cpp src/hitauth.cpp src/hitauthgame.cpp src/netfeature.cpp
    src/netcompress.cpp src/syncmarker.cpp src/packetfit.cpp src/identity.cpp
    src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp)
target_include_directories(HitAuthTests PRIVATE "${CMAKE_SOURCE_DIR}/..")
target_compile_options(HitAuthTests PRIVATE ${WARNINGS})
target_link_libraries(HitAuthTests PRIVATE DirectNetPart)
add_test(NAME HitAuthority COMMAND HitAuthTests)
add_test(NAME HitAuthorityGameCode COMMAND HitAuthTests "${EDF_DLL}")
set_tests_properties(HitAuthorityGameCode PROPERTIES SKIP_RETURN_CODE ${GAME_TEST_SKIPPED})

add_executable(RoomViewTests tests/roomview_test.cpp src/roomview.cpp src/armor.cpp src/mission.cpp src/midhook.cpp
    src/code.cpp src/patches.cpp src/log.cpp src/identity.cpp src/joinlog.cpp)
target_compile_options(RoomViewTests PRIVATE ${WARNINGS})
add_test(NAME RoomViewPaging COMMAND RoomViewTests)

add_executable(MidHookTests tests/midhook_test.cpp src/midhook.cpp src/code.cpp src/patches.cpp)
target_compile_options(MidHookTests PRIVATE ${WARNINGS})
add_test(NAME MidFunctionHooks COMMAND MidHookTests)

# Room size (docs/net-re/roomsize.md): widened compares run in-process.
add_executable(WideCompareTests tests/widecmp_test.cpp src/widecmp.cpp src/code.cpp src/patches.cpp)
target_compile_options(WideCompareTests PRIVATE ${WARNINGS})
add_test(NAME WideCompares COMMAND WideCompareTests)

# Interest management for large rooms (src/interest.h): bandwidth-driven priority accumulation.
add_executable(InterestTests tests/interest_test.cpp src/interest.cpp)
target_compile_options(InterestTests PRIVATE ${WARNINGS})
add_test(NAME InterestManagement COMMAND InterestTests)

add_executable(MissionTests tests/mission_test.cpp src/mission.cpp src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp)
target_compile_options(MissionTests PRIVATE ${WARNINGS})
add_test(NAME MissionSlots COMMAND MissionTests)

add_executable(HostModeTests tests/hostmode_test.cpp src/hostmode.cpp src/roomview.cpp src/mission.cpp src/armor.cpp
    src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp src/identity.cpp src/joinlog.cpp src/updatecheck.cpp
    src/lobbystate.cpp)
target_link_libraries(HostModeTests PRIVATE winhttp)
target_compile_options(HostModeTests PRIVATE ${WARNINGS})
add_test(NAME HostModeAndMenuLabel COMMAND HostModeTests)

# W2 player sync: block format, the game's bin element, sender velocity, extrapolation and convergence.
add_executable(NetPlayerTests tests/netplayer_test.cpp src/netplayer.cpp src/netplayer_members.cpp src/netaoi.cpp
    src/interest.cpp src/netfeature.cpp src/netcompress.cpp
    src/syncmarker.cpp src/packetfit.cpp src/identity.cpp src/log.cpp)
target_include_directories(NetPlayerTests PRIVATE "${CMAKE_SOURCE_DIR}/..")
target_compile_options(NetPlayerTests PRIVATE ${WARNINGS})
target_link_libraries(NetPlayerTests PRIVATE DirectNetPart)
add_test(NAME NetPlayerSync COMMAND NetPlayerTests)
add_executable(WeaponGuardTests tests/weaponguard_test.cpp src/weaponguard.cpp src/log.cpp)
target_compile_options(WeaponGuardTests PRIVATE ${WARNINGS})
add_test(NAME WeaponGuard COMMAND WeaponGuardTests)
add_executable(HostDataTests tests/hostdata_test.cpp src/hostdata.cpp)
target_include_directories(HostDataTests PRIVATE "${CMAKE_SOURCE_DIR}/..")
target_compile_options(HostDataTests PRIVATE ${WARNINGS})
target_link_libraries(HostDataTests PRIVATE DirectNetCore)
add_test(NAME HostDataBundles COMMAND HostDataTests "${CMAKE_BINARY_DIR}/hostdatatest")
add_executable(HostDataNetTests tests/hostdatanet_test.cpp src/hostdatanet.cpp src/hostdata.cpp src/hostdataopen.cpp
    src/hostdataprompt.cpp
    src/syncmarker.cpp src/packetfit.cpp src/identity.cpp src/log.cpp)
target_include_directories(HostDataNetTests PRIVATE "${CMAKE_SOURCE_DIR}/..")
target_compile_options(HostDataNetTests PRIVATE ${WARNINGS})
target_link_libraries(HostDataNetTests PRIVATE DirectNetCore)
add_test(NAME HostDataLinkTransfer COMMAND HostDataNetTests)

add_executable(ModFileTests tests/modfile_test.cpp src/modfile.cpp)
target_include_directories(ModFileTests PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_compile_options(ModFileTests PRIVATE ${WARNINGS})
add_test(NAME ModFiles COMMAND ModFileTests "${CMAKE_BINARY_DIR}/modfiletest")

add_executable(DefaultIniTests tests/default_ini_test.cpp)
target_include_directories(DefaultIniTests PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_compile_options(DefaultIniTests PRIVATE ${WARNINGS})
add_test(NAME DefaultIniKeys COMMAND DefaultIniTests "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/initest")

add_executable(SpawnTests tests/spawn_test.cpp src/spawn.cpp src/mission.cpp src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp)
target_compile_options(SpawnTests PRIVATE ${WARNINGS})
add_test(NAME EnemySpawnCounts COMMAND SpawnTests)

# Netcode W4 (src/networld.h): the enemy target decision and its hook against a fake game; with EDF.dll also the
# hook site against the game's code (skipped without it).
add_executable(NetWorldTests tests/networld_test.cpp src/networld.cpp src/worldrng.cpp src/netfeature.cpp src/netcompress.cpp src/syncmarker.cpp
    src/packetfit.cpp src/identity.cpp src/mission.cpp src/midhook.cpp
    src/code.cpp src/patches.cpp src/log.cpp)
target_include_directories(NetWorldTests PRIVATE "${CMAKE_SOURCE_DIR}/..")
target_compile_options(NetWorldTests PRIVATE ${WARNINGS})
target_link_libraries(NetWorldTests PRIVATE DirectNetPart)
add_test(NAME NetWorldEnemyTargets COMMAND NetWorldTests)
add_test(NAME NetWorldSitesMatchEDF COMMAND NetWorldTests "${EDF_DLL}")
set_tests_properties(NetWorldSitesMatchEDF PROPERTIES SKIP_RETURN_CODE ${GAME_TEST_SKIPPED})

add_executable(FakeMemberTests tests/fakemembers_test.cpp src/fakemembers.cpp src/roomview.cpp src/armor.cpp
    src/mission.cpp src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp src/identity.cpp src/joinlog.cpp)

add_executable(ArmorTests tests/armor_test.cpp src/armor.cpp src/mission.cpp src/midhook.cpp src/code.cpp
    src/patches.cpp src/log.cpp)
target_compile_options(ArmorTests PRIVATE ${WARNINGS})
add_test(NAME CopyArmor COMMAND ArmorTests)
target_compile_options(FakeMemberTests PRIVATE ${WARNINGS})
add_test(NAME FakeRoomMembers COMMAND FakeMemberTests)

add_executable(LogTests tests/log_test.cpp src/log.cpp)
target_compile_options(LogTests PRIVATE ${WARNINGS})
add_test(NAME LogFileCap COMMAND LogTests "${CMAKE_BINARY_DIR}/logtest")

add_executable(CrashDumpTests tests/crashdump_test.cpp src/crashlog.cpp src/log.cpp)
target_compile_options(CrashDumpTests PRIVATE ${WARNINGS})
add_test(NAME CrashDump COMMAND CrashDumpTests "${CMAKE_BINARY_DIR}/crashdumptest")
add_test(NAME CrashDumpInPlugin COMMAND CrashDumpTests "${CMAKE_BINARY_DIR}/crashdumptest" plugin)

# A plugin that refuses (Enabled=0, or no supported EDF.dll) is unloaded by EDFModLoader at once: nothing of it
# may keep running. Needs no game: without EDF.dll in the process the plugin refuses as it does in an unsupported one.
add_executable(RefusalTests tests/refusal_test.cpp)
target_include_directories(RefusalTests PRIVATE ${MODLOADER_INCLUDE})
target_compile_options(RefusalTests PRIVATE ${WARNINGS})
add_dependencies(RefusalTests EDF6Coop)
add_test(NAME PluginRefusalUnloads COMMAND RefusalTests "$<TARGET_FILE:EDF6Coop>" "${CMAKE_BINARY_DIR}/refusaltest")

add_executable(MultiSlotLoadTest tests/load_test.cpp src/patches.cpp src/midhook.cpp src/code.cpp src/widecmp.cpp)
target_include_directories(MultiSlotLoadTest PRIVATE ${MODLOADER_INCLUDE} "${CMAKE_BINARY_DIR}/generated")
target_compile_options(MultiSlotLoadTest PRIVATE ${WARNINGS})
add_dependencies(MultiSlotLoadTest EDF6Coop)
foreach(mode off host4 host8 fresh upgrade nomission recoveryoff keeproomoff quiet smooth)
    add_test(NAME PluginLoad_${mode} COMMAND MultiSlotLoadTest "${EDF_DLL}" "$<TARGET_FILE:EDF6Coop>"
             "${CMAKE_BINARY_DIR}/loadtest" ${mode})
    set_tests_properties(PluginLoad_${mode} PROPERTIES SKIP_RETURN_CODE ${GAME_TEST_SKIPPED})
endforeach()

# Rooms played by several processes, each running the real EDF.dll and the real EDF6Coop.dll with a fake EOS
# between them (tests/gamenet). The fake is named as Epic's SDK and sits next to GameMachine.exe, where the loader
# looks first, so it never meets the FakeEosSdk above. Skipped (77) without the game.
set(GAMENET_DIR "${CMAKE_BINARY_DIR}/gamenet")
add_library(GameNetEos SHARED tests/gamenet/fake_eos_net.cpp)
set_target_properties(GameNetEos PROPERTIES OUTPUT_NAME EOSSDK-Win64-Shipping RUNTIME_OUTPUT_DIRECTORY "${GAMENET_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${GAMENET_DIR}")
target_compile_options(GameNetEos PRIVATE ${WARNINGS})
add_executable(GameMachine tests/gamenet/machine.cpp tests/gamenet/roles.cpp tests/gamenet/game.cpp
    tests/gamenet/missionsync.cpp src/netplayer.cpp)
set_target_properties(GameMachine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${GAMENET_DIR}")
target_include_directories(GameMachine PRIVATE ${MODLOADER_INCLUDE})
target_compile_options(GameMachine PRIVATE ${WARNINGS})
add_dependencies(GameMachine GameNetEos)
add_executable(GameNetTests tests/gamenet/gamenet_test.cpp)
target_compile_options(GameNetTests PRIVATE ${WARNINGS})
add_dependencies(GameNetTests GameMachine EDF6Coop)
add_test(NAME GameNetLogStats COMMAND GameNetTests --log-stats)
foreach(scenario room link mission2 mission4 mission8 mission8late mission8lossy mission8busy mission8rushed sidelink mission4modweapon mission4slowpeer
        playersync mission16 mission32)
    add_test(NAME GameNet_${scenario} COMMAND GameNetTests "${EDF6_GAME_DIR}" "$<TARGET_FILE:EDF6Coop>"
             "$<TARGET_FILE:GameMachine>" "${CMAKE_BINARY_DIR}/gamenettest" ${scenario})
    set_tests_properties(GameNet_${scenario} PROPERTIES SKIP_RETURN_CODE ${GAME_TEST_SKIPPED})
endforeach()

# The direct-link part's own tests (they need no game) and its join probe.
add_executable(DirectNetTests "${CMAKE_SOURCE_DIR}/../tests/test_main.cpp")
target_compile_definitions(DirectNetTests PRIVATE ${DIRECTNET_DEFINES})
target_compile_options(DirectNetTests PRIVATE /W4 /WX /utf-8 /EHsc)
target_link_libraries(DirectNetTests PRIVATE DirectNetCore)
add_test(NAME DirectNet COMMAND DirectNetTests)
add_executable(ExtensionTransportTests "${CMAKE_SOURCE_DIR}/../tests/extension_transport_test.cpp")
target_compile_definitions(ExtensionTransportTests PRIVATE ${DIRECTNET_DEFINES})
target_compile_options(ExtensionTransportTests PRIVATE /W4 /WX /utf-8 /EHsc)
target_link_libraries(ExtensionTransportTests PRIVATE DirectNetCore)
add_test(NAME ExtensionTransport COMMAND ExtensionTransportTests)
add_library(FakeExtensionProfile SHARED "${CMAKE_SOURCE_DIR}/../tests/fake_extension_profile.cpp")
set_target_properties(FakeExtensionProfile PROPERTIES OUTPUT_NAME EDF6VehicleCrew
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/extension-profile")
add_executable(ExtensionProfileTests "${CMAKE_SOURCE_DIR}/../tests/extension_profile_test.cpp")
target_compile_definitions(ExtensionProfileTests PRIVATE ${DIRECTNET_DEFINES})
target_compile_options(ExtensionProfileTests PRIVATE /W4 /WX /utf-8 /EHsc)
target_link_libraries(ExtensionProfileTests PRIVATE DirectNetPart)
add_dependencies(ExtensionProfileTests FakeEosSdk FakeExtensionProfile)
add_test(NAME ExtensionProfile COMMAND ExtensionProfileTests "$<TARGET_FILE:FakeEosSdk>" "$<TARGET_FILE:FakeExtensionProfile>")
add_executable(DirectNetProbeJoin "${CMAKE_SOURCE_DIR}/../tests/probe_join.cpp")
target_compile_definitions(DirectNetProbeJoin PRIVATE ${DIRECTNET_DEFINES})
target_compile_options(DirectNetProbeJoin PRIVATE /W4 /WX /utf-8 /EHsc)
target_link_libraries(DirectNetProbeJoin PRIVATE DirectNetCore)

# --- Netcode rewrite W1 (transport): datagram classes, fragments, congestion budgets, multipath direct link ---
target_sources(DirectNetCore PRIVATE "${DIRECTNET_DIR}/netclass.cpp" "${DIRECTNET_DIR}/fragment.cpp"
    "${DIRECTNET_DIR}/congestion.cpp")
add_executable(NetTransportTests "${CMAKE_SOURCE_DIR}/../tests/net_transport_test.cpp")
target_compile_definitions(NetTransportTests PRIVATE ${DIRECTNET_DEFINES})
target_compile_options(NetTransportTests PRIVATE /W4 /WX /utf-8 /EHsc)
target_link_libraries(NetTransportTests PRIVATE DirectNetCore)
add_test(NAME NetTransport COMMAND NetTransportTests)
target_sources(EDF6Coop PRIVATE src/nettraffic.cpp)
target_sources(EDF6Coop PRIVATE src/netfeature.cpp src/netcompress.cpp src/netaoi.cpp)
add_executable(NetFeatureTests tests/netfeature_test.cpp src/netfeature.cpp src/netcompress.cpp src/syncmarker.cpp src/packetfit.cpp
    src/identity.cpp src/log.cpp)
target_include_directories(NetFeatureTests PRIVATE "${CMAKE_SOURCE_DIR}/..")
target_compile_options(NetFeatureTests PRIVATE ${WARNINGS})
target_link_libraries(NetFeatureTests PRIVATE DirectNetPart)
add_test(NAME NetcodeVersionGate COMMAND NetFeatureTests)
foreach(scenario netstats versiongate bulk mesh meshblocked meshflap mission8xpress mission32bulk netstatsmixed meshoff joinfull mission8bulklost slotorder mission8bulkcut slotmigrate slotmigratenodirect slotmigrateearlyroom slotgiveup slotchurn slotchurnlate)
    add_test(NAME GameNet_${scenario} COMMAND GameNetTests "${EDF6_GAME_DIR}" "$<TARGET_FILE:EDF6Coop>"
             "$<TARGET_FILE:GameMachine>" "${CMAKE_BINARY_DIR}/gamenettest" ${scenario})
    set_tests_properties(GameNet_${scenario} PROPERTIES SKIP_RETURN_CODE ${GAME_TEST_SKIPPED})
endforeach()
add_executable(NetInterestTests tests/netaoi_test.cpp src/netaoi.cpp src/interest.cpp src/netfeature.cpp src/netcompress.cpp
    src/lobbystate.cpp src/hostmode.cpp src/roomview.cpp src/mission.cpp src/armor.cpp src/midhook.cpp src/code.cpp src/patches.cpp
    src/joinlog.cpp src/updatecheck.cpp src/widecmp.cpp src/syncmarker.cpp src/packetfit.cpp src/identity.cpp src/log.cpp)
target_include_directories(NetInterestTests PRIVATE "${CMAKE_SOURCE_DIR}/..")
target_compile_options(NetInterestTests PRIVATE ${WARNINGS})
target_link_libraries(NetInterestTests PRIVATE DirectNetPart winhttp)
add_test(NAME NetInterestOnTheWire COMMAND NetInterestTests)

add_executable(ModRoomIsolationTests ../tests/mod_room_test.cpp)
target_compile_options(ModRoomIsolationTests PRIVATE /W4 /WX /utf-8 /EHsc)
add_test(NAME ModRoomIsolation COMMAND ModRoomIsolationTests)
