# Builds, tests and packages the tagged version and stages the release files:
#   release.ps1 [-Upload]
# Release packages need the game's menu asset (multislot\assets\LYT_MAINFRAME.SGO, made from Root.cpk), which
# cannot be on a build server, so this runs on a machine with the game (EDF6_GAME_DIR). It never signs: the
# signing key stays in the repository secret, and .github/workflows/release.yml signs and publishes.
#
# Without -Upload it stops after staging release\upload-<version>\. With -Upload it creates a DRAFT Release
# for the tag with those files (gh, tag already pushed); players' updaters never see a draft. Then run
#   gh workflow run release.yml --repo <owner>/<repo> -f tag=v<version>
# which checks the draft, signs EDF6Coop.dll, uploads its .sig and publishes it as latest.
# One DLL for every room size, staged as EDF6Coop.dll (what every updater since 2.3.0 downloads). 2.2's room-size
# builds looked for EDF6Coop-<N>p.dll; those copies are no longer published (they only confused players).
# Keep this script ASCII: PowerShell 5.1 reads it as ANSI.
param([switch]$Upload)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$multislot = Join-Path $root 'multislot'

$cmake = Get-Content -LiteralPath (Join-Path $multislot 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(EDF6Coop VERSION (\d+\.\d+\.\d+)[\s)]') { throw 'project(EDF6Coop VERSION x.y.z) not found in multislot\CMakeLists.txt' }
$version = $Matches[1]
$tag = "v$version"
$notes = Join-Path $root "release-notes\$version.md"
if (-not (Test-Path -LiteralPath $notes)) { throw "missing release-notes\$version.md: write the release text first" }

# gh picks its repository from the current directory, not from $root: name it explicitly, from $root's origin.
$origin = git -C $root remote get-url origin
if ($LASTEXITCODE) { throw 'git remote get-url origin failed' }
if ($origin -notmatch 'github\.com[:/]([^/]+/[^/]+?)(\.git)?/?$') { throw "origin is not a GitHub repository: $origin" }
$repo = $Matches[1]

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

& (Join-Path $root 'build.ps1') -Test
& (Join-Path $root 'package.ps1') | Out-Null
$dll = Join-Path $multislot 'dist\EDF6Coop.dll'
# The zip carries the same file.
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'EDF6Coop.dll')
$zip = Join-Path $root "release\EDF6Coop-$version.zip"
Copy-Item -LiteralPath $zip, "$zip.sha256" -Destination $stage

$files = @(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name)
$files | ForEach-Object { '{0,-36} {1,10}  {2}' -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
if (-not $Upload) {
    "Staged in $stage. Re-run with -Upload to create the draft Release $tag."
    return
}

gh release create $tag @($files.FullName) --repo $repo --draft --verify-tag --title "EDF6Coop $version" --notes-file $notes
if ($LASTEXITCODE) { throw 'gh release create failed' }
"Draft Release $tag created in $repo. Sign and publish it with: gh workflow run release.yml --repo $repo -f tag=$tag"
