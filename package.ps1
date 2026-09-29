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
$pluginVersion = [regex]::Match((Get-Content (Join-Path $root 'src\plugin.cpp') -Raw), 'kVersionText = "([^"]+)"').Groups[1].Value
$readmeVersion = [regex]::Match((Get-Content (Join-Path $root 'dist\README_EDF6DirectNet.txt') -TotalCount 1 -Encoding UTF8), 'EDF6DirectNet (\S+)').Groups[1].Value
if ($pluginVersion -ne $Version -or $readmeVersion -ne $Version) {
    throw "version mismatch: -Version $Version, src\plugin.cpp $pluginVersion, dist\README_EDF6DirectNet.txt $readmeVersion"
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
