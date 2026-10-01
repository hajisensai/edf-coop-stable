param([switch]$Uninstall, [string]$GameDir = '')
# EDF6Coop installer: finds the EARTH DEFENSE FORCE 6 folder via Steam and copies the plugin in.
# EDF6Coop replaces EDF6DirectNet and EDF6MultiSlot: their DLLs are renamed to .disabled (never deleted), and
# their settings files stay where they are; the plugin carries them into EDF6Coop.ini on its first start.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$gameFolder = 'EARTH DEFENSE FORCE 6'
# Messages follow the Windows display language: Chinese, Japanese, otherwise English.
$lang = switch ((Get-UICulture).TwoLetterISOLanguageName) { 'zh' { 0 } 'ja' { 1 } default { 2 } }
function T([string]$zh, [string]$ja, [string]$en) { return @($zh, $ja, $en)[$lang] }
$readme = @('README_EDF6Coop_zh.txt', 'README_EDF6Coop_ja.txt', 'README_EDF6Coop.txt')[$lang]

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
        if (Test-Path -LiteralPath $vdf) {
            # The vdf is UTF-8 without BOM: PowerShell 5.1 would read it as ANSI and garble non-ASCII library paths.
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw -Encoding UTF8), '"path"\s+"([^"]+)"')) {
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
        if ($d -and (Test-Path -LiteralPath (Join-Path $d 'EDF6.exe'))) { return $d }
    }
    foreach ($lib in Find-SteamLibraries) {
        $d = Join-Path $lib "steamapps\common\$gameFolder"
        if (Test-Path -LiteralPath (Join-Path $d 'EDF6.exe')) { return $d }
    }
    return $null
}

$game = Find-GameDir
if (-not $game) {
    Write-Host (T '没有自动找到 EDF6 的安装位置。' 'EDF6 のインストール先が自動で見つかりませんでした。' 'Could not find the EDF6 folder automatically.') -ForegroundColor Yellow
    Write-Host (T '在 Steam 库里右键 EDF6 → 管理 → 浏览本地文件，把打开的文件夹路径粘贴到这里：' `
        'Steam ライブラリで EDF6 を右クリック → 管理 → ローカルファイルを閲覧 で開いたフォルダのパスをここに貼り付けてください：' `
        'In your Steam library right-click EDF6 -> Manage -> Browse local files, then paste that folder path here:')
    $game = (Read-Host (T '游戏文件夹' 'ゲームフォルダ' 'Game folder')).Trim('"', ' ')
}
# Also covers -GameDir and an empty answer (Join-Path rejects an empty path with a raw error).
if (-not $game -or -not (Test-Path -LiteralPath (Join-Path $game 'EDF6.exe'))) {
    Write-Host ((T '这个文件夹里没有 EDF6.exe：' 'このフォルダに EDF6.exe がありません：' 'No EDF6.exe in this folder: ') + $game) -ForegroundColor Red
    exit 1
}
Write-Host ((T '游戏位置：' 'ゲームの場所：' 'Game folder: ') + $game)

$plugins = Join-Path $game 'Mods\Plugins'
$files = @(
    'Mods\Plugins\EDF6Coop.dll',
    'README_EDF6Coop.txt',
    'README_EDF6Coop_zh.txt',
    'README_EDF6Coop_ja.txt',
    'EDF6Coop_AllowFirewall.bat',
    'EDFModLoader_LICENSE.txt'
)
# What EDF6DirectNet and EDF6MultiSlot put next to the game, besides their DLLs and settings.
$replacedExtras = @('README_EDF6DirectNet.txt', 'README_EDF6DirectNet_zh.txt', 'README_EDF6DirectNet_ja.txt',
    'README_EDF6DirectNet_en.txt', 'EDF6DirectNet_AllowFirewall.bat', 'EDF6DirectNet_防火墙放行.bat',
    'README_EDF6MultiSlot.txt', 'RELEASE_NOTES_EDF6MultiSlot.md')

# Only a game started from this folder holds these files. A process whose path cannot be read (another
# user's, an elevated one) counts as this folder's.
$exe = [IO.Path]::GetFullPath((Join-Path $game 'EDF6.exe'))
$running = @(Get-Process EDF6 -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or $_.Path -ieq $exe })
if ($running.Count) {
    Write-Host (T '请先退出游戏再运行。' 'ゲームを終了してから実行してください。' 'Quit the game first, then run this again.') -ForegroundColor Red
    exit 1
}

