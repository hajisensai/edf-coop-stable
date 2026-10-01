# Signs the auto-update manifest of a release and checks the result (format: src/updater.h).
# Writes <Dll>.sig: "EDF6Coop <Version>\n<sha256 of Dll>\n<ECDSA P-256 signature, r||s in hex>\n".
# The private key is the base64 CNG ECCPRIVATE blob in $env:EDF6DN_UPDATE_SIGNING_KEY (repository secret);
# it must belong to the public key compiled into the updater (read from -Updater, src/updater.cpp), and
# the signature is verified with that public key before this returns. Works in PowerShell 5.1 and 7.
param(
    [Parameter(Mandatory)] [string]$Dll,
    [Parameter(Mandatory)] [string]$Version,
    # The room size the DLL was built for: its marker says EDF6COOP_<n>P_VERSION=.
    [ValidateSet(8, 10, 12, 16, 24, 32)] [int]$Players = 8,
    [string]$Updater = (Join-Path $PSScriptRoot 'src\updater.cpp'),
    # Test hook only: base64 CNG ECCPUBLIC blob used instead of the kReleaseKey in $Updater. The release
    # workflow never passes it, so releases are always checked against the key clients have compiled in.
    [string]$TestPublicKeyBase64
)
$ErrorActionPreference = 'Stop'
$crypto = 'System.Security.Cryptography'
if (-not $env:EDF6DN_UPDATE_SIGNING_KEY) { throw 'EDF6DN_UPDATE_SIGNING_KEY is not set; a release is never published unsigned' }
if ($Version -notmatch '^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$') { throw "not a plain x.y.z version: $Version" }

if ($TestPublicKeyBase64) {
    $public = [Convert]::FromBase64String($TestPublicKeyBase64)
} else {
    $source = Get-Content -LiteralPath $Updater -Raw
    $match = [regex]::Match($source, 'kReleaseKey\[72\] = \{([^}]*)\}')
    if (-not $match.Success) { throw "no kReleaseKey in $Updater" }
    $public = [byte[]]@($match.Groups[1].Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } |
        ForEach-Object { [Convert]::ToByte($_, 16) })
}
if ($public.Length -ne 72) { throw "public key has $($public.Length) bytes, not 72" }

$dllBytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Dll).Path)
$marker = "EDF6COOP_${Players}P_VERSION=$Version"
if ([Text.Encoding]::ASCII.GetString($dllBytes).IndexOf("$marker`0") -lt 0) {
    throw "$Dll does not carry the version marker $marker (is it the $Players-player build of ${Version}?)"
}
$hash = (Get-FileHash -LiteralPath $Dll -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = [Text.Encoding]::ASCII.GetBytes("EDF6Coop $Version`n$hash`n")

$key = [Security.Cryptography.CngKey]::Import([Convert]::FromBase64String($env:EDF6DN_UPDATE_SIGNING_KEY.Trim()),
    [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
try {
    $derived = $key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    if ([Convert]::ToBase64String($derived) -ne [Convert]::ToBase64String($public)) {
        throw "the signing key does not belong to the public key in $Updater (clients would reject the release)"
    }
    $signer = New-Object "$crypto.ECDsaCng" -ArgumentList (, $key)
    $signature = $signer.SignData($manifest, [Security.Cryptography.HashAlgorithmName]::SHA256)
    $signer.Dispose()
} finally {
    $key.Dispose()
}
if ($signature.Length -ne 64) { throw "unexpected signature length $($signature.Length)" }
$hex = -join ($signature | ForEach-Object { $_.ToString('x2') })
$sigPath = "$((Resolve-Path -LiteralPath $Dll).Path).sig"
[IO.File]::WriteAllBytes($sigPath, [Text.Encoding]::ASCII.GetBytes("EDF6Coop $Version`n$hash`n$hex`n"))

# Check what was written the way a client sees it: only the public key, the file as bytes.
$written = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($sigPath))
$lines = $written.Split("`n")
if ($lines.Count -ne 4 -or $lines[3] -ne '' -or $lines[0] -ne "EDF6Coop $Version" -or $lines[1] -ne $hash -or
    $lines[2] -notmatch '^[0-9a-f]{128}$') { throw "$sigPath is malformed" }
$check = [byte[]]@(0..63 | ForEach-Object { [Convert]::ToByte($lines[2].Substring(2 * $_, 2), 16) })
$publicKey = [Security.Cryptography.CngKey]::Import($public, [Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
try {
    $verifier = New-Object "$crypto.ECDsaCng" -ArgumentList (, $publicKey)
    $valid = $verifier.VerifyData([Text.Encoding]::ASCII.GetBytes("$($lines[0])`n$($lines[1])`n"), $check,
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    $verifier.Dispose()
} finally {
    $publicKey.Dispose()
}
if (-not $valid) { throw "$sigPath does not verify with the public key in $Updater" }
Write-Host "signed $Dll ($Version, sha256 $hash) -> $sigPath; verified with the updater's public key"
