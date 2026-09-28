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
    "\xEF\xBB\xBF; EDF6DirectNet 设置（修改后重启游戏生效）\r\n"
    "; 详细说明见游戏目录的 README_EDF6DirectNet.txt\r\n"
    "\r\n"
    "[DirectNet]\r\n"
    "; 1 启用插件 / 0 完全关闭（关闭后游戏与原版完全一样）\r\n"
    "Enabled=1\r\n"
    "\r\n"
    "; 公网直连模式：off 不用 / host 我开房（别人连我）/ join 我去连房主\r\n"
    "Mode=off\r\n"
    "\r\n"
    "; host：本机监听的 UDP 端口（路由器端口转发、防火墙放行的就是它）\r\n"
    "; join：本机使用的 UDP 端口，0 = 自动\r\n"
    "ListenPort=27015\r\n"
    "\r\n"
    "; join 模式填房主地址，支持：\r\n"
    ";   IPv4       HostAddress=123.45.67.89:27015\r\n"
    ";   IPv6       HostAddress=[2408:8207:1234::5]:27015\r\n"
    ";   域名/DDNS  HostAddress=myroom.ddns.net:27015\r\n"
    "HostAddress=\r\n"
    "\r\n"
    "; 可选的房间暗号。填了之后所有人必须填一样的，否则连不上（防止陌生人往你端口里塞包）\r\n"
    "; 只用英文字母和数字：中文/日文在不同语言的系统上会被读成不同的字节，导致暗号对不上\r\n"
    "Key=\r\n"
    "\r\n"
    "; host 模式自动让路由器做端口映射（UPnP）。路由器不支持时请手动做端口转发\r\n"
    "UPnP=1\r\n"
    "\r\n"
    "; 直连流量固定从物理网卡走，不被 Clash/加速器等 TUN 虚拟网卡劫持。一般不用改\r\n"
    "BindPhysicalInterface=1\r\n"
    "\r\n"
    "; 断线容忍（毫秒）：网络卡住期间数据先缓存、不断重传，恢复后按顺序补齐，游戏不会掉线。\r\n"
    "; 超过这么久仍收不到对方任何包，才判定直连真的断开\r\n"
    "LinkTimeoutMs=60000\r\n"
    "\r\n"
    "[EOS]\r\n"
    "; 游戏原本的 EOS 联机使用的固定 UDP 端口（会用 FixedPort ~ FixedPort+7）。\r\n"
    "; 0 = 保持游戏默认（随机端口）。设了以后在路由器上转发这段端口，EOS 更容易直连而不是走中继\r\n"
    "FixedPort=0\r\n"
    "\r\n"
    "; EOS 中继策略：default 不改 / allow 允许中继 / norelay 禁止中继（只直连，打洞失败就连不上）/ force 强制中继\r\n"
    "Relay=default\r\n"
    "\r\n"
    "[Sync]\r\n"
    "; 防不同步：游戏原本把所有联机数据都用「不可靠」方式发送，网络一丢包就丢了，是画面不同步的来源之一。\r\n"
    "; 1 = 改成「可靠但不排队」发送：丢了自动重发，到了马上交给游戏。只需要发送方装插件就生效。\r\n"
    "; 0 = 原版行为\r\n"
    "ReliableGameTraffic=1\r\n"
    "\r\n"
    "[Resilience]\r\n"
    "; 断线宽限：EOS 连接因超时/网络错误断开时，先不告诉游戏，插件自动重新连接；\r\n"
    "; 恢复了游戏完全感觉不到，不会把人踢出去。\r\n"
    ";   auto 只对「直连」的人生效（他们一定装了插件）；直连还通就一直扣住，永远不会因 EOS 断开而掉人\r\n"
    ";   all  对房间里所有人生效。只有确认一起玩的人【全部】装了本插件才能开：\r\n"
    ";        没装插件的人那边会照常把你踢掉，你这边却以为他还在，两边会对不上\r\n"
    ";   off  关闭（原版行为）\r\n"
    "HoldDisconnects=auto\r\n"
    "\r\n"
    "; all 模式下的宽限秒数：超过这么久还没恢复，才把断线交给游戏处理\r\n"
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
