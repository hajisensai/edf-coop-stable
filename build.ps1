# Builds build\EDF6DirectNet.dll and build\edf6_directnet_tests.exe with MSVC (x64, static CRT).
param([switch]$Test)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC x64 toolset not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars calls vswhere itself
New-Item -ItemType Directory -Force (Join-Path $root 'build') | Out-Null

$common = '/nologo /std:c++20 /O2 /MT /EHsc /W4 /WX /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS'
$core = 'src\log.cpp src\auth.cpp src\wire.cpp src\reliable.cpp src\direct_net.cpp src\netif.cpp src\config.cpp src\iat.cpp src\hold.cpp'
$libs = 'ws2_32.lib iphlpapi.lib bcrypt.lib ole32.lib oleaut32.lib'

$steps = @(
    "call `"$vcvars`" >nul",
    "cd /d `"$root`"",
    "cl $common /LD $core src\upnp.cpp src\lobby_marker.cpp src\eos_hooks.cpp src\plugin.cpp /Fobuild\ /Febuild\EDF6DirectNet.dll /link $libs",
    "cl $common $core tests\test_main.cpp /Fobuild\ /Febuild\edf6_directnet_tests.exe /link $libs",
    "cl $common $core tests\probe_join.cpp /Fobuild\ /Febuild\probe_join.exe /link $libs"
)
cmd /c ($steps -join ' && ')
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
if ($Test) {
    & (Join-Path $root 'build\edf6_directnet_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
}
