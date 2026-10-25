// The room screen without the game: the member rows roomview.cpp lays out on fake HUiRoom panels (the game's builder,
// SetArea, CreateByName and SetText replaced by recorders), the faction labels, and the input names it keeps.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../src/identity.h"
#include "../src/patches.h"
#include "../src/roomrows.h"
#include "../src/roomview.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// The game's object layouts as roomview.cpp reads them.
struct Block;
struct BlockVtable {
    void(__fastcall* destroy)(Block*);
    void(__fastcall* deleteThis)(Block*);
};
struct Block {
    BlockVtable* vtable;
    long uses;
    long weaks;
};
int destroyed = 0;
void __fastcall Destroy(Block*) { ++destroyed; }
void __fastcall DeleteThis(Block*) {}
BlockVtable blockVtable{&Destroy, &DeleteThis};

struct Node {
    Node* next;
    Node* prev;
    void* object;
    void* control;
};
struct Entry {
    void* object;
    void* control;
};

template <typename T>
T& At(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}

struct Component {
    std::vector<unsigned char> bytes = std::vector<unsigned char>(0x300, 0);
    Block block{&blockVtable, 1, 1};
    Node head{};       // +0x138: the child list
    Node headToo{};    // +0x158: the other one, empty here
    std::deque<Node> nodes;
    void* self() { return bytes.data(); }
    Component() {
        head.next = head.prev = &head;
        headToo.next = headToo.prev = &headToo;
        At<Node*>(self(), 0x138) = &head;
        At<Node*>(self(), 0x158) = &headToo;
        At<std::uint8_t>(self(), 0x1F8) = 1;
        At<float>(self(), 0x170) = At<float>(self(), 0x184) = 1.0f;
    }
    void Add(Component& child) {
        nodes.push_back({&head, head.prev, child.self(), &child.block});
        head.prev->next = &nodes.back();
        head.prev = &nodes.back();
    }
};
float X(Component& c) { return At<float>(c.self(), 0x1A0); }
float Y(Component& c) { return At<float>(c.self(), 0x1A4); }
float W(Component& c) { return At<float>(c.self(), 0x1B8); }
bool Visible(Component& c) { return At<std::uint8_t>(c.self(), 0x1F8) != 0; }

// A panel as the builder leaves it: its children at their stock places, its MemberInfo at +0x200.
struct Panel {
    Component panel;
    std::deque<Component> children;
    std::string member;  // what the member-id source reads from the MemberInfo
    explicit Panel(bool master, std::string id) : member(std::move(id)) {
        At<float>(panel.self(), 0x1B8) = rows::kPanelWidth;
        At<float>(panel.self(), 0x1BC) = master ? rows::kMasterHeight : rows::kMemberHeight;
        for (int ordinal = 0; ordinal < (master ? 18 : 17); ++ordinal) {
            children.emplace_back();
            Component& c = children.back();
            At<std::int32_t>(c.self(), 0x110) = ordinal;
            const rows::Rect r = rows::StockRect(rows::RoleOf(ordinal, master));
            At<float>(c.self(), 0x1A0) = r.x;
            At<float>(c.self(), 0x1A4) = r.y;
            At<float>(c.self(), 0x1B8) = r.w;
            At<float>(c.self(), 0x1BC) = r.h;
            panel.Add(c);
        }
        At<const void*>(panel.self(), 0x200) = &member;
    }
    Component& Child(int ordinal) { return children[static_cast<std::size_t>(ordinal)]; }
};

struct Room {
    std::vector<unsigned char> bytes = std::vector<unsigned char>(0x1000, 0);
    std::deque<Panel> panels;
    std::vector<Entry> list;
    void* self() { return bytes.data(); }
    void Build(std::size_t members) {
        panels.clear();
        list.clear();
        for (std::size_t i = 0; i < members; ++i) {
            panels.emplace_back(i == 0, "member" + std::to_string(i));
            At<float>(panels.back().panel.self(), 0x1A4) = i ? rows::kMasterHeight + rows::kMemberHeight * (i - 1) : 0.0f;
        }
        for (auto& p : panels) list.push_back({p.panel.self(), &p.panel.block});
        At<Entry*>(self(), 0xB58) = list.data();
        At<std::size_t>(self(), 0xB68) = list.size();
    }
};

