param([switch]$Uninstall, [string]$GameDir = '')
# EDF6DirectNet installer: finds the EARTH DEFENSE FORCE 6 folder via Steam and copies the plugin in.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$gameFolder = 'EARTH DEFENSE FORCE 6'

function Find-SteamLibraries {
    $roots = @()
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty -Path $key -ErrorAction Stop
            foreach ($v in $p.SteamPath, $p.InstallPath) { if ($v) { $roots += ($v -replace '/', '\') } }
        } catch {}
    }
    $libs = @()
    foreach ($root in ($roots | Select-Object -Unique)) {
        $libs += $root
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            # The vdf is UTF-8 without BOM: PowerShell 5.1 would read it as ANSI and garble non-ASCII library paths.
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw -Encoding UTF8), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    return $libs | Select-Object -Unique
}

function Find-GameDir {
    if ($GameDir) { return $GameDir }
    # Running from inside the game folder (zip extracted there) also works.
    foreach ($d in @($here, (Split-Path -Parent $here))) {
        if ($d -and (Test-Path (Join-Path $d 'EDF6.exe'))) { return $d }
    }
    foreach ($lib in Find-SteamLibraries) {
        $d = Join-Path $lib "steamapps\common\$gameFolder"
        if (Test-Path (Join-Path $d 'EDF6.exe')) { return $d }
    }
    return $null
}

$game = Find-GameDir
if (-not $game) {
    Write-Host '没有自动找到 EDF6 的安装位置。' -ForegroundColor Yellow
    Write-Host '在 Steam 库里右键 EDF6 → 管理 → 浏览本地文件，把打开的文件夹路径粘贴到这里：'
    $game = (Read-Host '游戏文件夹').Trim('"', ' ')
}
# Also covers -GameDir and an empty answer (Join-Path rejects an empty path with a raw error).
if (-not $game -or -not (Test-Path -LiteralPath (Join-Path $game 'EDF6.exe'))) {
    Write-Host "这个文件夹里没有 EDF6.exe：$game" -ForegroundColor Red
    exit 1
}
Write-Host "游戏位置：$game"

$plugins = Join-Path $game 'Mods\Plugins'
$files = @(
    @{ From = 'Mods\Plugins\EDF6DirectNet.dll'; To = 'Mods\Plugins\EDF6DirectNet.dll' },
    @{ From = 'README_EDF6DirectNet.txt'; To = 'README_EDF6DirectNet.txt' },
    @{ From = 'EDF6DirectNet_AllowFirewall.bat'; To = 'EDF6DirectNet_AllowFirewall.bat' }
)

if (Get-Process EDF6 -ErrorAction SilentlyContinue) {
    Write-Host '请先退出游戏再运行。' -ForegroundColor Red
    exit 1
}

if ($Uninstall) {
    foreach ($f in $files) { Remove-Item -LiteralPath (Join-Path $game $f.To) -ErrorAction SilentlyContinue }
    foreach ($f in 'EDF6DirectNet.ini', 'EDF6DirectNet.log', 'EDF6DirectNet.log.1') {
        Remove-Item -LiteralPath (Join-Path $plugins $f) -ErrorAction SilentlyContinue
    }
    Write-Host '已卸载 EDF6DirectNet。游戏恢复原样（EDFModLoader 和其他 Mod 没有动）。' -ForegroundColor Green
    # Deleting a firewall rule needs administrator rights; reading it does not.
    if (Get-NetFirewallRule -DisplayName 'EDF6 DirectNet (UDP in)' -ErrorAction SilentlyContinue) {
        Write-Host '之前添加的防火墙放行规则还在。不需要的话，以管理员身份打开命令提示符运行：'
        Write-Host '  netsh advfirewall firewall delete rule name="EDF6 DirectNet (UDP in)"'
    }
    exit 0
}

function Copy-Into([string]$from, [string]$to) {
    $src = (Resolve-Path -LiteralPath $from).Path
    $dst = [System.IO.Path]::GetFullPath($to)
    if ($src -ieq $dst) { return }  # the zip was extracted straight into the game folder
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst -Force
}

foreach ($f in $files) { Copy-Into (Join-Path $here $f.From) (Join-Path $game $f.To) }
# Name used by 0.1.x; replaced by the ASCII name above.
Remove-Item -LiteralPath (Join-Path $game 'EDF6DirectNet_防火墙放行.bat') -ErrorAction SilentlyContinue
Write-Host '已安装 EDF6DirectNet。' -ForegroundColor Green

# EDFModLoader loads the plugin. Install the bundled official build only when none is present:
# an existing winmm.dll may be a newer or patched loader that other mods rely on.
if (-not (Test-Path (Join-Path $game 'winmm.dll'))) {
    Copy-Into (Join-Path $here 'EDFModLoader\winmm.dll') (Join-Path $game 'winmm.dll')
    if (-not (Test-Path (Join-Path $game 'ModLoader.ini'))) {
        Copy-Into (Join-Path $here 'EDFModLoader\ModLoader.ini') (Join-Path $game 'ModLoader.ini')
    }
    Write-Host '已安装 EDFModLoader（官方 v1.0.10，MIT 许可，见 EDFModLoader\LICENSE.txt）。' -ForegroundColor Green
} else {
    Write-Host '检测到已有 EDFModLoader（winmm.dll），保持不变。'
}
Write-Host ''
Write-Host '完成！直接从 Steam 启动游戏即可。'
Write-Host '第一次启动后会生成 Mods\Plugins\EDF6DirectNet.ini（设置）和 .log（日志），说明见 README_EDF6DirectNet.txt。'
