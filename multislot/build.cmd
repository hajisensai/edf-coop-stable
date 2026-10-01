@echo off
rem build.cmd [players]  - builds and tests EDF6Coop.dll: 8 (default) into dist\, 10/12/16/24/32 into dist-<N>p\.
rem The build itself is ..\build.ps1; set EDF6_GAME_DIR to the game folder (holds EDF.dll and Root.cpk; only
rem read) for the menu asset and the tests against the game's code.
set "PLAYERS=%~1"
if "%PLAYERS%"=="" set "PLAYERS=8"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\build.ps1" -Players %PLAYERS% -Test
exit /b %errorlevel%
