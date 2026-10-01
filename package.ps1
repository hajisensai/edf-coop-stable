# Builds the release package of one room size: release\EDF6Coop-<version>-<N>p\ and its .zip (+ .sha256).
#   package.ps1 [-Players 8|10|12|16|24|32]      (after build.ps1 -Players <N>)
# The layout mirrors the game folder: dropping the contents next to EDF6.exe is the whole install, and
# INSTALL.bat does the same for players who would rather not look for the folder (it finds it via Steam).
#
# No INI files are shipped, so an update never overwrites anyone's settings:
# - ModLoader.ini: EDFModLoader turns every option on when it is missing; a player who has one keeps it.
# - EDF6Coop.ini: the plugin writes it on first start, carrying over EDF6DirectNet.ini / EDF6MultiSlot.ini.
# winmm.dll is EDFModLoader with the shared-dispatch race fixed (packaging\LOADER_FIX_JA.md); bundling only
# the plugin DLL leaves the loader defect active.
# Reads only; writes nothing outside release\. Keep this script ASCII: PowerShell 5.1 reads it as ANSI.
param([ValidateSet(8, 10, 12, 16, 24, 32)][int]$Players = 8)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$multislot = Join-Path $root 'multislot'

# One version: project(EDF6Coop VERSION x.y.z) in multislot\CMakeLists.txt. The plugin, its update marker
# and its log banner all take it from there.
$cmake = Get-Content -LiteralPath (Join-Path $multislot 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(EDF6Coop VERSION (\d+\.\d+\.\d+)[\s)]') { throw 'project(EDF6Coop VERSION x.y.z) not found in multislot\CMakeLists.txt' }
$version = $Matches[1]
$notes = Join-Path $root "release-notes\$version.md"

$distName = if ($Players -eq 8) { 'dist' } else { "dist-${Players}p" }
$buildName = if ($Players -eq 8) { 'build' } else { "build-${Players}p" }
$dist = Join-Path $multislot $distName
$name = "EDF6Coop-$version-${Players}p"
$release = Join-Path $root 'release'
$out = Join-Path $release $name
$zip = Join-Path $release "$name.zip"

$sources = [ordered]@{
    'winmm.dll'                       = Join-Path $dist 'winmm.dll'
    'Mods\Plugins\EDF6Coop.dll'       = Join-Path $dist 'EDF6Coop.dll'
    'EDFModLoader_LICENSE.txt'        = Join-Path $multislot 'third_party\EDFModLoader\LICENSE.txt'
    'loader-fix.json'                 = Join-Path $multislot "$buildName\loader-fix.json"
    'LOADER_FIX_JA.md'                = Join-Path $multislot 'packaging\LOADER_FIX_JA.md'
    'HANDSHAKE_RECOVERY_JA.md'        = Join-Path $multislot 'packaging\HANDSHAKE_RECOVERY_JA.md'
    'INSTALL.bat'                     = Join-Path $root 'dist\INSTALL.bat'
    'UNINSTALL.bat'                   = Join-Path $root 'dist\UNINSTALL.bat'
    'install.ps1'                     = Join-Path $root 'dist\install.ps1'
    'EDF6Coop_AllowFirewall.bat'      = Join-Path $root 'dist\EDF6Coop_AllowFirewall.bat'
    'README_EDF6Coop.txt'             = Join-Path $root 'dist\README_EDF6Coop.txt'
    'README_EDF6Coop_zh.txt'          = Join-Path $root 'dist\README_EDF6Coop_zh.txt'
    'README_EDF6Coop_ja.txt'          = Join-Path $root 'dist\README_EDF6Coop_ja.txt'
    'RELEASE_NOTES_EDF6Coop.md'       = $notes
    'LICENSE.txt'                     = Join-Path $root 'LICENSE'
}
foreach ($source in $sources.Values) {
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing $source (run build.ps1 -Players $Players first?)" }
}

$loaderHash = (Get-FileHash -LiteralPath $sources['winmm.dll'] -Algorithm SHA256).Hash
if ($loaderHash -ne 'BE94E1FAC0CA12C41B6924E2EB168851641C999CE951D2A5A9FAEA5161B0F9A3') {
    throw "winmm.dll is not the verified race-fixed EDFModLoader ($loaderHash); build.ps1 makes it with tools\fix_winmm_proxy.py"
}
# The DLL must be this room size and this version: the auto-updater of every installed copy trusts its marker.
$pluginText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($sources['Mods\Plugins\EDF6Coop.dll']))
if ($pluginText.Contains('MULTISLOT CI PLACEHOLDER')) {
    throw 'EDF6Coop.dll is a CI build with a placeholder menu layout; build with assets\LYT_MAINFRAME.SGO (build.ps1 without -CI) to package'
}
$marker = "EDF6COOP_${Players}P_VERSION=$version"
if ($pluginText.IndexOf("$marker`0") -lt 0) { throw "EDF6Coop.dll does not carry $marker; rebuild with build.ps1 -Players $Players" }
# A DLL older than its sources would ship something else than the tag says.
$newestSource = Get-ChildItem (Join-Path $root 'src'), (Join-Path $multislot 'src') -File |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ((Get-Item -LiteralPath $sources['Mods\Plugins\EDF6Coop.dll']).LastWriteTime -lt $newestSource.LastWriteTime) {
    throw "EDF6Coop.dll is older than $($newestSource.FullName); run build.ps1 -Players $Players first"
}
foreach ($readme in 'README_EDF6Coop.txt', 'README_EDF6Coop_zh.txt', 'README_EDF6Coop_ja.txt') {
    $first = Get-Content -LiteralPath $sources[$readme] -TotalCount 1 -Encoding UTF8
    if ($first -notmatch "EDF6Coop $([regex]::Escape($version))\b") { throw "dist\$readme does not say EDF6Coop $version on its first line" }
}

# Every deletion target stays below release\.
$releaseFull = [IO.Path]::GetFullPath($release).TrimEnd('\') + '\'
foreach ($target in @($out, $zip, "$zip.sha256")) {
    if (-not [IO.Path]::GetFullPath($target).StartsWith($releaseFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing path outside release: $target"
    }
}
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
foreach ($old in $zip, "$zip.sha256") { if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old -Force } }
foreach ($entry in $sources.GetEnumerator()) {
    $target = Join-Path $out $entry.Key
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    [IO.File]::Copy($entry.Value, $target, $true)
}

# Not Compress-Archive: Windows PowerShell 5.1 stores '\' in entry names, which some unzip tools
# extract as flat file names instead of folders. The ZIP spec uses '/'.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $out -Recurse -File) {
        $entry = $file.FullName.Substring($out.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $archive.Dispose()
}
$zipHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
[IO.File]::WriteAllText("$zip.sha256", "$zipHash  $name.zip`n", (New-Object Text.ASCIIEncoding))
Get-ChildItem -LiteralPath $out -Recurse -File | ForEach-Object {
    $_.FullName.Substring($out.Length + 1) + "  " + $_.Length + "  " + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
}
Get-Item -LiteralPath $zip | Select-Object FullName, Length
"SHA256 $zipHash"
