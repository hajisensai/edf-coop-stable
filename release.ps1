# Builds, tests and packages every room size of the tagged version and stages the release files:
#   release.ps1 [-Players 8,10,...] [-Upload]
# Release packages need the game's menu asset (multislot\assets\LYT_MAINFRAME.SGO, made from Root.cpk), which
# cannot be on a build server, so this runs on a machine with the game (EDF6_GAME_DIR). It never signs: the
# signing key stays in the repository secret, and .github/workflows/release.yml signs and publishes.
#
# Without -Upload it stops after staging release\upload-<version>\. With -Upload it creates a DRAFT Release
# for the tag with those files (gh, tag already pushed); players' updaters never see a draft. Then run
#   gh workflow run release.yml -f tag=v<version>
# which checks the draft, signs each EDF6Coop-<N>p.dll, uploads the .sig files and publishes it as latest.
# Keep this script ASCII: PowerShell 5.1 reads it as ANSI.
param(
    [ValidateSet(8, 10, 12, 16, 24, 32)] [int[]]$Players = @(8, 10, 12, 16, 24, 32),
    [switch]$Upload
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$multislot = Join-Path $root 'multislot'

$cmake = Get-Content -LiteralPath (Join-Path $multislot 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(EDF6Coop VERSION (\d+\.\d+\.\d+)[\s)]') { throw 'project(EDF6Coop VERSION x.y.z) not found in multislot\CMakeLists.txt' }
$version = $Matches[1]
$tag = "v$version"
$notes = Join-Path $root "release-notes\$version.md"
if (-not (Test-Path -LiteralPath $notes)) { throw "missing release-notes\$version.md: write the release text first" }

# What is built must be exactly the tagged commit.
$dirty = git -C $root status --porcelain --untracked-files=no
if ($LASTEXITCODE) { throw 'git status failed' }
if ($dirty) { throw "uncommitted changes; release only a committed tree:`n$($dirty -join "`n")" }
$head = git -C $root rev-parse HEAD
$tagged = git -C $root rev-parse --verify --quiet "$tag^{commit}"
if (-not $tagged) { throw "tag $tag does not exist; tag the release commit first" }
if ($tagged -ne $head) { throw "HEAD $head is not the commit of $tag ($tagged)" }

$stage = Join-Path $root "release\upload-$version"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

foreach ($n in $Players) {
    & (Join-Path $root 'build.ps1') -Players $n -Test
    & (Join-Path $root 'package.ps1') -Players $n | Out-Null
    $distName = if ($n -eq 8) { 'dist' } else { "dist-${n}p" }
    # The updater downloads EDF6Coop-<N>p.dll of its own room size; the zip carries the same file.
    Copy-Item -LiteralPath (Join-Path $multislot "$distName\EDF6Coop.dll") -Destination (Join-Path $stage "EDF6Coop-${n}p.dll")
    $zip = Join-Path $root "release\EDF6Coop-$version-${n}p.zip"
    Copy-Item -LiteralPath $zip, "$zip.sha256" -Destination $stage
}

$files = @(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name)
$files | ForEach-Object { '{0,-36} {1,10}  {2}' -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
if (-not $Upload) {
    "Staged in $stage. Re-run with -Upload to create the draft Release $tag."
    return
}

gh release create $tag @($files.FullName) --draft --verify-tag --title "EDF6Coop $version" --notes-file $notes
if ($LASTEXITCODE) { throw 'gh release create failed' }
"Draft Release $tag created. Sign and publish it with: gh workflow run release.yml -f tag=$tag"
