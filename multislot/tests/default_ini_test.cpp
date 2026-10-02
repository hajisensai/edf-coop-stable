// The INI the plugin writes when there is none (EDF6MultiSlot.ini, kDefaultIni) must hold every key the
// plugin reads in the section it reads it from: a key under another section is silently ignored, and the
// line in the file then promises a setting that does nothing.
//   DefaultIniTests src-folder work-folder
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <utility>

#include "default_ini.h"

namespace {

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// (section, key) of every GetPrivateProfile*W(L"Section", L"Key", ...) call in the plugin's sources.
std::set<std::pair<std::string, std::string>> KeysRead(const std::string& srcFolder) {
    std::set<std::pair<std::string, std::string>> keys;
    const std::regex call(R"re(GetPrivateProfile\w*W\s*\(\s*L"([^"]+)"\s*,\s*L"([^"]+)")re");
    WIN32_FIND_DATAA found{};
    const HANDLE search = FindFirstFileA((srcFolder + "\\*.cpp").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return keys;
    do {
        std::ifstream in(srcFolder + "\\" + found.cFileName, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (std::sregex_iterator it(text.begin(), text.end(), call), end; it != end; ++it) keys.insert({(*it)[1], (*it)[2]});
    } while (FindNextFileA(search, &found));
    FindClose(search);
    return keys;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: DefaultIniTests src-folder work-folder\n");
        return 2;
    }
    const std::string work = argv[2];
    CreateDirectoryA(work.c_str(), nullptr);
    const std::string ini = work + "\\default.ini";
    {
        std::ofstream out(ini, std::ios::binary);
        out << kDefaultIni;
    }
    const std::wstring wini(ini.begin(), ini.end());

    const auto keys = KeysRead(argv[1]);
    Check(keys.size() >= 10, "the plugin's INI reads are found in its sources");
    // Keys the plugin reads but deliberately leaves out of the default file (documented elsewhere).
    // [MultiSlot] EightPlayerRooms: a 2.2 INI's setting, read only when RoomSize is absent (plugin.cpp).
    const std::set<std::pair<std::string, std::string>> optional = {{"MultiSlot", "EightPlayerRooms"}};
    for (const auto& [section, key] : keys) {
        if (optional.count({section, key})) continue;
        const std::wstring wsection(section.begin(), section.end()), wkey(key.begin(), key.end());
        wchar_t value[64]{};
        GetPrivateProfileStringW(wsection.c_str(), wkey.c_str(), L"<missing>", value, 64, wini.c_str());
        Check(std::wstring(value) != L"<missing>", "the default INI has [" + section + "] " + key + " where the plugin reads it");
    }

    DeleteFileA(ini.c_str());
    RemoveDirectoryA(work.c_str());
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("default INI verified (%zu keys)\n", keys.size());
    return 0;
}
