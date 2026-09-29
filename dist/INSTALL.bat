@echo off
rem Installs EDF6DirectNet into EARTH DEFENSE FORCE 6 (found automatically via Steam).
if not exist "%~dp0install.ps1" (
  echo install.ps1 not found. Extract the whole zip first, then run INSTALL.bat from the extracted folder.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
pause
