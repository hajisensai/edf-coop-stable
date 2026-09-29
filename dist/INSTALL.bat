@echo off
rem Installs EDF6DirectNet into EARTH DEFENSE FORCE 6 (found automatically via Steam).
rem Every path is quoted; %~dp0 is expanded before parsing, so spaces, & parentheses, brackets and non-ASCII names in the folder are safe.
if not exist "%~dp0install.ps1" (
  echo install.ps1 not found. Extract the whole zip first, then run INSTALL.bat from the extracted folder.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
set "RC=%errorlevel%"
pause
exit /b %RC%
