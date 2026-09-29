#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace dn {
namespace {

// The default settings file. Keys and defaults are written once; only the comments are translated
// (Chinese, Japanese, English), so the three files can never disagree on a setting. Written as UTF-8
// with BOM so Notepad shows the comments; the first line is a comment, so the BOM never ends up in a
// key name.
struct IniEntry {
    const char* setting;     // "Key=default", or "[Section]"
    const char* comment[3];  // zh, ja, en; lines separated by '\n'
};

const char* const kIniTitle[3] = {
    "EDF6DirectNet 设置（改完重启游戏生效）",
    "EDF6DirectNet 設定（変更後はゲームを再起動すると反映されます）",
    "EDF6DirectNet settings (restart the game after editing)",
};

const IniEntry kIniEntries[] = {
    {"[DirectNet]", {}},
    {"Enabled=1",
     {"1 启用 / 0 关闭插件（关闭后与原版完全一样）",
      "1 有効 / 0 無効（無効にすると元のゲームと完全に同じ）",
      "1 on / 0 off (off = exactly the original game)"}},
    {"Mode=off",
     {"公网直连：off 平时 / host 我当房主（别人进我房间会自动直连我）/ join 手动指定房主地址",
      "直接接続：off 通常 / host 自分がホスト（部屋に入った人が自動で直接つないでくる）/ join ホストのアドレスを手動指定",
      "Direct link: off normal / host I host (players who join my room connect to me directly) / join enter the host address by hand"}},
    {"ListenPort=27015",
     {"host：监听的 UDP 端口（端口转发、防火墙放行的就是它）；join：本机端口，0 = 自动",
      "host：待ち受ける UDP ポート（ポート開放・ファイアウォール許可の対象）。join：自分側のポート、0 = 自動",
      "host: UDP port to listen on (the one to forward and allow in the firewall); join: local port, 0 = automatic"}},
    {"PublicAddress=",
     {"host：告诉别人连哪里。留空 = 自动（公网 IPv6 + UPnP 映射的 IPv4）\n"
      "  自己做了端口映射就手动填公网地址:外部端口，例：123.45.67.89:40000 / myroom.ddns.net:40000",
      "host：参加者の接続先。空欄 = 自動（グローバル IPv6 + UPnP で開けた IPv4）\n"
      "  自分でポートを開けた場合はグローバルアドレス:外部ポートを記入。例：123.45.67.89:40000 / myroom.ddns.net:40000",
      "host: where other players connect. Empty = automatic (public IPv6 + IPv4 mapped via UPnP)\n"
      "  Forwarded a port yourself? Enter public address:external port, e.g. 123.45.67.89:40000 / myroom.ddns.net:40000"}},
    {"AutoJoin=1",
     {"进房后自动直连房主（房主设了 Mode=host 才会生效）。1 开 / 0 关",
      "部屋に入ったらホストへ自動で直接接続（ホストが Mode=host のときのみ）。1 オン / 0 オフ",
      "Connect directly to the room host after joining (only when the host uses Mode=host). 1 on / 0 off"}},
    {"HostAddress=",
     {"手动模式 join 才用：房主地址，例：123.45.67.89:27015 / [2408:8207::5]:27015",
      "手動モード join のみ：ホストのアドレス。例：123.45.67.89:27015 / [2408:8207::5]:27015",
      "Manual join mode only: the host address, e.g. 123.45.67.89:27015 / [2408:8207::5]:27015"}},
    {"Key=",
     {"可选暗号，只用英文字母和数字。不会自动发给别人：房主设了，每个加入者都要填一样的，否则自动直连失败",
      "任意の合言葉（英数字のみ）。自動では伝わりません：ホストが設定したら参加者全員が同じものを記入しないと自動直接接続は失敗します",
      "Optional shared key, letters and digits only. It is not sent to anyone: if the host sets one, every player who joins must enter the same key or auto-connect fails"}},
    {"UPnP=1",
     {"host 时自动让路由器做端口映射（UPnP）",
      "host のときルーターに自動でポートを開けてもらう（UPnP）",
      "host: ask the router to forward the port automatically (UPnP)"}},
    {"BindPhysicalInterface=1",
     {"直连流量固定走物理网卡，不被 Clash / 加速器的 TUN 网卡劫持",
      "直接接続の通信を物理 NIC に固定し、Clash や VPN の TUN アダプターに横取りされないようにする",
      "Keep direct-link traffic on the physical network adapter so Clash / VPN TUN adapters cannot capture it"}},
    {"LinkTimeoutMs=60000",
     {"直连多久收不到对方任何数据才算断开（毫秒）",
      "相手から何も届かない状態が何ミリ秒続いたら直接接続が切れたとみなすか",
      "Milliseconds without any data from the other side before the direct link counts as lost"}},
    {"[EOS]", {}},
    {"FixedPort=0",
     {"游戏原本联机的固定 UDP 端口（占用 FixedPort ~ FixedPort+7），0 = 随机。一般不用改",
      "ゲーム本来のオンライン通信の固定 UDP ポート（FixedPort ～ FixedPort+7 を使用）、0 = ランダム。通常は変更不要",
      "Fixed UDP ports for the game's own online traffic (uses FixedPort to FixedPort+7), 0 = random. Usually leave it"}},
    {"Relay=default",
     {"EOS 中继：default 不改 / allow 允许 / norelay 禁止 / force 强制。一般不用改",
      "EOS リレー：default 変更しない / allow 許可 / norelay 禁止 / force 強制。通常は変更不要",
      "EOS relay: default unchanged / allow / norelay never / force always. Usually leave it"}},
    {"[Sync]", {}},
    {"ReliableGameTraffic=1",
     {"防不同步：联机数据丢包自动重发。1 开 / 0 原版",
      "同期ズレ対策：取りこぼした通信を自動で再送。1 オン / 0 元の動作",
      "Desync fix: lost online packets are resent automatically. 1 on / 0 original"}},
    {"[Resilience]", {}},
    {"HoldDisconnects=auto",
     {"掉线不重来：网络抖动断开时先不让游戏踢人，后台自动重连\n"
      "  auto 对装了本插件的人生效（自动识别）/ off 原版行为",
      "回線落ちでやり直さない：回線が一瞬切れてもすぐには切断扱いにせず、裏で自動再接続\n"
      "  auto このプラグインを入れている人に有効（自動判別）/ off 元の動作",
      "Survive drops: when a connection hiccups, keep the player in the mission while reconnecting in the background\n"
      "  auto for players who run this plugin (detected automatically) / off original behaviour"}},
    {"GraceSeconds=30",
     {"最多等多少秒，等不回来就按原版处理",
      "最大何秒待つか。戻らなければ元のゲームどおりに処理",
      "Wait at most this many seconds, then handle it like the original game"}},
};

int iniLanguage(unsigned short langId) {
    switch (PRIMARYLANGID(langId)) {
    case LANG_CHINESE: return 0;
    case LANG_JAPANESE: return 1;
    default: return 2;
    }
}

std::wstring readString(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(section, key, def, buf, 512, ini.c_str());
    std::wstring s = buf;
    auto notSpace = [](wchar_t c) { return c != L' ' && c != L'\t'; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

int readInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, def, ini.c_str()));
}