// The game's functions, recorded.
std::map<void*, std::wstring> texts;
std::map<void*, int> disabled;
std::deque<Component> made;  // components CreateByName made
int areaCalls = 0, createCalls = 0, consumed = 0;
std::wstring lastDefinition;
void __fastcall FakeSetArea(void* button, const float* area) {
    ++areaCalls;
    std::memcpy(static_cast<unsigned char*>(button) + 0x1B0, area, 16);
}
void __fastcall FakeSetEnabled(void* component, bool enabled) {
    if (!enabled) ++disabled[component];
}
void* __fastcall FakeCreate(void* screen, void* out, void* parent, const wchar_t* name) {
    (void)screen;
    ++createCalls;
    lastDefinition = name;
    auto* p = static_cast<Entry*>(parent);
    auto* panelBlock = static_cast<Block*>(p->control);
    --panelBlock->uses;  // consumed
    ++consumed;
    made.emplace_back();
    Component& label = made.back();
    label.block.uses = 2;  // the panel's reference and the caller's
    // The new child goes on the end of the panel's list (the game's tail insert).
    auto* head = At<Node*>(p->object, 0x138);
    auto* node = new Node{head, head->prev, label.self(), &label.block};
    head->prev->next = node;
    head->prev = node;
    *static_cast<Entry*>(out) = {label.self(), &label.block};
    return out;
}
void __fastcall FakeSetText(void* field, const wchar_t* text, const void*) { texts[field] = text; }
void* __cdecl Identity(void* component) { return component; }

const char* MemberOf(const void* memberInfo, char* out, std::size_t outChars) {
    strncpy_s(out, outChars, static_cast<const std::string*>(memberInfo)->c_str(), _TRUNCATE);
    return out;
}

// All Forces' labels: members listed here have a faction.
std::map<std::string, std::pair<std::wstring, std::uint32_t>> factions;
int labelCalls = 0;
int Label(const char* id, wchar_t* out, std::uint32_t outChars, std::uint32_t* argb) {
    ++labelCalls;
    out[0] = 0;
    const auto it = factions.find(id);
    if (it == factions.end()) return 0;
    wcsncpy_s(out, outChars, it->second.first.c_str(), _TRUNCATE);
    *argb = it->second.second;
    return 1;
}

Component* LabelOf(Panel& p) {
    for (Node* n = At<Node*>(p.panel.self(), 0x138)->next; n != At<Node*>(p.panel.self(), 0x138); n = n->next)
        for (auto& m : made)
            if (n->object == m.self()) return &m;
    return nullptr;
}

int builds = 0;
std::uint64_t FakeBuilder(void*) {
    ++builds;
    return 0x1234;
}

}  // namespace

