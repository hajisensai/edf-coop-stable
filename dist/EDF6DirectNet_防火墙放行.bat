@echo off
rem Allow inbound UDP for EDF6 (needed when hosting with EDF6DirectNet). Run as administrator.
net session >nul 2>&1
if errorlevel 1 (
  echo Please right-click this file and choose "Run as administrator".
  pause
  exit /b 1
)
set "EXE=%~dp0EDF6.exe"
if not exist "%EXE%" (
  echo EDF6.exe not found next to this file. Put this .bat in the game folder.
  pause
  exit /b 1
)
netsh advfirewall firewall delete rule name="EDF6 DirectNet (UDP in)" >nul 2>&1
netsh advfirewall firewall add rule name="EDF6 DirectNet (UDP in)" dir=in action=allow program="%EXE%" protocol=UDP enable=yes profile=any
if errorlevel 1 ( echo Failed to add the firewall rule. & pause & exit /b 1 )
echo Done: inbound UDP is now allowed for "%EXE%".
pause
