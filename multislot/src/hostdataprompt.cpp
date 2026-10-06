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
    const wchar_t* unknownCount; // its member runs an EDF6Coop that does not say how many
    const wchar_t* kept;         // after a source this machine fetched before
    const wchar_t* filesHead;    // the list's heading
    const wchar_t* replacesYours;
    const wchar_t* more;         // "... and N more" around the number: before it
    const wchar_t* moreTail;     // after it
    const wchar_t* unlisted;     // files not known before they are downloaded
    const wchar_t* yes;          // what "Yes" does, and its risks
    const wchar_t* no;           // what "No" does, and its risk
    const wchar_t* keyBefore;    // later: press <key> on a menu screen
    const wchar_t* keyAfter;
};

constexpr Words kEnglish{
    L"EDF6Coop - download the room's weapon files?",
    L"Players in this room bring weapon or vehicle data files that differ from yours:\n",
    L"the room host's Mods",
    L"the weapon page of member ",
    L" file(s)",
    L"number of files not known (an older EDF6Coop)",
    L" - downloaded before, still here",
    L"\nFiles your game would read instead of its own:\n",
    L"  (replaces the one in your Mods)",
    L"  ... and ",
    L" more\n",
    L"\nWhich files they are is known only once they are downloaded; the log names them.\n",
    L"\nYes: they are downloaded now, and from the next mission your game reads them, in this room only. Leaving the "
    L"room, quitting or a crash brings back your own files; nothing in your Mods folder is changed.\n"
    L"Risks: the files come from another player and nobody vouches for them. Only weapon and vehicle data (.SGO) "
    L"is taken - never a DLL or a patch - and it is checked against the SHA-256 that member published, but that "
    L"cannot tell whether the data itself is sound: a bad file can crash the game or make weapons behave oddly. "
    L"Weapons and vehicles may also differ from what you know offline.\n",
    L"\nNo: nothing is downloaded and you keep your own files. Risk: the members of the room then have different "
    L"data, so their weapons or vehicles can behave differently on your screen, and the game can crash (for example "
    L"when a vehicle has a gun in the host's files that yours does not have).\n",
    L"\nYou can change this later with ",
    L" on a menu screen.",
};

constexpr Words kChinese{
    L"EDF6Coop - 下载房间里的武器文件？",
    L"这个房间里有玩家带来的武器/载具数据文件和你的不一样：\n",
    L"房主的 Mods",
    L"成员的武器页 ",
    L" 个文件",
    L"文件数未知（对方的 EDF6Coop 版本较旧）",
    L"（以前下载过，本地还留着）",
    L"\n你的游戏会改读这些文件（代替原来的）：\n",
    L"  （替换你 Mods 里的同名文件）",
    L"  ……还有 ",
    L" 个\n",
    L"\n具体是哪些文件要下载后才知道，会写进日志。\n",
    L"\n选「是」：现在开始下载，从下一场任务开始你的游戏改用这些文件，只在这个房间里有效。离开房间、退出游戏或崩溃后"
    L"自动换回你自己的文件，你 Mods 文件夹里的东西不会被改动。\n"
    L"风险：这些文件来自别的玩家，没有人为它们担保。插件只接收武器和载具数据（.SGO），绝不接收 DLL 或补丁，并且会按对方"
    L"公布的 SHA-256 核对；但这只能证明文件没被篡改，不能证明数据本身没问题——有问题的文件可能让游戏崩溃或武器表现异常。"
    L"武器和载具的性能也可能和你单机时不同。\n",
    L"\n选「否」：什么都不下载，保持你自己的文件。风险：房间里各人的数据不一致，别人的武器或载具在你这边可能表现不同，"
    L"严重时游戏会崩溃（例如房主文件里的载具多了一门炮，而你的游戏里没有）。\n",
    L"\n之后可以在菜单画面按 ",
    L" 切换。",
};

constexpr Words kJapanese{
    L"EDF6Coop - 部屋の武器ファイルをダウンロードしますか？",
    L"この部屋には、あなたのものと異なる武器・ビークルのデータファイルを持つプレイヤーがいます：\n",
    L"部屋のホストの Mods",
    L"メンバーの武器ページ ",
    L" 個のファイル",
    L"ファイル数不明（相手の EDF6Coop が古い版）",
    L"（以前ダウンロード済み）",
    L"\nゲームが元のファイルの代わりに読むファイル：\n",
    L"  （あなたの Mods の同名ファイルと入れ替え）",
    L"  ……ほか ",
    L" 個\n",
    L"\nどのファイルかはダウンロード後に分かり、ログに記録されます。\n",
    L"\nはい：今ダウンロードし、次のミッションから、この部屋の中だけこれらのファイルを使います。部屋を出る・ゲームを終了"
    L"する・クラッシュすると自分のファイルに戻り、Mods フォルダーの中身は変更されません。\n"
    L"リスク：これらは他のプレイヤーのファイルで、誰も安全を保証しません。受け取るのは武器・ビークルのデータ（.SGO）だけで"
    L"DLL やパッチは受け取らず、そのメンバーが公開した SHA-256 で照合しますが、それで分かるのは改ざんがないことだけで、"
    L"データ自体が正しいかは分かりません。不正なファイルでゲームがクラッシュしたり、武器の挙動がおかしくなることがあります。"
    L"武器やビークルの性能がオフラインと違うこともあります。\n",
    L"\nいいえ：何もダウンロードせず、自分のファイルのままです。リスク：部屋の中でデータが食い違うため、他の人の武器や"
    L"ビークルがあなたの画面では違う動きをしたり、ゲームがクラッシュしたりすることがあります（例：ホストのファイルの"
    L"ビークルにだけ砲が付いている）。\n",
    L"\nあとからメニュー画面で ",
    L" を押して切り替えられます。",
};

const Words& WordsFor(PromptLanguage language) {
    if (language == PromptLanguage::Chinese) return kChinese;
    if (language == PromptLanguage::Japanese) return kJapanese;
    return kEnglish;
}

std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// "68 file(s), 632 KB", the count alone when the size is not known, or that neither is.
std::wstring SourceSize(const PromptSource& source, const Words& words) {
    if (!source.paths.empty()) return std::to_wstring(source.paths.size()) + words.fileCount + words.kept;
    if (!source.sized) return words.unknownCount;
    return std::to_wstring(source.files) + words.fileCount + L", " + std::to_wstring((source.bytes + 1023) / 1024) +
           L" KB";
}

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
    bool unlisted = false;
    for (const PromptSource& source : sources) {
        text += L"  - ";
        text += source.mods ? std::wstring(words.hostMods) : words.memberPage + Wide(source.member.substr(0, 8));
        text += L": " + SourceSize(source, words) + L"\n";
        paths.insert(paths.end(), source.paths.begin(), source.paths.end());
        unlisted = unlisted || source.paths.empty();
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (!paths.empty()) text += words.filesHead;
    for (std::size_t i = 0; i < paths.size() && i < kPromptMaxPaths; ++i) {
        text += L"  " + Wide(paths[i]);
        if (std::binary_search(yours.begin(), yours.end(), paths[i])) text += words.replacesYours;
        text += L"\n";
    }
    if (paths.size() > kPromptMaxPaths)
        text += words.more + std::to_wstring(paths.size() - kPromptMaxPaths) + words.moreTail;
    if (unlisted) text += words.unlisted;
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
