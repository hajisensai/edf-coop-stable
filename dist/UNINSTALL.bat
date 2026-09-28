@echo off
rem Removes EDF6DirectNet from EARTH DEFENSE FORCE 6.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
pause
