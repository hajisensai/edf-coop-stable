#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace dn {
namespace {

// UTF-8 with BOM so Notepad shows the Chinese comments; the first line is a comment, so the BOM
// never ends up in a key name.
const char kDefaultIni[] =
    "\xEF\xBB\xBF; EDF6DirectNet 设置（改完重启游戏生效）\r\n"
    "\r\n"
    "[DirectNet]\r\n"
    "; 1 启用 / 0 关闭插件（关闭后与原版完全一样）\r\n"
    "Enabled=1\r\n"
    "\r\n"
    "; 公网直连（可选）：off 不用 / host 我开房 / join 我连房主\r\n"
    "Mode=off\r\n"
    "\r\n"
    "; host：监听的 UDP 端口（端口转发、防火墙放行的就是它）；join：本机端口，0 = 自动\r\n"
    "ListenPort=27015\r\n"
    "\r\n"
    "; join 时填房主地址，例：123.45.67.89:27015 / [2408:8207::5]:27015 / myroom.ddns.net:27015\r\n"
    "HostAddress=\r\n"
    "\r\n"
    "; 可选暗号，所有人填一样的，只用英文字母和数字\r\n"
    "Key=\r\n"
    "\r\n"
    "; host 时自动让路由器做端口映射（UPnP）\r\n"
    "UPnP=1\r\n"
    "\r\n"
    "; 直连流量固定走物理网卡，不被 Clash / 加速器的 TUN 网卡劫持\r\n"
    "BindPhysicalInterface=1\r\n"
    "\r\n"
    "; 直连多久收不到对方任何数据才算断开（毫秒）\r\n"
    "LinkTimeoutMs=60000\r\n"
    "\r\n"
    "[EOS]\r\n"
    "; 游戏原本联机的固定 UDP 端口（占用 FixedPort ~ FixedPort+7），0 = 随机。一般不用改\r\n"
    "FixedPort=0\r\n"
    "\r\n"
    "; EOS 中继：default 不改 / allow 允许 / norelay 禁止 / force 强制。一般不用改\r\n"
    "Relay=default\r\n"
    "\r\n"
    "[Sync]\r\n"
    "; 防不同步：联机数据丢包自动重发。1 开 / 0 原版\r\n"
    "ReliableGameTraffic=1\r\n"
    "\r\n"
    "[Resilience]\r\n"
    "; 掉线不重来：网络抖动断开时先不让游戏踢人，后台自动重连\r\n"
    ";   auto 对装了本插件的人生效（自动识别）/ off 原版行为\r\n"
    "HoldDisconnects=auto\r\n"
    "\r\n"
    "; 最多等多少秒，等不回来就按原版处理\r\n"
    "GraceSeconds=30\r\n";

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

Config loadConfig(const std::wstring& iniPath) {
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(iniPath.c_str(), L"wb");
        if (f) {
            fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, f);
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
