cmake_minimum_required(VERSION 3.20)
project(EDF6MultiSlot VERSION 1.5.13 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Room size of this build (patches.h): 8 is the distributed build; 10 and 12 are families of their own whose
# rooms only the same build can join. Every target compiles with the same value, the tests included.
set(MULTISLOT_MAX_PLAYERS 8 CACHE STRING "Largest room this build supports (8, 10 or 12)")
set_property(CACHE MULTISLOT_MAX_PLAYERS PROPERTY STRINGS 8 10 12)
add_compile_definitions(MULTISLOT_MAX_PLAYERS=${MULTISLOT_MAX_PLAYERS})
# The version EDFModLoader is told (PluginInfo) and the one kVersion must start with (plugin.cpp) come from here.
add_compile_definitions(MULTISLOT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MULTISLOT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
                        MULTISLOT_VERSION_PATCH=${PROJECT_VERSION_PATCH})
if(MULTISLOT_MAX_PLAYERS EQUAL 8)
    set(MULTISLOT_DIST "${CMAKE_SOURCE_DIR}/dist")
else()
    set(MULTISLOT_DIST "${CMAKE_SOURCE_DIR}/dist-${MULTISLOT_MAX_PLAYERS}p")
endif()
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
# The INI is a template for the room size (@MULTISLOT_MAX_PLAYERS@, @MULTISLOT_MAX_GHOSTS@).
string(CONFIGURE "${MULTISLOT_DEFAULT_INI}" MULTISLOT_DEFAULT_INI @ONLY)
configure_file(src/default_ini.h.in "${CMAKE_BINARY_DIR}/generated/default_ini.h" @ONLY)
# The menu frame layout with the 8Player MOD label field (tools/make_menu_label.py); the plugin writes it
# to Mods/UI while it is active.
set(MENU_LAYOUT "${CMAKE_SOURCE_DIR}/assets/LYT_MAINFRAME.SGO")
# CI has no game: MULTISLOT_CI=ON builds the plugin and every test that does not need the game with a
# placeholder in place of the layout. The placeholder names itself (MULTISLOT CI PLACEHOLDER), the plugin never
# writes it into a game folder, and package.ps1 refuses a DLL that carries it, so such a build cannot ship.
option(MULTISLOT_CI "Build without assets/LYT_MAINFRAME.SGO (placeholder layout; the result cannot be packaged)" OFF)
set(MULTISLOT_PLACEHOLDER_LAYOUT OFF)
if(NOT EXISTS "${MENU_LAYOUT}")
    if(NOT MULTISLOT_CI)
        # Made from the game's own UI/LYT_MAINFRAME.SGO, so it is not in the repository.
        message(FATAL_ERROR "assets/LYT_MAINFRAME.SGO is missing: run `python -B tools/make_menu_label.py` with "
                            "EDF6_GAME_DIR set to the game folder first (it reads Root.cpk there)")
    endif()
    message(WARNING "MULTISLOT_CI: no assets/LYT_MAINFRAME.SGO, so the plugin embeds a placeholder layout. "
                    "This build is for tests only; package.ps1 refuses it")
    set(MULTISLOT_PLACEHOLDER_LAYOUT ON)
    # An SGO header, the marker package.ps1 looks for, and the label name in UTF-16, so the file handling tests
    # have something shaped like the real layout.
    string(HEX "SGO" SGO_HEX)
    string(HEX "MULTISLOT CI PLACEHOLDER - not the game's layout, never packaged" MARKER_HEX)
    set(MENU_LAYOUT_HEX "${SGO_HEX}00${MARKER_HEX}004d0053004c006100620065006c000000")
    add_compile_definitions(MULTISLOT_PLACEHOLDER_LAYOUT=1)
else()
    file(READ "${MENU_LAYOUT}" MENU_LAYOUT_HEX HEX)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MENU_LAYOUT}")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," MENU_LAYOUT_BYTES "${MENU_LAYOUT_HEX}")
set(BYTE "0x[0-9a-f][0-9a-f],")
string(REGEX REPLACE "(${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE}${BYTE})"
       "\\1\n" MENU_LAYOUT_BYTES "${MENU_LAYOUT_BYTES}")
configure_file(src/menu_layout.h.in "${CMAKE_BINARY_DIR}/generated/menu_layout.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/EDF6MultiSlot.ini")

add_library(EDF6MultiSlot SHARED
    src/plugin.cpp src/patches.cpp src/rooms.cpp src/roomview.cpp src/fakemembers.cpp src/hostmode.cpp src/menulayout.cpp
    src/mission.cpp src/spawn.cpp src/joinlog.cpp src/armor.cpp src/midhook.cpp src/code.cpp src/crashlog.cpp src/log.cpp
    src/netlog.cpp src/identity.cpp src/smoothing.cpp src/updatecheck.cpp)
