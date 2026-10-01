# Builds EDF6Coop.dll (the direct link and the room part in one plugin) with CMake + Ninja + MSVC x64.
#   build.ps1 [-Players 8|10|12|16|24|32] [-Test] [-CI]
# The 8-player build lands in multislot\dist\, the others in multislot\dist-<N>p\ (each with the race-fixed
# EDFModLoader winmm.dll next to it). Rooms of one size are a family of their own: only the same build joins.
# Set EDF6_GAME_DIR to the game folder (holds EDF.dll and Root.cpk; only read) for the menu asset and the
# tests against the game's code. -CI builds without the game's menu asset (placeholder; cannot be packaged).
param(
    [ValidateSet(8, 10, 12, 16, 24, 32)] [int]$Players = 8,
    [switch]$Test,
    [switch]$CI
)
$ErrorActionPreference = 'Stop'
$multislot = Join-Path $PSScriptRoot 'multislot'
$buildDir = if ($Players -eq 8) { Join-Path $multislot 'build' } else { Join-Path $multislot "build-${Players}p" }

if (-not (Test-Path -LiteralPath (Join-Path $multislot 'third_party\EDFModLoader\winmm.dll'))) {
    & (Join-Path $multislot 'tools\fetch_modloader.ps1')
    if ($LASTEXITCODE) { throw "fetch_modloader.ps1 failed ($LASTEXITCODE)" }
}
if (-not $CI -and -not (Test-Path -LiteralPath (Join-Path $multislot 'assets\LYT_MAINFRAME.SGO'))) {
    python -B (Join-Path $multislot 'tools\make_menu_label.py')
    if ($LASTEXITCODE) { throw "make_menu_label.py failed ($LASTEXITCODE); set EDF6_GAME_DIR to the game folder" }
}

$configure = "cmake -S `"$multislot`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMULTISLOT_MAX_PLAYERS=$Players"
if ($CI) { $configure += ' -DMULTISLOT_CI=ON' }
if ($env:LOADER_PYTHON) { $configure += " `"-DPython3_EXECUTABLE=$env:LOADER_PYTHON`"" }
$steps = @(
    "call `"$(Join-Path $multislot 'tools\msvc-x64-env.cmd')`" >nul",
    'set VSLANG=1033',
    $configure,
    "cmake --build `"$buildDir`""
)
if ($Test) { $steps += "ctest --test-dir `"$buildDir`" --output-on-failure --no-tests=error" }
cmd /c ($steps -join ' && ')
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
