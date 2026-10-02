@echo off
rem build.cmd  - builds and tests EDF6Coop.dll into dist\ (one build for every room size).
rem The build itself is ..\build.ps1; set EDF6_GAME_DIR to the game folder (holds EDF.dll and Root.cpk; only
rem read) for the menu asset and the tests against the game's code.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\build.ps1" -Test
exit /b %errorlevel%
