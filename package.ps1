# Builds the release zip build\EDF6DirectNet-v<version>.zip from build\EDF6DirectNet.dll and dist\.
# EDFModLoader (official, MIT) is bundled from -ModLoaderDir: winmm.dll, ModLoader.ini, LICENSE.txt.
param([Parameter(Mandatory)] [string]$Version, [Parameter(Mandatory)] [string]$ModLoaderDir)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = $PSScriptRoot
$zipPath = Join-Path $root "build\EDF6DirectNet-v$Version.zip"
$entries = [ordered]@{
    'INSTALL.bat'                       = 'dist\INSTALL.bat'
    'UNINSTALL.bat'                     = 'dist\UNINSTALL.bat'
    'install.ps1'                       = 'dist\install.ps1'
    'README_EDF6DirectNet.txt'          = 'dist\README_EDF6DirectNet.txt'
    'README_EDF6DirectNet_en.txt'       = 'dist\README_EDF6DirectNet_en.txt'
    'README_EDF6DirectNet_ja.txt'       = 'dist\README_EDF6DirectNet_ja.txt'
    'EDF6DirectNet_AllowFirewall.bat'   = 'dist\EDF6DirectNet_AllowFirewall.bat'
    'Mods/Plugins/EDF6DirectNet.dll'    = 'build\EDF6DirectNet.dll'
    'EDFModLoader/winmm.dll'            = (Join-Path $ModLoaderDir 'winmm.dll')
    'EDFModLoader/ModLoader.ini'        = (Join-Path $ModLoaderDir 'ModLoader.ini')
    'EDFModLoader/LICENSE.txt'          = (Join-Path $ModLoaderDir 'LICENSE.txt')
    'LICENSE.txt'                       = 'LICENSE'
}
$sources = [ordered]@{}
foreach ($name in $entries.Keys) {
    $src = $entries[$name]
    if (-not [System.IO.Path]::IsPathRooted($src)) { $src = Join-Path $root $src }
    if (-not (Test-Path -LiteralPath $src)) { throw "missing input file $src" }
    $sources[$name] = $src
}
# The version is written in three places; a mismatch ships a zip whose log and readme lie about it.
$pluginSource = Get-Content (Join-Path $root 'src\plugin.cpp') -Raw
$pluginVersion = [regex]::Match($pluginSource, 'kVersionText = "([^"]+)"').Groups[1].Value
# The numeric version EDFModLoader shows must say the same.
$numbers = [regex]::Match($pluginSource, 'kVersionMajor = (\d+), kVersionMinor = (\d+), kVersionPatch = (\d+)')
$numericVersion = '{0}.{1}.{2}' -f $numbers.Groups[1].Value, $numbers.Groups[2].Value, $numbers.Groups[3].Value
if ($numericVersion -ne $pluginVersion) { throw "src\plugin.cpp: kVersionText $pluginVersion but kVersionMajor/Minor/Patch $numericVersion" }
if ($pluginVersion -ne $Version) { throw "version mismatch: -Version $Version, src\plugin.cpp $pluginVersion" }
foreach ($readme in 'README_EDF6DirectNet.txt', 'README_EDF6DirectNet_en.txt', 'README_EDF6DirectNet_ja.txt') {
    $readmeVersion = [regex]::Match((Get-Content (Join-Path $root "dist\$readme") -TotalCount 1 -Encoding UTF8), 'EDF6DirectNet (\S+)').Groups[1].Value
    if ($readmeVersion -ne $Version) { throw "version mismatch: -Version $Version, dist\$readme $readmeVersion" }
}
$newestSource = Get-ChildItem (Join-Path $root 'src') -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ((Get-Item $sources['Mods/Plugins/EDF6DirectNet.dll']).LastWriteTime -lt $newestSource.LastWriteTime) {
    throw "build\EDF6DirectNet.dll is older than src\$($newestSource.Name); run build.ps1 first"
}
if (Test-Path $zipPath) { Remove-Item $zipPath }
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, 'Create')
try {
    foreach ($name in $sources.Keys) {
        # Forward slashes: backslash entry names break extraction in many tools.
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $sources[$name], $name, 'Optimal') | Out-Null
    }
} finally { $zip.Dispose() }
Get-Item $zipPath | Select-Object Name, Length