$fwRule = 'EDF6Coop (UDP in)'
$fwRuleLegacy = 'EDF6 DirectNet (UDP in)'  # made by EDF6DirectNet_AllowFirewall.bat before 2.0.0
# The description the plugin gives its router mapping (src/upnp.h kUpnpDescription; unchanged since
# EDF6DirectNet, so mappings made by either are recognised).
$upnpDescription = 'EDF6DirectNet'
# SHA-256 of the official EDFModLoader v1.0.10 winmm.dll (it has a multithread bug; the package ships a fixed build).
$loaderOfficialSha256 = 'B80E4DA6AE7264F0E9774C992DE3C5F9B6E9BC9422146ADEEB5BB2BD681E112E'

function Test-Admin { return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }

# EDF6Coop.ini is UTF-16LE with a BOM (the plugin writes it so); the old EDF6DirectNet.ini is UTF-8.
function Read-IniText([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { return [Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2) }
    return [Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF)
}

# The UDP port the plugin mapped: what it recorded when it asked the router, else ListenPort of a host-mode ini.
function Get-MappedPort([string]$plugins) {
    foreach ($name in 'EDF6Coop.upnp', 'EDF6DirectNet.upnp') {
        $record = Join-Path $plugins $name
        if (Test-Path -LiteralPath $record) {
            $m = [regex]::Match((Get-Content -LiteralPath $record -Raw), 'UDP\s+(\d+)')
            if ($m.Success -and [int]$m.Groups[1].Value -ge 1 -and [int]$m.Groups[1].Value -le 65535) { return [int]$m.Groups[1].Value }
        }
    }
    foreach ($name in 'EDF6Coop.ini', 'EDF6DirectNet.ini') {
        $ini = Join-Path $plugins $name
        if (-not (Test-Path -LiteralPath $ini)) { continue }
        $text = Read-IniText $ini
        if ($text -match '(?im)^[ \t]*Mode[ \t]*=[ \t]*host\b') {
            $m = [regex]::Match($text, '(?im)^[ \t]*ListenPort[ \t]*=[ \t]*(\d+)')
            if (-not $m.Success) { return 27015 }
            if ([int]$m.Groups[1].Value -ge 1 -and [int]$m.Groups[1].Value -le 65535) { return [int]$m.Groups[1].Value }
        }
        return 0
    }
    return 0
}

# Removes our UPnP mapping, the way the plugin does: only when it forwards to one of this PC's IPv4
# addresses and carries our description. Any COM error (no UPnP router, UPnP off) ends up as a message.
function Remove-OurUpnpMapping([int]$port) {
    try {
        $nat = New-Object -ComObject HNetCfg.NATUPnP
        $collection = $nat.StaticPortMappingCollection
        if ($null -eq $collection) {
            Write-Host (T '没有找到支持 UPnP 的路由器，无需清理端口映射。' 'UPnP 対応ルーターが見つからないため、ポート開放の削除は不要です。' 'No UPnP router found; no port mapping to remove.')
            return
        }
        $mapping = $collection.Item($port, 'UDP')
        if ($null -eq $mapping) { return }
        $local = @([System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() | ForEach-Object { $_.GetIPProperties().UnicastAddresses } |
            Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' } | ForEach-Object { $_.Address.ToString() })
        if ($mapping.Description -ne $upnpDescription -or $local -notcontains $mapping.InternalClient) {
            Write-Host (T "路由器上的 UDP $port 映射不是本插件在本机建立的，保持不变。" "ルーターの UDP $port の設定はこのプラグインがこの PC で作ったものではないため、そのままにしました。" "The router's UDP $port mapping was not made by this plugin on this PC; left as is.")
            return
        }
        $collection.Remove($port, 'UDP')
        Write-Host (T "已从路由器删除 UPnP 端口映射（UDP $port）。" "ルーターの UPnP ポート開放（UDP $port）を削除しました。" "Removed the UPnP port mapping (UDP $port) from the router.")
    } catch {
        $why = $_.Exception.Message
        Write-Host (T "无法清理路由器上的 UPnP 映射（UDP $port）：$why。如仍在，请在路由器的端口转发页面手动删除描述为 $upnpDescription 的条目。" `
            "ルーターの UPnP 設定（UDP $port）を削除できませんでした：$why。残っている場合はルーターの設定画面で説明が $upnpDescription の項目を手動で削除してください。" `
            "Could not remove the UPnP mapping (UDP $port) from the router: $why. If it is still there, delete the entry described as $upnpDescription in the router's port-forwarding page.") -ForegroundColor Yellow
    }
}

# Firewall rules made by the AllowFirewall .bat (this one's and EDF6DirectNet's): deleting needs administrator rights.
function Remove-FirewallRules {
    foreach ($rule in $fwRule, $fwRuleLegacy) {
        $present = $false
        try { $present = [bool](Get-NetFirewallRule -DisplayName $rule -ErrorAction Stop) } catch {}
        if (-not $present) { continue }
        $removed = $false
        if (Test-Admin) { try { Remove-NetFirewallRule -DisplayName $rule -ErrorAction Stop; $removed = $true } catch {} }
        if ($removed) {
            Write-Host ((T '已删除防火墙放行规则：' 'ファイアウォールの許可規則を削除しました：' 'Removed the firewall rule: ') + $rule)
        } else {
            Write-Host (T '之前添加的防火墙放行规则还在（需要管理员权限才能删除）。不需要的话，以管理员身份打开命令提示符运行：' `
                '以前追加したファイアウォール許可規則が残っています（削除には管理者権限が必要です）。不要なら管理者のコマンドプロンプトで実行してください：' `
                'The firewall rule added earlier is still there (deleting it needs administrator rights). If you no longer need it, run in an administrator command prompt:')
            Write-Host "  netsh advfirewall firewall delete rule name=`"$rule`""
        }
    }
}

if ($Uninstall) {
    # First: the UPnP record (and the ini) tell which port to clean up, and are deleted below.
    $port = Get-MappedPort $plugins
    if ($port -ne 0) { Remove-OurUpnpMapping $port }
    foreach ($f in $files) { Remove-Item -LiteralPath (Join-Path $game $f) -ErrorAction SilentlyContinue }
    # Settings, logs, the crash dump, the UPnP records, and what the auto-updater keeps next to the DLL
    # (.old/.trial/.bad/.rolledback/.new<pid>). EDF6DirectNet.ini / EDF6MultiSlot.ini are the player's
    # old settings and stay; so do the .disabled old plugins.
    foreach ($f in 'EDF6Coop.ini', 'EDF6Coop.log', 'EDF6Coop.log.1', 'EDF6Coop.log.queue', 'EDF6Coop-crash.dmp',
                   'EDF6Coop.upnp', 'EDF6DirectNet.upnp', 'EDF6Coop.dll.old', 'EDF6Coop.dll.trial', 'EDF6Coop.dll.bad',
                   'EDF6Coop.dll.rolledback') {
        Remove-Item -LiteralPath (Join-Path $plugins $f) -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $plugins) {
        Get-ChildItem -LiteralPath $plugins -Filter 'EDF6Coop.dll.new*' -File -ErrorAction SilentlyContinue |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -ErrorAction SilentlyContinue }
    }
    Write-Host (T '已卸载 EDF6Coop。游戏恢复原样（EDFModLoader 和其他 Mod 没有动）。' `
        'EDF6Coop をアンインストールしました（EDFModLoader とほかの Mod はそのままです）。' `
        'EDF6Coop removed. EDFModLoader and other mods were left untouched.') -ForegroundColor Green
    Remove-FirewallRules
    exit 0
}

function Copy-Into([string]$from, [string]$to) {
    $src = (Resolve-Path -LiteralPath $from).Path
    $dst = [System.IO.Path]::GetFullPath($to)
    if ($src -ieq $dst) { return }  # the zip was extracted straight into the game folder
    # .NET calls, not New-Item/Copy-Item: -Destination is wildcard-matched, so a game folder with [ ] in its path breaks it.
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $dst)) | Out-Null
    [System.IO.File]::Copy($src, $dst, $true)
}

foreach ($f in $files) { Copy-Into (Join-Path $here $f) (Join-Path $game $f) }
# The plugins EDF6Coop replaces: both would hook the same game code. Renamed, not deleted, so going back
# is a rename; their .ini files stay for EDF6Coop to carry over.
foreach ($old in 'EDF6DirectNet.dll', 'EDF6MultiSlot.dll') {
    $path = Join-Path $plugins $old
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $disabled = "$path.disabled"
    if (Test-Path -LiteralPath $disabled) { Remove-Item -LiteralPath $disabled -Force }
    [System.IO.File]::Move($path, $disabled)
    Write-Host ((T '已停用旧插件（改名为 .disabled）：' '古いプラグインを無効にしました（.disabled に改名）：' 'Disabled the replaced plugin (renamed to .disabled): ') + $old)
}
foreach ($f in $replacedExtras) { Remove-Item -LiteralPath (Join-Path $game $f) -ErrorAction SilentlyContinue }
Write-Host (T '已安装 EDF6Coop。' 'EDF6Coop をインストールしました。' 'EDF6Coop installed.') -ForegroundColor Green

# EDFModLoader loads the plugin. The package bundles a fixed build of the official v1.0.10 loader (the official one
# has a multithread bug). It is installed when there is no winmm.dll; an existing winmm.dll is replaced only when it
# is byte-identical to the official v1.0.10 one (backed up first). Any other winmm.dll may be a newer or patched
# loader that other mods rely on, and stays as it is. With no ModLoader.ini, EDFModLoader turns every option on.
$loaderSrc = Join-Path $here 'winmm.dll'
$loaderDst = Join-Path $game 'winmm.dll'
if (-not (Test-Path -LiteralPath $loaderDst)) {
    Copy-Into $loaderSrc $loaderDst
    Write-Host (T '已安装 EDFModLoader（v1.0.10 修复版，MIT 许可，见 EDFModLoader_LICENSE.txt）。' `
        'EDFModLoader（v1.0.10 修正版、MIT ライセンス、EDFModLoader_LICENSE.txt 参照）をインストールしました。' `
        'Installed EDFModLoader (v1.0.10, fixed build, MIT license, see EDFModLoader_LICENSE.txt).') -ForegroundColor Green
} elseif ((Test-Path -LiteralPath $loaderSrc) -and
          ((Get-FileHash -LiteralPath $loaderDst -Algorithm SHA256).Hash -eq $loaderOfficialSha256) -and
          ((Get-FileHash -LiteralPath $loaderSrc -Algorithm SHA256).Hash -ne $loaderOfficialSha256)) {
    $backup = Join-Path $game 'winmm.dll.bak-official'
    if (-not (Test-Path -LiteralPath $backup)) { [System.IO.File]::Copy($loaderDst, $backup, $false) }
    Copy-Into $loaderSrc $loaderDst
    Write-Host (T '已把官方 v1.0.10 的 EDFModLoader（winmm.dll）升级为修复版（多线程问题）；原文件备份为 winmm.dll.bak-official。' `
        '公式 v1.0.10 の EDFModLoader（winmm.dll）を修正版（マルチスレッド不具合の修正）に置き換えました。元のファイルは winmm.dll.bak-official に保存しています。' `
        'Replaced the official v1.0.10 EDFModLoader (winmm.dll) with the fixed build (multithread bug); the original is saved as winmm.dll.bak-official.') -ForegroundColor Green
} else {
    Write-Host (T '检测到已有 EDFModLoader（winmm.dll），保持不变。' `
        '既存の EDFModLoader（winmm.dll）が見つかったので、そのままにしました。' `
        'Found an existing EDFModLoader (winmm.dll); left it as is.')
}
Write-Host ''
Write-Host (T '完成！直接从 Steam 启动游戏即可。' '完了！Steam からそのままゲームを起動してください。' 'Done! Start the game from Steam as usual.')
Write-Host ((T '第一次启动后会生成 Mods\Plugins\EDF6Coop.ini（设置，旧设置会自动搬过来）和 .log（日志），说明见 ' `
    '初回起動時に Mods\Plugins\EDF6Coop.ini（設定。古い設定は自動で引き継がれます）と .log（ログ）が作成されます。説明は ' `
    'The first start creates Mods\Plugins\EDF6Coop.ini (settings; your old settings are carried over) and .log (log); see ') + $readme)