int main() {
    static std::uint8_t gameBase[16];
    InitRoomView(gameBase, RoomViewSettings{});
    SetRoomUiCalls({&FakeSetArea, &FakeSetEnabled, &FakeCreate, &FakeSetText, &Identity});
    SetPanelMemberIdSource(&MemberOf);
    SetMemberLabelSource(&Label);
    Room room;

    // Four or fewer: the game's layout untouched; a label per row where All Forces has one, in the clear rate's place.
    factions["member0"] = {L"EDF / Ranger", 0xFF3C8CFFu};
    factions["member2"] = {L"Monster / Unit", 0xFFE04040u};
    room.Build(3);
    Check(CallBuilder(room.self(), &FakeBuilder) == 0x1234 && builds == 1, "the game's builder runs and its result is kept");
    Check(areaCalls == 0 && Y(room.panels[1].panel) == rows::kMasterHeight && Y(room.panels[2].panel) == rows::kMasterHeight + rows::kMemberHeight,
          "three members: the panels stay where the game put them");
    Check(createCalls == 3 && consumed == 3 && lastDefinition == L"_pl_Name", "a label is made from _pl_Name for every row");
    for (auto& p : room.panels) Check(p.panel.block.uses == 1, "the parent reference CreateByName consumes was the plugin's own");
    for (auto& m : made) Check(m.block.uses == 1, "the plugin lets go of its reference to each label: the panel keeps it");
    Component* label0 = LabelOf(room.panels[0]);
    Component* label1 = LabelOf(room.panels[1]);
    Check(label0 && label1 && Visible(*label0) && !Visible(*label1), "a label shows where All Forces has a faction, only there");
    Check(label0 && texts[label0->self()] == L"EDF / Ranger" && X(*label0) == 332 && Y(*label0) == 122,
          "the label's text, in the clear rate's place");
    const float* colour = label0 ? &At<float>(label0->self(), 0x2B0) : nullptr;
    Check(colour && colour[0] > 0.23f && colour[0] < 0.24f && colour[2] == 1.0f && colour[3] == 1.0f, "in the faction's colour");
    Check(!Visible(room.panels[0].Child(4)) && !Visible(room.panels[0].Child(5)) && Visible(room.panels[1].Child(5)),
          "the clear rate gives way to a label, and only to one");
    Check(X(room.panels[0].Child(1)) == 166 && Visible(room.panels[0].Child(6)), "everything else is the game's");

    // A refresh follows All Forces: a new pick, PvP turned off.
    factions["member0"] = {L"EDF 2 / Ranger", 0xFFF0A030u};
    factions.erase("member2");
    RefreshLabels(room.self());
    Check(label0 && texts[label0->self()] == L"EDF 2 / Ranger", "a changed faction is written again");
    Component* label2 = LabelOf(room.panels[2]);
    Check(label2 && !Visible(*label2) && Visible(room.panels[2].Child(5)), "a faction gone: the label hides, the clear rate is back");
    const int calls = labelCalls;
    SetMemberLabelSource(nullptr);
    RefreshLabels(room.self());
    Check(labelCalls == calls, "without All Forces' export nothing is asked");
    SetMemberLabelSource(&Label);

    // Ten members: one-line rows, every panel moved and sized, the children placed for it.
    made.clear();
    texts.clear();
    room.Build(10);
    CallBuilder(room.self(), &FakeBuilder);
    const rows::Plan ten = rows::PlanRows(10);
    Check(ten.mode == rows::Mode::OneLine && areaCalls == 10, "ten members: one-line rows, each panel's area set");
    bool placed = true;
    for (std::size_t i = 0; i < 10; ++i) {
        rows::Rect r{};
        rows::RowRect(ten, i, r);
        Panel& p = room.panels[i];
        placed = placed && X(p.panel) == r.x && Y(p.panel) == r.y && At<float>(p.panel.self(), 0x1B8) == r.w &&
                 At<float>(p.panel.self(), 0x1BC) == r.h;
    }
    Check(placed, "each row where roomrows.h puts it, the host's first");
    Panel& host = room.panels[0];
    Check(X(host.Child(0)) == 8 && W(host.Child(0)) == 104 && X(host.Child(1)) == 116 && W(host.Child(1)) == 170,
          "class and name on one line");
    Check(!Visible(host.Child(6)) && !Visible(host.Child(11)) && !Visible(host.Child(12)) && !Visible(host.Child(3)),
          "weapons and status hidden; the armor gives way to the label");
    Check(X(host.Child(14)) == 417 && Visible(host.Child(14)) && X(host.Child(13)) == 420, "voice icons moved, the host's mark kept");
    Check(Visible(room.panels[3].Child(3)) && X(room.panels[3].Child(3)) == 296, "a row without a label shows the armor there");
    Component* tenLabel = LabelOf(host);
    Check(tenLabel && X(*tenLabel) == 290 && Visible(*tenLabel), "the host's label beside the name");

    // Forty members: two columns of 16, the first 32 shown; the rest hidden and out of the focus.
    made.clear();
    disabled.clear();
    createCalls = 0;
    room.Build(40);
    CallBuilder(room.self(), &FakeBuilder);
    const rows::Plan forty = rows::PlanRows(40);
    Check(forty.mode == rows::Mode::Narrow && forty.shown == 32 && createCalls == 32, "forty members: 32 rows, 32 labels");
    bool hidden = true;
    for (std::size_t i = 32; i < 40; ++i)
        hidden = hidden && !Visible(room.panels[i].panel) && disabled[room.panels[i].panel.self()] == 1;
    Check(hidden, "panels past the cap are hidden and disabled");
    Check(X(room.panels[16].panel) == forty.rowWidth + rows::kColumnGap && Y(room.panels[16].panel) == 0,
          "the 17th row starts the second column");
    Check(!Visible(room.panels[5].Child(0)) && W(room.panels[5].Child(1)) == forty.rowWidth - 52, "narrow rows: the name, no class");

    // A panel list that changed without a build the hook saw: the old rows are not refreshed.
    std::swap(room.list[0], room.list[1]);
    const int before = labelCalls;
    RefreshLabels(room.self());
    Check(labelCalls == before, "rows of panels that are gone are left alone");

    // Input names the other features keep using.
    Check(PadButtonOffset(L"RightStick") == 0xB8 && PadButtonOffset(L"rt") == 0x48 && PadButtonOffset(L"None") == 0 &&
              PadButtonOffset(L"Triangle") == -1,
          "pad button names");
    Check(VirtualKey(L"F2") == VK_F2 && VirtualKey(L"f24") == VK_F24 && VirtualKey(L"Tab") == VK_TAB &&
              VirtualKey(L"None") == 0 && VirtualKey(L"65") == 65 && VirtualKey(L"F25") == -1 && VirtualKey(L"Q") == -1,
          "key names");
    RoomViewSettings keySettings;
    keySettings.dummyAddKey = VK_F2;
    keySettings.dummyRemoveKey = VK_F2;
    Check(ResolveDummyKeys(keySettings) && keySettings.dummyAddKey == VK_F6 && keySettings.dummyRemoveKey == VK_F7,
          "F2 is never a dummy key");
    keySettings.dummyAddKey = VK_F8;
    keySettings.dummyRemoveKey = VK_F8;
    Check(ResolveDummyKeys(keySettings) && keySettings.dummyAddKey == VK_F8 && keySettings.dummyRemoveKey == VK_F7,
          "add and remove never share a key");
    keySettings.dummyAddKey = VK_F7;
    keySettings.dummyRemoveKey = VK_F7;
    Check(ResolveDummyKeys(keySettings) && keySettings.dummyAddKey == VK_F7 && keySettings.dummyRemoveKey == 0,
          "no remove key when its default is the add key");
    RoomViewSettings defaults;
    Check(!ResolveDummyKeys(defaults) && defaults.dummyAddKey == VK_F6 && defaults.dummyRemoveKey == VK_F7, "defaults do not collide");
    wchar_t keyName[8]{};
    Check(KeyName(VK_F12, keyName, 8) == 3 && std::wcscmp(keyName, L"F12") == 0 && KeyName(65, keyName, 8) == 2 &&
              std::wcscmp(keyName, L"65") == 0,
          "key names for the label");
    Check(DummyCount() == 0, "no fake members are asked for by default");
    SetDummyCount(3);
    Check(DummyCount() == 3, "the fake member count is what the harness adds to the list");
    SetDummyCount(0);

    // The name a member plays under, as the roster logs it (identity.h): a std::wstring of the game's, SSO
    // capacity 7. The longest name read is 64 characters, and 64 CJK characters are 192 bytes of UTF-8.
    struct FakeWideString {
        const wchar_t* chars;
        std::uint8_t padding[8];
        std::size_t size;
        std::size_t capacity;
    };
    std::wstring longName(kMaxNameChars, L'\x65E5');  // 日 x 64
    const FakeWideString wide{longName.c_str(), {}, longName.size(), longName.size()};
    char name[kNameTextBytes]{};
    Check(NameText(&wide, name, sizeof(name)) && std::strlen(name) == kMaxNameChars * 3,
          "a 64-character Japanese name is logged whole, not as (no name)");
    std::wstring forged = L"x\r\n[2026-09-20 00:09:00.000] SHUTDOWN\x2028\x85";
    const FakeWideString evil{forged.c_str(), {}, forged.size(), forged.size() > 7 ? forged.size() : 8};
    Check(NameText(&evil, name, sizeof(name)) && std::strchr(name, '\r') == nullptr && std::strchr(name, '\n') == nullptr &&
              std::strcmp(name, "x??[2026-09-20 00:09:00.000] SHUTDOWN??") == 0,
          "control characters and line separators in a name come out as '?'");
    const FakeWideString tooLong{longName.c_str(), {}, kMaxNameChars + 1, kMaxNameChars + 1};
    Check(!NameText(&tooLong, name, sizeof(name)) && name[0] == 0, "a longer 'name' is not read at all");

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("room view rows verified\n");
    return 0;
}
