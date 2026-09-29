# Puts the official EDFModLoader v1.0.10 winmm.dll (BlueAmulet, MIT) at third_party\EDFModLoader\winmm.dll.
# The build turns it into the race-fixed loader (tools/fix_winmm_proxy.py) that the package ships; the
# original is not kept in the repository. Both the release zip and the DLL in it are checked by SHA-256
# (the zip hash is the one the DirectNet release pipeline pins).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$target = Join-Path $root 'third_party\EDFModLoader\winmm.dll'
$zipUrl = 'https://github.com/BlueAmulet/EDFModLoader/releases/download/v1.0.10/EDFModLoader.zip'
$zipSha256 = '850BE7567954EE319DC2B65E9174D1EEC70791BA9A43D2508A40B22B98BB4B08'
$dllSha256 = 'B80E4DA6AE7264F0E9774C992DE3C5F9B6E9BC9422146ADEEB5BB2BD681E112E'

if ((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $dllSha256) {
    Write-Host 'EDFModLoader winmm.dll already in place'
    exit 0
}
$temp = Join-Path ([IO.Path]::GetTempPath()) ('EDFModLoader-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    $zip = Join-Path $temp 'EDFModLoader.zip'
    $request = @{ Uri = $zipUrl; OutFile = $zip; UseBasicParsing = $true }
    if ($env:HTTPS_PROXY) { $request.Proxy = $env:HTTPS_PROXY }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest @request
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    if ($hash -ne $zipSha256) { throw "EDFModLoader.zip hash $hash does not match the pinned one" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entry = $archive.Entries | Where-Object { $_.FullName -eq 'winmm.dll' } | Select-Object -First 1
        if (-not $entry) { throw 'EDFModLoader.zip has no winmm.dll' }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    } finally {
        $archive.Dispose()
    }
    $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    if ($hash -ne $dllSha256) {
        Remove-Item -LiteralPath $target -Force
        throw "winmm.dll hash $hash does not match the pinned one"
    }
    Write-Host "EDFModLoader v1.0.10 winmm.dll written to $target"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