std::string toUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

uint16_t clampPort(int v, uint16_t def) { return v >= 0 && v <= 65535 ? static_cast<uint16_t>(v) : def; }

}  // namespace

std::string defaultIni(unsigned short langId) {
    int lang = iniLanguage(langId);
    std::string s = "\xEF\xBB\xBF; ";
    s += kIniTitle[lang];
    s += "\r\n";
    for (const IniEntry& e : kIniEntries) {
        s += "\r\n";
        for (const char* p = e.comment[lang]; p && *p;) {
            const char* end = strchr(p, '\n');
            size_t n = end ? static_cast<size_t>(end - p) : strlen(p);
            s += "; ";
            s.append(p, n);
            s += "\r\n";
            p += n + (end ? 1 : 0);
        }
        s += e.setting;
        s += "\r\n";
    }
    return s;
}

Config loadConfig(const std::wstring& iniPath) {
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(iniPath.c_str(), L"wb");
        if (f) {
            std::string text = defaultIni(GetUserDefaultUILanguage());
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
        }
    }
    Config c;
    const wchar_t* s = L"DirectNet";
    c.enabled = readInt(iniPath, s, L"Enabled", 1) != 0;
    std::wstring mode = lower(readString(iniPath, s, L"Mode", L"off"));
    c.direct.mode = mode == L"host" ? Mode::Host : mode == L"join" ? Mode::Join : Mode::Off;
    c.direct.listenPort = clampPort(readInt(iniPath, s, L"ListenPort", 27015), 27015);
    if (c.direct.mode == Mode::Join && readString(iniPath, s, L"ListenPort", L"").empty()) c.direct.listenPort = 0;
    c.direct.hostAddress = toUtf8(readString(iniPath, s, L"HostAddress", L""));
    c.direct.key = toUtf8(readString(iniPath, s, L"Key", L""));
    c.direct.linkTimeoutMs = static_cast<uint32_t>(std::clamp(readInt(iniPath, s, L"LinkTimeoutMs", 60000), 3000, 300000));
    c.publicAddress = toUtf8(readString(iniPath, s, L"PublicAddress", L""));
    c.autoJoin = readInt(iniPath, s, L"AutoJoin", 1) != 0;
    c.upnp = readInt(iniPath, s, L"UPnP", 1) != 0;
    c.bindPhysicalInterface = readInt(iniPath, s, L"BindPhysicalInterface", 1) != 0;

    c.eosFixedPort = clampPort(readInt(iniPath, L"EOS", L"FixedPort", 0), 0);
    std::wstring relay = lower(readString(iniPath, L"EOS", L"Relay", L"default"));
    c.eosRelay = relay == L"norelay" ? 0 : relay == L"allow" ? 1 : relay == L"force" ? 2 : -1;

    std::wstring hold = lower(readString(iniPath, L"Resilience", L"HoldDisconnects", L"auto"));
    c.hold = hold == L"all" ? Config::Hold::All : (hold == L"off" || hold == L"0") ? Config::Hold::Off : Config::Hold::Auto;
    c.reliableGameTraffic = readInt(iniPath, L"Sync", L"ReliableGameTraffic", 1) != 0;
    c.direct.upgradeUnreliable = c.reliableGameTraffic;
    c.graceMs = static_cast<uint32_t>(std::clamp(readInt(iniPath, L"Resilience", L"GraceSeconds", 30), 1, 600)) * 1000u;
    return c;
}

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Host: return "host";
        case Mode::Join: return "join";
        default: return "off";
    }
}

const char* relayName(int relay) {
    switch (relay) {
        case 0: return "norelay";
        case 1: return "allow";
        case 2: return "force";
        default: return "default";
    }
}

}  // namespace dn
