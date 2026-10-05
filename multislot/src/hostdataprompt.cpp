#include "hostdataprompt.h"

#include <algorithm>
#include <memory>

#include "log.h"

namespace multislot {
namespace {

struct Words {
    const wchar_t* title;
    const wchar_t* intro;        // the room has files that differ from yours
    const wchar_t* hostMods;     // "the room host's Mods"
    const wchar_t* memberPage;   // "the weapon page of member "
    const wchar_t* fileCount;    // " file(s)" after a number
    const wchar_t* filesHead;    // the list's heading
    const wchar_t* replacesYours;
    const wchar_t* more;         // "... and N more" around the number: before it
    const wchar_t* moreTail;     // after it
    const wchar_t* yes;          // what "Yes" does, and its risks
    const wchar_t* no;           // what "No" does, and its risk
    const wchar_t* keyBefore;    // later: press <key> on a menu screen
    const wchar_t* keyAfter;
};

constexpr Words kEnglish{
    L"EDF6Coop - use the room's weapon files?",
    L"Players in this room bring weapon or vehicle data files that differ from yours:\n",
    L"the room host's Mods",
    L"the weapon page of member ",
    L" file(s)",
    L"\nFiles your game would read instead of its own:\n",
    L"  (replaces the one in your Mods)",
    L"  ... and ",
    L" more\n",
    L"\nYes: from the next mission your game reads these files, in this room only. Leaving the room, quitting or a "
    L"crash brings back your own files; nothing in your Mods folder is changed.\n"
    L"Risks: the files come from another player and nobody vouches for them. Only weapon and vehicle data (.SGO) "
    L"is taken - never a DLL or a patch - and it was checked against the SHA-256 that member published, but that "
    L"cannot tell whether the data itself is sound: a bad file can crash the game or make weapons behave oddly. "
    L"Weapons and vehicles may also differ from what you know offline.\n",
    L"\nNo: you keep your own files. Risk: the members of the room then have different data, so their weapons or "
    L"vehicles can behave differently on your screen, and the game can crash (for example when a vehicle has a gun "
    L"in the host's files that yours does not have).\n",
    L"\nYou can change this later with ",
    L" on a menu screen.",
};

constexpr Words kChinese{
    L"EDF6Coop - 使用房间里的武器文件？",
    L"这个房间里有玩家带来的武器/载具数据文件和你的不一样：\n",
    L"房主的 Mods",
    L"成员的武器页 ",
    L" 个文件",
    L"\n你的游戏会改读这些文件（代替原来的）：\n",
    L"  （替换你 Mods 里的同名文件）",
    L"  ……还有 ",
    L" 个\n",
    L"\n选「是」：从下一场任务开始，你的游戏改用这些文件，只在这个房间里有效。离开房间、退出游戏或崩溃后自动换回你自己的"
    L"文件，你 Mods 文件夹里的东西不会被改动。\n"
    L"风险：这些文件来自别的玩家，没有人为它们担保。插件只接收武器和载具数据（.SGO），绝不接收 DLL 或补丁，并且已按对方"
    L"公布的 SHA-256 核对过；但这只能证明文件没被篡改，不能证明数据本身没问题——有问题的文件可能让游戏崩溃或武器表现异常。"
    L"武器和载具的性能也可能和你单机时不同。\n",
    L"\n选「否」：保持你自己的文件。风险：房间里各人的数据不一致，别人的武器或载具在你这边可能表现不同，严重时游戏会崩溃"
    L"（例如房主文件里的载具多了一门炮，而你的游戏里没有）。\n",
    L"\n之后可以在菜单画面按 ",
    L" 切换。",
};

constexpr Words kJapanese{
    L"EDF6Coop - 部屋の武器ファイルを使いますか？",
    L"この部屋には、あなたのものと異なる武器・ビークルのデータファイルを持つプレイヤーがいます：\n",
    L"部屋のホストの Mods",
    L"メンバーの武器ページ ",
    L" 個のファイル",
    L"\nゲームが元のファイルの代わりに読むファイル：\n",
    L"  （あなたの Mods の同名ファイルと入れ替え）",
    L"  ……ほか ",
    L" 個\n",
    L"\nはい：次のミッションから、この部屋の中だけこれらのファイルを使います。部屋を出る・ゲームを終了する・クラッシュする"
    L"と自分のファイルに戻り、Mods フォルダーの中身は変更されません。\n"
    L"リスク：これらは他のプレイヤーのファイルで、誰も安全を保証しません。受け取るのは武器・ビークルのデータ（.SGO）だけで"
    L"DLL やパッチは受け取らず、そのメンバーが公開した SHA-256 で照合済みですが、それで分かるのは改ざんがないことだけで、"
    L"データ自体が正しいかは分かりません。不正なファイルでゲームがクラッシュしたり、武器の挙動がおかしくなることがあります。"
    L"武器やビークルの性能がオフラインと違うこともあります。\n",
    L"\nいいえ：自分のファイルのままです。リスク：部屋の中でデータが食い違うため、他の人の武器やビークルがあなたの画面では"
    L"違う動きをしたり、ゲームがクラッシュしたりすることがあります（例：ホストのファイルのビークルにだけ砲が付いている）。\n",
    L"\nあとからメニュー画面で ",
    L" を押して切り替えられます。",
};

const Words& WordsFor(PromptLanguage language) {
    if (language == PromptLanguage::Chinese) return kChinese;
    if (language == PromptLanguage::Japanese) return kJapanese;
    return kEnglish;
}

std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

struct Question {
    std::wstring title;
    std::wstring text;
    std::function<void(bool)> answer;
};

DWORD WINAPI QuestionThread(void* parameter) {
    const std::unique_ptr<Question> question(static_cast<Question*>(parameter));
    // No owner: the game's window stays responsive behind it. Topmost, so it is not lost behind a borderless game.
    const int choice = MessageBoxW(nullptr, question->text.c_str(), question->title.c_str(),
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
    question->answer(choice == IDYES);
    return 0;
}

}  // namespace

PromptLanguage PromptLanguageFor(LANGID language) {
    switch (PRIMARYLANGID(language)) {
    case LANG_CHINESE: return PromptLanguage::Chinese;
    case LANG_JAPANESE: return PromptLanguage::Japanese;
    default: return PromptLanguage::English;
    }
}

std::wstring PromptTitle(PromptLanguage language) { return WordsFor(language).title; }

std::wstring PromptText(const std::vector<PromptSource>& sources, const std::vector<std::string>& yours,
                        const wchar_t* key, PromptLanguage language) {
    const Words& words = WordsFor(language);
    std::wstring text = words.intro;
    std::vector<std::string> paths;
    for (const PromptSource& source : sources) {
        text += L"  - ";
        text += source.mods ? std::wstring(words.hostMods) : words.memberPage + Wide(source.member.substr(0, 8));
        text += L": " + std::to_wstring(source.paths.size()) + words.fileCount + L"\n";
        paths.insert(paths.end(), source.paths.begin(), source.paths.end());
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    text += words.filesHead;
    for (std::size_t i = 0; i < paths.size() && i < kPromptMaxPaths; ++i) {
        text += L"  " + Wide(paths[i]);
        if (std::binary_search(yours.begin(), yours.end(), paths[i])) text += words.replacesYours;
        text += L"\n";
    }
    if (paths.size() > kPromptMaxPaths)
        text += words.more + std::to_wstring(paths.size() - kPromptMaxPaths) + words.moreTail;
    text += words.yes;
    text += words.no;
    if (key && key[0]) text += words.keyBefore + std::wstring(key) + words.keyAfter;
    return text;
}

bool AskInWindow(std::wstring title, std::wstring text, std::function<void(bool use)> answer) {
    auto* question = new Question{std::move(title), std::move(text), std::move(answer)};
    const HANDLE thread = CreateThread(nullptr, 0, &QuestionThread, question, 0, nullptr);
    if (!thread) {
        Log("Host data: the question about the room's files could not be shown (error %lu)", GetLastError());
        delete question;
        return false;
    }
    CloseHandle(thread);
    return true;
}

}  // namespace multislot
