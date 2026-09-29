@echo off
setlocal EnableExtensions
rem Allow inbound UDP for EDF6 (needed when hosting with EDF6DirectNet). Run as administrator.
rem The rule is limited to EDF6.exe AND the UDP port the plugin listens on (ListenPort in
rem Mods\Plugins\EDF6DirectNet.ini, default 27015), not to every UDP port of the game.
rem profile=any is kept on purpose: Windows often labels a home connection "Public" (new networks, some
rem routers, hotspots), and a host on such a network would be silently unreachable. The exposure is one
rem UDP port of one program. Remove the rule with UNINSTALL.bat (as administrator) or
rem   netsh advfirewall firewall delete rule name="EDF6 DirectNet (UDP in)"
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

set "PORT=27015"
set "RAW="
set "INI=%~dp0Mods\Plugins\EDF6DirectNet.ini"
rem No parenthesised blocks and no "||" after a pipe below: %RAW% must be re-read after each set.
if not exist "%INI%" goto :port_done
for /f "usebackq tokens=1,* delims==" %%A in (`findstr /b /i /r /c:"[ ]*ListenPort[ ]*=" "%INI%"`) do set "RAW=%%B"
if not defined RAW goto :port_done
rem Keep the first word after "=" (drops spaces) and strip leading zeros, then require 1..65535.
for /f "tokens=1" %%V in ("%RAW%") do set "RAW=%%V"
set "RAW2="
for /f "tokens=* delims=0" %%Z in ("%RAW%") do set "RAW2=%%Z"
set "RAW=%RAW2%"
set "RAW2="
if not defined RAW goto :port_bad
set RAW | findstr /r /x "RAW=[0-9][0-9]*" >nul
if errorlevel 1 goto :port_bad
if not "%RAW:~5,1%"=="" goto :port_bad
if %RAW% GTR 65535 goto :port_bad
set "PORT=%RAW%"
goto :port_done
:port_bad
echo ListenPort in EDF6DirectNet.ini is not a port number from 1 to 65535, so 27015 is used.
:port_done

netsh advfirewall firewall delete rule name="EDF6 DirectNet (UDP in)" >nul 2>&1
netsh advfirewall firewall add rule name="EDF6 DirectNet (UDP in)" dir=in action=allow program="%EXE%" protocol=UDP localport=%PORT% enable=yes profile=any
if errorlevel 1 ( echo Failed to add the firewall rule. & pause & exit /b 1 )
echo Done: inbound UDP port %PORT% is now allowed for "%EXE%".
echo If you change ListenPort in the ini later, run this file again.
pause
