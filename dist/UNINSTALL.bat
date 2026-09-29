@echo off
rem Removes EDF6DirectNet from EARTH DEFENSE FORCE 6. Run as administrator to also remove the firewall rule.
if not exist "%~dp0install.ps1" (
  echo install.ps1 not found. Run UNINSTALL.bat from the extracted zip folder.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
set "RC=%errorlevel%"
pause
exit /b %RC%
