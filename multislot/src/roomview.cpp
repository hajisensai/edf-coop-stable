#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "roomview.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "armor.h"
#include "crashlog.h"
#include "identity.h"
#include "joinlog.h"
#include "log.h"
#include "mission.h"
#include "patches.h"
#include "roomrows.h"

namespace multislot {
namespace {

// ui::HUiRoom fields (EDF.dll 678CCB46).
constexpr std::size_t kMembers = 0x900;          // {shared_ptr<MemberInfo>* data; capacity; size}
constexpr std::size_t kRebuild = 0x940;          // byte: OnUpdate rebuilds members and panels
constexpr std::size_t kChildA = 0xAF0;           // control blocks of the child screen / dialog
constexpr std::size_t kChildB = 0xB00;           //   handles OnUpdate drops when it rebuilds
// The room's focus targets: a vector of weak_ptr<Component> (data, capacity, size at +0xB58 / +0xB60 / +0xB68). The
// screen puts its menu buttons there first; the builder keeps the member panels at its end, their number in the int at
// +0xB78: it pops that many off the end (8F8584..8F85D5, then +0xB78 = 0) and appends the new panels from PlayersGroup's
// child list in its order (8F85EC..8F8742, +0xB78 + 1 each). So the panels are the last +0xB78 entries, never the whole
// vector: read from its start, the menu buttons were laid out as member rows (2026-10-31, a one-member room: "8 member
// panels", the buttons 98 apart and the one panel on the eighth row).
constexpr std::size_t kTargets = 0xB58;
constexpr std::size_t kTargetCount = 0xB68;
constexpr std::size_t kPanelCount = 0xB78;
constexpr std::uint32_t kPanelBuilder = 0x8F8020;
constexpr std::uint32_t kRoomOnUpdate = 0x8FDFE0;
// ui::Component (constructor 7E9790, Init 7EBC30): its ordinal under its parent in the layout tree, its two child lists
// (std::list heads; a node is {next, prev, object, control}; 8E1950 looks a child up in both), its local matrix (the
// scale on the diagonal, the translation in the last row), its area {x, y, w, h} and its visibility. The draw (7EC790)
// reads the matrix and the area every frame and skips a hidden component with everything under it.
constexpr std::size_t kOrdinal = 0x110;
constexpr std::size_t kChildren = 0x138;
constexpr std::size_t kChildrenToo = 0x158;
constexpr std::size_t kScaleX = 0x170;
constexpr std::size_t kScaleY = 0x184;
constexpr std::size_t kTranslateX = 0x1A0;
constexpr std::size_t kTranslateY = 0x1A4;
constexpr std::size_t kAreaWidth = 0x1B8;
constexpr std::size_t kAreaHeight = 0x1BC;
constexpr std::size_t kVisible = 0x1F8;
constexpr std::size_t kPanelMember = 0x200;      // the panel's MemberInfo (902328: members[i] for panel i)
constexpr std::size_t kFontColour = 0x2B0;       // ui::TextField: RGBA floats (drawn by 7F890C)
// A member's way to its EOS user: MemberInfo +0x98 weak_ptr<eos::RoomInfo::PlayerInfo> (901AB5..901B60), PlayerInfo
// +0x00 weak_ptr<eos::User> (744B31), User +0x18 its EOS_ProductUserId (as Users::Add's slots, joinlog.cpp).
constexpr std::size_t kMemberPlayerInfo = 0x98;
constexpr std::size_t kPlayerInfoUser = 0x00;
constexpr std::size_t kUserProductId = 0x18;
// The game functions the rows use.
constexpr std::uint32_t kSetArea = 0x7E7FA0;       // HUiButton::SetArea(button, const float4*): area and its own label
constexpr std::uint32_t kSetEnabled = 0x7EE280;    // (component, bool): the pending state the next Update commits
constexpr std::uint32_t kCreateByName = 0x8456D0;  // (screen, Shared* out, Shared* parent (consumed), definition name)
constexpr std::uint32_t kSetText = 0x7F8BB0;       // (ui::TextField*, text, font params or null), as the panel fill
constexpr std::uint32_t kDynamicCast = 0x12DA7AA;  // __RTDynamicCast
constexpr std::uint32_t kComponentType = 0x2062F00;  // type descriptors ui::Component and ui::TextField (8E1991)
constexpr std::uint32_t kTextFieldType = 0x2062F28;
// The definition a faction label is made from: the panel's own name field (TextField, Transparent skin, 16 point). It
// is a top-level definition of LYT_HUIONLINEROOM.SGO, so CreateByName can make another one under any panel.
constexpr const wchar_t* kLabelDefinition = L"_pl_Name";
// Pad input: two slots of 0x4A0 bytes holding pointers to per-button channels; the channel's
// byte +0x4C is the button's current state (EDF+1182A00 writes it every frame).
constexpr std::uint32_t kPadSlots = 0x2136530;
// Armor probe (1.4.0, diagnostic only: it reads and logs, it changes nothing). HUiRoom::MemberInfo keeps
// one record per room member; 901100 fills it from the member list with the getters named here. GameStatus
// keeps the local player's own choice per split-screen slot, 0x3E60 apart.
constexpr std::size_t kMemberNameOk = 0x10;    // byte  the name below has been filled in
// std::wstring of the name the member plays under, SSO capacity 7 (constructor 8F1E80 builds it in place).
constexpr std::size_t kMemberName = 0x18;
constexpr std::size_t kMemberClassOk = 0x08;   // byte  (747340 read the source field)
constexpr std::size_t kMemberClass = 0x0C;     // int   soldier type
constexpr std::size_t kMemberArmorOk = 0x38;   // byte  (745D50)
constexpr std::size_t kMemberArmor = 0x3C;     // int   the number the room panel prints as _pl_Armor
constexpr std::uint32_t kLocalPlayers = 0x6E90;    // GameStatus: class, then the armor field at +4
constexpr std::uint32_t kEffectiveArmor = 0x0D8700;  // (GameStatus, slot) -> the armor limit, or its default
constexpr std::size_t kPadSlotSize = 0x4A0;
constexpr std::size_t kChannelPressed = 0x4C;
constexpr std::uint64_t kLabelInterval = 200;  // ms between faction label refreshes

struct Entry {
    void* object;
    void* control;
};
struct MemberVector {
    Entry* data;
    std::size_t capacity;
    std::size_t size;
};

// MSVC's _Ref_count_base: the vtable's first two entries destroy the object and free the block.
struct ControlBlock;
struct ControlBlockVtable {
    void(__fastcall* destroy)(ControlBlock*);
    void(__fastcall* deleteThis)(ControlBlock*);
};
struct ControlBlock {
    ControlBlockVtable* vtable;
    long uses;
    long weaks;
};

const unsigned char* game = nullptr;
RoomViewSettings config{};
RoomFunction originalBuilder = nullptr;
using OnUpdateFunction = std::uint64_t (*)(void*, void*);
OnUpdateFunction nextOnUpdate = nullptr;  // the game's HUiRoom::OnUpdate, or a hook chained onto its slot first

// Everything below runs on the game's UI thread only (HUiRoom::OnUpdate and what it calls).
void* currentRoom = nullptr;
std::size_t dummyCount = 0;
bool addKeyWasDown = false;
bool removeKeyWasDown = false;

MemberVector* Members(void* room) { return static_cast<MemberVector*>(static_cast<void*>(static_cast<char*>(room) + kMembers)); }

bool Alive(void* control) {
    return control && static_cast<ControlBlock*>(control)->uses > 0;
}

void RequestRebuild(void* room) { static_cast<std::uint8_t*>(room)[kRebuild] = 1; }

// A child screen or dialog of the room is alive; rebuilding now would drop it. An unreadable
// field counts as open, so nothing is rebuilt.
bool ChildOpen(void* room) {
    return Probing([&]() -> bool {
        __try {
            const auto* bytes = static_cast<const char*>(room);
            return Alive(*reinterpret_cast<void* const*>(bytes + kChildA)) || Alive(*reinterpret_cast<void* const*>(bytes + kChildB));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return true;
        }
    });
}

bool PadDown(std::uint32_t channel) {
    if (!channel) return false;
    return Probing([&]() -> bool {
        __try {
            for (std::size_t slot = 0; slot < 2; ++slot) {
                const auto* button = *reinterpret_cast<const std::uint8_t* const*>(game + kPadSlots + slot * kPadSlotSize + channel);
                if (button && button[kChannelPressed]) return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return false;
    });
}

bool KeyDown(int key) {
    if (!key) return false;
    const HWND window = GetForegroundWindow();
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId() && (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool Edge(bool down, bool& was) {
    const bool pressed = down && !was;
    was = down;
    return pressed;
}

int lastLocalClass = -2, lastLocalArmor = -2;

const std::uint8_t* GameStatus() {
    if (!game) return nullptr;
    std::uint64_t status = 0;
    std::memcpy(&status, game + kGameStatusPointer, sizeof(status));
    return reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(status));
}

int Field32(const std::uint8_t* at, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, at + offset, sizeof(value));
    return value;
}

// Diagnostic: what the game holds for this machine's own player. Logged once and again whenever it
// changes, so picking a different armor or class on the equipment screen shows up here.
void ProbeLocalArmor() {
    Probing([&] {
        __try {
            const std::uint8_t* status = GameStatus();
            if (!status) return;
            const int soldier = Field32(status, kLocalPlayers);
            const int armor = Field32(status, kLocalPlayers + 4);
            if (soldier == lastLocalClass && armor == lastLocalArmor) return;
            lastLocalClass = soldier;
            lastLocalArmor = armor;
            const auto effective = reinterpret_cast<int(__fastcall*)(const void*, int)>(game + kEffectiveArmor);
            int ownClass = 0, pickups = 0, own = 0;
            const bool read = LocalArmor(ownClass, pickups, own);
            Log("ARMOR you: class %d, armor %d from %d pickups%s (armor limit setting %d, effective %d)", soldier,
                read ? own : -1, read ? pickups : -1, read ? "" : " - could not be read", armor, effective(status, 0));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    });
}

const char* SoldierName(int soldier) {
    switch (soldier) {
        case 0: return "Ranger";
        case 1: return "Wing Diver";
        case 2: return "Air Raider";
        case 3: return "Fencer";
        default: return "?";
    }
}

// The roster, written once whenever it changes. A member's name is the one thing in a log that says who
// the person is; the EOS ProductUserId on the user-slot lines says which line in another log is theirs.
char lastRoster[768]{};

void NoteRoster(const MemberVector* members, std::size_t count, const RoomMemberArmor* seen) {
    NoteUserSlots(count);  // Also runs when a pending user's arrival leaves the displayed roster unchanged.
    char roster[768]{};
    int used = 0;
    for (std::size_t i = 0; i < count && used >= 0; ++i) {
        const auto* member = static_cast<const std::uint8_t*>(members->data[i].object);
        char name[kNameTextBytes]{};
        if (member && member[kMemberNameOk] != 0) NameText(member + kMemberName, name, sizeof(name));
        const int written =
            _snprintf_s(roster + used, sizeof(roster) - used, _TRUNCATE, "%s%zu %s %s %d", used ? ", " : "", i,
                        name[0] ? name : "(no name)", seen[i].valid ? SoldierName(seen[i].soldier) : "not chosen",
                        seen[i].valid ? seen[i].armor : 0);
        used = written < 0 ? -1 : used + written;
    }
    if (std::strcmp(roster, lastRoster) == 0) return;
    _snprintf_s(lastRoster, sizeof(lastRoster), _TRUNCATE, "%s", roster);
    Log("ROOM members (%zu): %s", count, roster);
}

// What "copy armor" needs from the room screen: the class and armor of every member, in panel order.
void NoteMembers(void* room) {
    Probing([&] {
        __try {
            const MemberVector* members = Members(room);
            const std::size_t count = members->size;
            if (!members->data || count > static_cast<std::size_t>(kMaxPlayers)) {
                ForgetRoom();
                return;
            }
            RoomMemberArmor seen[kMaxPlayers]{};
            for (std::size_t i = 0; i < count; ++i) {
                const auto* member = static_cast<const std::uint8_t*>(members->data[i].object);
                if (!member) continue;
                seen[i].valid = member[kMemberClassOk] != 0 && member[kMemberArmorOk] != 0;
                seen[i].soldier = Field32(member, kMemberClass);
                seen[i].armor = Field32(member, kMemberArmor);
            }
            NoteRoster(members, count, seen);
            NoteRoomMembers(seen, count);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ForgetRoom();
        }
    });
}

void Poll(void* room) {
    if (room != currentRoom) {
        currentRoom = room;
        lastRoster[0] = 0;  // a different room writes its roster again
        ForgetUserSlots();
    }
    ProbeLocalArmor();
    NoteMembers(room);
    const bool add = config.dummies && Edge(KeyDown(config.dummyAddKey), addKeyWasDown);
    const bool remove = config.dummies && Edge(KeyDown(config.dummyRemoveKey), removeKeyWasDown);
    if (!add && !remove) return;
    if (ChildOpen(room)) return;
    const std::size_t shown = Members(room)->size;
    const std::size_t previous = dummyCount;
    std::size_t next = dummyCount;
    if (add && shown < static_cast<std::size_t>(kMaxPlayers)) ++next;
    if (remove && next) --next;
    if (next == dummyCount) return;
    dummyCount = next;
    RequestRebuild(room);
    // `shown` still holds the previous frame's list: say what the next rebuild will show.
    Log("ROOMVIEW fake members %zu (the room screen will show %zu)", dummyCount,
        shown >= previous ? shown - previous + dummyCount : dummyCount);
}

// --- Member rows (roomrows.h) ---

RoomUiCalls ui{};
MemberLabelFn labelSource = nullptr;
bool labelSourceSet = false;  // a test's source; otherwise EDF6VehicleCrew.dll's, looked up per room screen
void* labelRoom = nullptr;    // the room screen the plugin's source was looked up for
PanelMemberIdFn memberIdSource = nullptr;

// What the rows were laid out with, for the label refreshes until the next build.
struct Row {
    void* panel;
    void* label;  // the faction label made for this row, null without one
    bool master;
    bool labelShown;
    std::uint32_t argb;
    wchar_t text[64];
};
void* rowsRoom = nullptr;
rows::Plan plan{};
Row rowState[rows::kMaxRows]{};
std::size_t rowCount = 0;
std::uint64_t lastRefresh = 0;
// What was logged last, so a rebuild with the same rows says nothing.
std::size_t loggedMembers = ~std::size_t{0};
bool loggedLabels = false;
const char* loggedFailure = nullptr;

const char* ModeName(rows::Mode m) {
    switch (m) {
        case rows::Mode::Stock: return "the game's own";
        case rows::Mode::TwoLine: return "two-line";
        case rows::Mode::OneLine: return "one-line";
        default: return "two columns of";
    }
}

template <typename T>
T& Field(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}

void ReleaseShared(void* control) {
    auto* block = static_cast<ControlBlock*>(control);
    if (block && InterlockedDecrement(&block->uses) == 0) {
        block->vtable->destroy(block);
        if (InterlockedDecrement(&block->weaks) == 0) block->vtable->deleteThis(block);
    }
}

void Apply(void* child, const rows::Place& p) {
    if (p.move) {
        Field<float>(child, kTranslateX) = p.x;
        Field<float>(child, kTranslateY) = p.y;
    }
    if (p.width > 0) Field<float>(child, kAreaWidth) = p.width;
    if (p.height > 0) Field<float>(child, kAreaHeight) = p.height;
    if (p.scale > 0) {
        Field<float>(child, kScaleX) = p.scale;
        Field<float>(child, kScaleY) = p.scale;
    }
    if (p.shown != rows::Shown::Keep) Field<std::uint8_t>(child, kVisible) = p.shown == rows::Shown::Show ? 1 : 0;
}

// Every direct child of `panel` but its faction label, placed for this plan. Both child lists are walked (8E1950
// searches both); placing a child twice writes the same values twice.
void PlaceChildren(void* panel, bool master, bool label, const void* labelChild) {
    for (std::size_t list : {kChildren, kChildrenToo}) {
        auto* head = Field<unsigned char*>(panel, list);
        if (!head) continue;
        std::size_t steps = 0;
        for (auto* node = *reinterpret_cast<unsigned char**>(head); node && node != head && steps < 64;
             node = *reinterpret_cast<unsigned char**>(node), ++steps) {
            void* child = *reinterpret_cast<void**>(node + 0x10);
            if (!child || child == labelChild) continue;
            const rows::Role role = rows::RoleOf(Field<std::int32_t>(child, kOrdinal), master);
            if (role != rows::Role::Other) Apply(child, rows::PlaceChild(plan, role, label));
        }
    }
}

// A faction label under `panel`: a new _pl_Name made by the game's own CreateByName, which hands the panel a strong
// reference to it; ours is let go at once (the label lives and dies with its panel). Hidden until it has a text.
void* MakeLabel(void* room, void* panel, void* control) {
    if (!ui.createByName) return nullptr;
    InterlockedIncrement(&static_cast<ControlBlock*>(control)->uses);  // the parent copy CreateByName consumes
    Entry parent{panel, control}, made{};
    ui.createByName(room, &made, &parent, kLabelDefinition);
    void* label = made.object;
    if (label) {
        Apply(label, rows::PlaceLabel(plan));
        Field<std::uint8_t>(label, kVisible) = 0;
    }
    if (made.control) ReleaseShared(made.control);
    return label;
}

// The row's member, as All Forces knows it: the EOS ProductUserId behind the panel's MemberInfo.
const char* PanelMemberId(void* panel, char* out, std::size_t outChars) {
    out[0] = 0;
    const void* member = Field<void*>(panel, kPanelMember);
    return member && memberIdSource ? memberIdSource(member, out, outChars) : out;
}

// One row's label as of now: its text and colour, and what it takes the place of.
void ShowLabel(Row& row) {
    if (!row.label || !labelSource) return;
    char id[64]{};
    wchar_t text[64]{};
    std::uint32_t argb = 0;
    const bool shown = PanelMemberId(row.panel, id, sizeof(id))[0] && labelSource(id, text, 64, &argb) == 1 && text[0];
    if (shown != row.labelShown) {
        PlaceChildren(row.panel, row.master, shown, row.label);
        Field<std::uint8_t>(row.label, kVisible) = shown ? 1 : 0;
        row.labelShown = shown;
    }
    if (!shown || (std::wcscmp(text, row.text) == 0 && argb == row.argb)) return;
    void* field = ui.textField ? ui.textField(row.label) : nullptr;
    if (!field) return;
    ui.setText(field, text, nullptr);
    const float colour[4] = {static_cast<float>((argb >> 16) & 0xFF) / 255.0f, static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                             static_cast<float>(argb & 0xFF) / 255.0f, static_cast<float>((argb >> 24) & 0xFF) / 255.0f};
    std::memcpy(static_cast<unsigned char*>(field) + kFontColour, colour, sizeof(colour));
    std::memcpy(row.text, text, sizeof(row.text));
    row.argb = argb;
}

// The member panels at the end of the room's focus targets (kTargets): `count` of them from `panels`. False when the
// numbers do not make a panel list (more panels than targets or than a room holds, no data).
bool PanelsOf(void* room, const Entry*& panels, std::size_t& count) {
    const auto* targets = Field<Entry*>(room, kTargets);
    const std::size_t total = Field<std::size_t>(room, kTargetCount);
    const int built = Field<int>(room, kPanelCount);
    if (!targets || built < 0 || built > kMaxPlayers || static_cast<std::size_t>(built) > total) return false;
    count = static_cast<std::size_t>(built);
    panels = targets + (total - count);
    return true;
}

// The panels of `room` as the builder left them, laid out by `plan`. False when a field could not be read (nothing
// after it is touched; the log says so once).
bool LayOut(void* room, const char*& failure) {
    __try {
        const Entry* panels = nullptr;
        std::size_t count = 0;
        if (!PanelsOf(room, panels, count)) {
            failure = "the room's panel list is not one";
            return false;
        }
        plan = rows::PlanRows(count);
        for (std::size_t i = 0; i < count; ++i) {
            void* panel = panels[i].object;
            if (!panel || !Alive(panels[i].control)) continue;
            rows::Rect r{};
            if (plan.mode != rows::Mode::Stock) {
                if (!rows::RowRect(plan, i, r)) {
                    // Past the cap: neither drawn nor reached by the focus.
                    Field<std::uint8_t>(panel, kVisible) = 0;
                    ui.setEnabled(panel, false);
                    continue;
                }
                const float area[4] = {0.0f, 0.0f, r.w, r.h};
                ui.setArea(panel, area);
                Field<float>(panel, kTranslateX) = r.x;
                Field<float>(panel, kTranslateY) = r.y;
            }
            // At most kMaxRows rows are shown (Stock: at most four), so every shown panel has a row.
            if (rowCount == rows::kMaxRows) continue;
            Row& row = rowState[rowCount++];
            row = Row{panel, nullptr, i == 0, false, 0, {}};
            if (labelSource) row.label = MakeLabel(room, panel, panels[i].control);
            PlaceChildren(panel, row.master, false, row.label);
        }
        for (std::size_t i = 0; i < rowCount; ++i) ShowLabel(rowState[i]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        failure = "a panel could not be read or laid out";
        return false;
    }
}

bool Refresh(void* room) {
    __try {
        const Entry* panels = nullptr;
        std::size_t count = 0;
        if (!PanelsOf(room, panels, count)) return false;
        for (std::size_t i = 0, at = 0; i < count && at < rowCount; ++i) {
            if (!panels[i].object || !Alive(panels[i].control)) continue;
            // The rows of the last build only: a panel list that changed since means a build the hook did not see.
            if (panels[i].object != rowState[at].panel) return false;
            ShowLabel(rowState[at++]);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ResolveLabelSource(void* room) {
    if (labelSourceSet || room == labelRoom) return;
    labelRoom = room;
    const HMODULE allForces = GetModuleHandleW(L"EDF6VehicleCrew.dll");
    labelSource = allForces ? reinterpret_cast<MemberLabelFn>(GetProcAddress(allForces, "EDF6AF_PvpMemberLabel")) : nullptr;
}

// The game's way from MemberInfo to the member's EOS user, each weak_ptr checked alive (on this, the UI thread, nothing
// else ends them meanwhile).
const void* ProductIdOf(const void* member) {
    __try {
        const auto* m = static_cast<const unsigned char*>(member);
        const auto* info = *reinterpret_cast<const unsigned char* const*>(m + kMemberPlayerInfo);
        if (!info || !Alive(*reinterpret_cast<void* const*>(m + kMemberPlayerInfo + 8))) return nullptr;
        const auto* user = *reinterpret_cast<const unsigned char* const*>(info + kPlayerInfoUser);
        if (!user || !Alive(*reinterpret_cast<void* const*>(info + kPlayerInfoUser + 8))) return nullptr;
        return *reinterpret_cast<const void* const*>(user + kUserProductId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const char* GameMemberId(const void* member, char* out, std::size_t outChars) {
    const void* id = Probing([&] { return ProductIdOf(member); });
    return ProductUserIdText(id, out, outChars);
}

// The component as the ui::TextField the game's own fill writes to (8E1950's cast).
void* GameTextField(void* component) {
    using Cast = void*(__cdecl*)(void*, long, void*, void*, int);
    return reinterpret_cast<Cast>(const_cast<unsigned char*>(game) + kDynamicCast)(
        component, 0, const_cast<unsigned char*>(game) + kComponentType, const_cast<unsigned char*>(game) + kTextFieldType, 0);
}

}  // namespace

int PadButtonOffset(const wchar_t* name) {
    struct Named {
        const wchar_t* name;
        int offset;
    };
    constexpr Named buttons[] = {
        {L"None", 0}, {L"RightStick", 0xB8}, {L"LeftStick", 0xB0}, {L"Back", 0x90}, {L"Start", 0x98},
        {L"X", 0x80},  {L"Y", 0x88},          {L"LB", 0xA0},        {L"RB", 0xA8},   {L"LT", 0x40},
        {L"RT", 0x48},
    };
    for (const auto& button : buttons)
        if (_wcsicmp(name, button.name) == 0) return button.offset;
    return -1;
}

const wchar_t* PadButtonShortName(std::uint32_t offset) {
    switch (offset) {
        case 0xB8: return L"RS";
        case 0xB0: return L"LS";
        case 0x90: return L"Back";
        case 0x98: return L"Start";
        case 0x80: return L"X";
        case 0x88: return L"Y";
        case 0xA0: return L"LB";
        case 0xA8: return L"RB";
        case 0x40: return L"LT";
        case 0x48: return L"RT";
        default: return L"";
    }
}

std::size_t KeyName(int key, wchar_t* out, std::size_t outChars) {
    if (!out || !outChars) return 0;
    int written = 0;
    if (key == VK_TAB) written = _snwprintf_s(out, outChars, _TRUNCATE, L"Tab");
    else if (key >= VK_F1 && key <= VK_F24) written = _snwprintf_s(out, outChars, _TRUNCATE, L"F%d", key - VK_F1 + 1);
    else written = _snwprintf_s(out, outChars, _TRUNCATE, L"%d", key);
    if (written < 0) {
        out[0] = 0;
        return 0;
    }
    return static_cast<std::size_t>(written);
}

int VirtualKey(const wchar_t* name) {
    if (_wcsicmp(name, L"None") == 0) return 0;
    if (_wcsicmp(name, L"Tab") == 0) return VK_TAB;
    if ((name[0] == L'F' || name[0] == L'f') && name[1] >= L'1' && name[1] <= L'9') {
        wchar_t* end = nullptr;
        const long number = std::wcstol(name + 1, &end, 10);
        if (end && !*end && number >= 1 && number <= 24) return VK_F1 + static_cast<int>(number) - 1;
        return -1;
    }
    wchar_t* end = nullptr;
    const long code = std::wcstol(name, &end, 10);
    return (end && end != name && !*end && code > 0 && code < 256) ? static_cast<int>(code) : -1;
}

bool ResolveDummyKeys(RoomViewSettings& settings) {
    const auto taken = [&](int key, int other) { return key && (key == kEightPlayerModKey || key == other); };
    bool changed = false;
    if (taken(settings.dummyAddKey, 0)) {
        settings.dummyAddKey = VK_F6;
        changed = true;
    }
    if (taken(settings.dummyRemoveKey, settings.dummyAddKey)) {
        settings.dummyRemoveKey = taken(VK_F7, settings.dummyAddKey) ? 0 : VK_F7;
        changed = true;
    }
    return changed;
}

void InitRoomView(const unsigned char* gameBase, const RoomViewSettings& settings) {
    game = gameBase;
    config = settings;
    originalBuilder = reinterpret_cast<RoomFunction>(gameBase + kPanelBuilder);
    nextOnUpdate = reinterpret_cast<OnUpdateFunction>(gameBase + kRoomOnUpdate);
    auto* base = const_cast<unsigned char*>(gameBase);
    ui.setArea = reinterpret_cast<decltype(ui.setArea)>(base + kSetArea);
    ui.setEnabled = reinterpret_cast<decltype(ui.setEnabled)>(base + kSetEnabled);
    ui.createByName = reinterpret_cast<decltype(ui.createByName)>(base + kCreateByName);
    ui.setText = reinterpret_cast<decltype(ui.setText)>(base + kSetText);
    ui.textField = &GameTextField;
    memberIdSource = &GameMemberId;
}

void ChainRoomOnUpdate(std::uint64_t next) {
    nextOnUpdate = reinterpret_cast<OnUpdateFunction>(static_cast<std::uintptr_t>(next));
}

void LayoutRows(void* room) {
    ResolveLabelSource(room);
    rowsRoom = room;
    rowCount = 0;
    plan = {};
    const char* failure = nullptr;
    const bool laid = Probing([&] { return LayOut(room, failure); });
    if (!laid) {
        rowsRoom = nullptr;
        rowCount = 0;
        if (failure != loggedFailure) Log("ROOMVIEW the member rows were left as the game built them: %s", failure);
        loggedFailure = failure;
        return;
    }
    loggedFailure = nullptr;
    lastRefresh = GetTickCount64();
    const bool labels = labelSource != nullptr;
    if (plan.members == loggedMembers && labels == loggedLabels) return;
    loggedMembers = plan.members;
    loggedLabels = labels;
    if (plan.mode == rows::Mode::Stock)
        Log("ROOMVIEW %zu member panel(s), the game's own layout%s", plan.members,
            labels ? "; All Forces' faction labels on them" : "");
    else
        Log("ROOMVIEW %zu member panels: %s rows %.0f x %.0f, %zu shown%s%s", plan.members, ModeName(plan.mode),
            plan.rowWidth, plan.rowHeight, plan.shown,
            plan.shown < plan.members ? " (the room screen shows the first 32; the rest are in the room, not on it)" : "",
            labels ? "; All Forces' faction labels on them" : "");
}

void RefreshLabels(void* room) {
    if (room != rowsRoom || !rowCount || !labelSource) return;
    if (!Probing([&] { return Refresh(room); })) rowCount = 0;  // the panels it knew are gone; the next build lays out anew
}

std::uint64_t CallBuilder(void* room, RoomFunction original) {
    const std::uint64_t result = original(room);
    LayoutRows(room);
    return result;
}

std::uint64_t BuildPanelsHook(void* room) { return CallBuilder(room, originalBuilder); }

std::uint64_t RoomOnUpdateHook(void* room, void* context) {
    Poll(room);
    ResolveLabelSource(room);
    const std::uint64_t now = GetTickCount64();
    if (now - lastRefresh >= kLabelInterval) {
        lastRefresh = now;
        RefreshLabels(room);
    }
    return nextOnUpdate(room, context);
}

void SetDummyCount(std::size_t count) { dummyCount = count; }

std::size_t DummyCount() { return dummyCount; }

void SetMemberLabelSource(MemberLabelFn label) {
    labelSource = label;
    labelSourceSet = true;
}

void SetPanelMemberIdSource(PanelMemberIdFn source) { memberIdSource = source; }

void SetRoomUiCalls(const RoomUiCalls& calls) { ui = calls; }

bool PadButtonDown(std::uint32_t channel) { return PadDown(channel); }

}  // namespace multislot
