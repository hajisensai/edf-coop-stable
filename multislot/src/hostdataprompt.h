// The question a player gets before the game uses other members' weapon and vehicle files (hostdatanet.h,
// Accept=Ask): who brings what, which files, and what either answer risks.
//
// The files are fetched and checked before the question (they sit in Mods\Plugins\EDF6Coop.hostdata, which the game
// does not read), so it names every file. It is a window of its own (a message box on a thread of its own): the game
// keeps running behind it, and the answer reaches hostdatanet on whatever thread the window ran.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace multislot {

enum class PromptLanguage { English, Chinese, Japanese };
PromptLanguage PromptLanguageFor(LANGID language);

// One member's files the question is about.
struct PromptSource {
    bool mods = false;               // the room owner's Mods; otherwise a member's weapon page
    std::string member;              // EOS_ProductUserId as text
    std::vector<std::string> paths;  // DataFile paths ("WEAPON/AWEAPON346.SGO")
};

constexpr std::size_t kPromptMaxPaths = 20;  // listed; the rest are counted

// `yours`: the paths this machine's own Mods holds (sorted), so a file can say it takes the place of one of them.
// `key`: the accept key's name ("F1").
std::wstring PromptTitle(PromptLanguage language);
std::wstring PromptText(const std::vector<PromptSource>& sources, const std::vector<std::string>& yours,
                        const wchar_t* key, PromptLanguage language);

// Shows the question and hands `answer` true for "use them". Returns at once; false when no window could be started
// (then `answer` is never called).
bool AskInWindow(std::wstring title, std::wstring text, std::function<void(bool use)> answer);

}  // namespace multislot
