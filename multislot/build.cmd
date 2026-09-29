@echo off
setlocal
rem build.cmd [players]  - 8 (default) builds dist\, 10 or 12 build dist-10p\ or dist-12p\ (patches.h).
rem Set EDF6_GAME_DIR to the game folder (holds EDF.dll and Root.cpk; only read) for the menu asset and
rem the tests against the game's code.
set "PLAYERS=%~1"
if "%PLAYERS%"=="" set "PLAYERS=8"
set "BUILD_DIR=%~dp0build"
if not "%PLAYERS%"=="8" set "BUILD_DIR=%~dp0build-%PLAYERS%p"
if not exist "%~dp0third_party\EDFModLoader\winmm.dll" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\fetch_modloader.ps1"
    if errorlevel 1 exit /b 1
)
if not exist "%~dp0assets\LYT_MAINFRAME.SGO" (
    python -B "%~dp0tools\make_menu_label.py"
    if errorlevel 1 exit /b 1
)
call "%~dp0tools\msvc-x64-env.cmd"
if errorlevel 1 exit /b 1
set VSLANG=1033
cmake -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMULTISLOT_MAX_PLAYERS=%PLAYERS%
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1
ctest --test-dir "%BUILD_DIR%" --output-on-failure
exit /b %errorlevel%