target_link_libraries(EDF6MultiSlot PRIVATE winhttp)
target_include_directories(EDF6MultiSlot PRIVATE ${MODLOADER_INCLUDE} "${CMAKE_BINARY_DIR}/generated")
target_compile_options(EDF6MultiSlot PRIVATE ${WARNINGS})
target_link_options(EDF6MultiSlot PRIVATE /DYNAMICBASE /NXCOMPAT /INCREMENTAL:NO /PDBALTPATH:EDF6MultiSlot.pdb)
set_target_properties(EDF6MultiSlot PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MULTISLOT_DIST}")

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
add_executable(NetLogForwardTests tests/netlog_forward_test.cpp src/joinlog.cpp src/identity.cpp src/log.cpp)
target_compile_options(NetLogForwardTests PRIVATE ${WARNINGS})
add_test(NAME NetworkImportForwarding COMMAND NetLogForwardTests)
set(EDF_DLL "${EDF6_GAME_DIR}/EDF.dll")
# The tests against the game's code are registered either way. Without EDF.dll they report themselves skipped
# (exit code 77), so ctest names them in its summary instead of a configure warning nobody reads in CI.
set(GAME_TEST_SKIPPED 77)
if(NOT EXISTS "${EDF_DLL}")
    message(WARNING "No EDF.dll in EDF6_GAME_DIR (${EDF6_GAME_DIR}): the tests against the game's code "
                    "(PatchTablesMatchEDF, PluginLoad_*) will be reported as skipped")
endif()

add_executable(MultiSlotTests tests/tests.cpp src/patches.cpp src/rooms.cpp src/mission.cpp src/midhook.cpp src/code.cpp src/log.cpp src/smoothing.cpp)
target_compile_options(MultiSlotTests PRIVATE ${WARNINGS})
add_test(NAME PatchTablesMatchEDF COMMAND MultiSlotTests "${EDF_DLL}")
set_tests_properties(PatchTablesMatchEDF PROPERTIES SKIP_RETURN_CODE ${GAME_TEST_SKIPPED})

add_executable(RoomViewTests tests/roomview_test.cpp src/roomview.cpp src/armor.cpp src/mission.cpp src/midhook.cpp
    src/code.cpp src/patches.cpp src/log.cpp src/identity.cpp src/joinlog.cpp)
target_compile_options(RoomViewTests PRIVATE ${WARNINGS})
add_test(NAME RoomViewPaging COMMAND RoomViewTests)

add_executable(MidHookTests tests/midhook_test.cpp src/midhook.cpp src/code.cpp)
target_compile_options(MidHookTests PRIVATE ${WARNINGS})
add_test(NAME MidFunctionHooks COMMAND MidHookTests)

add_executable(MissionTests tests/mission_test.cpp src/mission.cpp src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp)
target_compile_options(MissionTests PRIVATE ${WARNINGS})
add_test(NAME MissionSlots COMMAND MissionTests)

add_executable(HostModeTests tests/hostmode_test.cpp src/hostmode.cpp src/roomview.cpp src/mission.cpp src/armor.cpp
    src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp src/identity.cpp src/joinlog.cpp src/updatecheck.cpp)
target_link_libraries(HostModeTests PRIVATE winhttp)
target_compile_options(HostModeTests PRIVATE ${WARNINGS})
add_test(NAME HostModeAndMenuLabel COMMAND HostModeTests)

add_executable(MenuLayoutTests tests/menulayout_test.cpp src/menulayout.cpp)
target_include_directories(MenuLayoutTests PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_compile_options(MenuLayoutTests PRIVATE ${WARNINGS})
add_test(NAME MenuLayoutFile COMMAND MenuLayoutTests "${CMAKE_BINARY_DIR}/layouttest")

add_executable(SpawnTests tests/spawn_test.cpp src/spawn.cpp src/mission.cpp src/midhook.cpp src/code.cpp src/patches.cpp src/log.cpp)
target_compile_options(SpawnTests PRIVATE ${WARNINGS})
add_test(NAME EnemySpawnCounts COMMAND SpawnTests)

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
add_dependencies(RefusalTests EDF6MultiSlot)
add_test(NAME PluginRefusalUnloads COMMAND RefusalTests "$<TARGET_FILE:EDF6MultiSlot>" "${CMAKE_BINARY_DIR}/refusaltest")

add_executable(MultiSlotLoadTest tests/load_test.cpp src/patches.cpp src/midhook.cpp src/code.cpp)
target_include_directories(MultiSlotLoadTest PRIVATE ${MODLOADER_INCLUDE} "${CMAKE_BINARY_DIR}/generated")
target_compile_options(MultiSlotLoadTest PRIVATE ${WARNINGS})
add_dependencies(MultiSlotLoadTest EDF6MultiSlot)
foreach(mode off host4 host8 fresh nomission recoveryoff quiet smooth)
    add_test(NAME PluginLoad_${mode} COMMAND MultiSlotLoadTest "${EDF_DLL}" "$<TARGET_FILE:EDF6MultiSlot>"
             "${CMAKE_BINARY_DIR}/loadtest" ${mode})
    set_tests_properties(PluginLoad_${mode} PROPERTIES SKIP_RETURN_CODE ${GAME_TEST_SKIPPED})
endforeach()

configure_file(EDF6MultiSlot.ini "${MULTISLOT_DIST}/EDF6MultiSlot.ini" @ONLY)
