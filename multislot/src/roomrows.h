#pragma once
// Where the room screen's member rows go (roomview.h). Pure arithmetic, no game: tests/roomrows_test.cpp.
//
// The game builds one panel per member in PlayersGroup (RoomPanelPatches in patches.h): panel 0 from Button_Master,
// every later one from Button_Member, each placed below the one before it. Four of them fill the group (210 + 3 x 188
// of its 780); a fifth and later would hang below it. So with more than four members the plugin lays the panels out
// again after every build (BuildPanelsHook), by these rules, and with four or fewer it leaves the game's layout alone:
//
//   members   rows                                           a row shows
//   1..4      the game's own                                 everything (stock)
//   5..8      one column, 92..120 high                       class, name, voice; armor, faction label, status (small)
//   9..17     one column, 40..60 high                        class, name, faction label (armor when there is none), voice
//   18..32    two columns of 250, 45..60 high                name over its faction label, voice
//   33+       the first 32 as above; the rest are hidden     (kMaxRows: smaller than 45 is not readable at 1080p)
//
// Panel 0 (the room's host, Button_Master) is always the first row, top left; the rest follow in the game's order,
// down the first column, then down the second. Positions are the panel's own (parent PlayersGroup) coordinates, a
// child's are its panel's. Values from Root.cpk UI/LYT_HUIONLINEROOM.SGO (docs/net-re/roompanels.md).
#include <cstddef>

namespace multislot::rows {

constexpr float kGroupWidth = 506.0f;   // PlayersGroup area
constexpr float kGroupHeight = 780.0f;
constexpr float kPanelWidth = 455.0f;   // ButtonMasterBase / ButtonMemberBase area
constexpr float kMasterHeight = 210.0f;
constexpr float kMemberHeight = 188.0f;
constexpr std::size_t kStockRows = 4;   // what the group holds as the game lays it out
constexpr float kRowGap = 4.0f;
constexpr float kColumnGap = 6.0f;
constexpr float kTwoLineMin = 92.0f, kTwoLineMax = 120.0f;
constexpr float kOneLineMin = 40.0f, kOneLineMax = 60.0f;
constexpr std::size_t kMaxColumns = 2;
// The most rows shown. Two columns of 16 rows are 45 high; a 17th row each would be 42, and a third column 165 wide,
// neither of which holds a 16-point name. A room larger than this shows its first kMaxRows members.
constexpr std::size_t kMaxRows = 32;

enum class Mode { Stock, TwoLine, OneLine, Narrow };

struct Plan {
    Mode mode = Mode::Stock;
    std::size_t members = 0;    // panels the game built
    std::size_t shown = 0;      // rows laid out; panels from here on are hidden
    std::size_t columns = 1;
    std::size_t perColumn = 0;  // rows in a full column
    float rowWidth = kPanelWidth;
    float rowHeight = 0.0f;     // 0 in Stock: the game's own
};

constexpr float Floor(float v) { return v <= 0.0f ? 0.0f : static_cast<float>(static_cast<long long>(v)); }
constexpr float Smaller(float a, float b) { return a < b ? a : b; }
// The height of each of `rows` rows that fill the group's height with kRowGap between them.
constexpr float FitHeight(std::size_t rows) {
    return rows ? Floor((kGroupHeight - kRowGap * static_cast<float>(rows - 1)) / static_cast<float>(rows)) : kGroupHeight;
}

constexpr Plan PlanRows(std::size_t members) {
    Plan p{};
    p.members = members;
    p.shown = members;
    p.perColumn = members;
    if (members <= kStockRows) return p;
    p.shown = members < kMaxRows ? members : kMaxRows;
    p.perColumn = p.shown;
    const float h = FitHeight(p.shown);
    if (h >= kTwoLineMin) {
        p.mode = Mode::TwoLine;
        p.rowHeight = Smaller(h, kTwoLineMax);
        return p;
    }
    if (h >= kOneLineMin) {
        p.mode = Mode::OneLine;
        p.rowHeight = Smaller(h, kOneLineMax);
        return p;
    }
    p.mode = Mode::Narrow;
    p.columns = kMaxColumns;
    p.perColumn = (p.shown + kMaxColumns - 1) / kMaxColumns;
    p.rowWidth = Floor((kGroupWidth - kColumnGap * static_cast<float>(kMaxColumns - 1)) / static_cast<float>(kMaxColumns));
    p.rowHeight = Smaller(FitHeight(p.perColumn), kOneLineMax);
    return p;
}

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// Row `index`'s place in the group; false for Stock (the game's) and for a row past `shown` (hidden).
constexpr bool RowRect(const Plan& p, std::size_t index, Rect& out) {
    if (p.mode == Mode::Stock || index >= p.shown || !p.perColumn) return false;
    const std::size_t column = index / p.perColumn, row = index % p.perColumn;
    out = {static_cast<float>(column) * (p.rowWidth + kColumnGap), static_cast<float>(row) * (p.rowHeight + kRowGap),
           p.rowWidth, p.rowHeight};
    return true;
}

// A panel's children, by their place in the layout tree (the game's ordinal, component +0x110). Button_Master has
// _pl_RoomMaster at 13 and its four voice chat icons at 14..17; Button_Member has the icons at 13..16.
enum class Role { Class, Name, ArmorDesc, Armor, RatioDesc, Ratio, Weapon, Location, MasterMark, Voice, Other };
constexpr Role RoleOf(int ordinal, bool master) {
    switch (ordinal) {
        case 0: return Role::Class;
        case 1: return Role::Name;
        case 2: return Role::ArmorDesc;
        case 3: return Role::Armor;
        case 4: return Role::RatioDesc;
        case 5: return Role::Ratio;
        case 12: return Role::Location;
        default: break;
    }
    if (ordinal >= 6 && ordinal <= 11) return Role::Weapon;
    if (master && ordinal == 13) return Role::MasterMark;
    const int firstIcon = master ? 14 : 13;
    return ordinal >= firstIcon && ordinal < firstIcon + 4 ? Role::Voice : Role::Other;
}

// The stock geometry of each child (position in the panel, area), for the tests and for "keep".
constexpr Rect StockRect(Role r) {
    switch (r) {
        case Role::Class: return {11, 9, 149, 35};
        case Role::Name: return {166, 9, 250, 35};
        case Role::ArmorDesc: return {332, 53, 106, 19};
        case Role::Armor: return {332, 85, 117, 23};
        case Role::RatioDesc: return {332, 122, 104, 19};
        case Role::Ratio: return {332, 151, 119, 23};
        case Role::Weapon: return {9, 51, 316, 21};  // _pl_Weapon0; the others 22 apart below it
        case Role::Location: return {92.5f, 58, 270, 112};
        case Role::MasterMark: return {420, -20, 48, 30};
        case Role::Voice: return {417, 9, 36, 36};
        default: return {};
    }
}
// The faction label: a copy of _pl_Name (TextField 250 x 35, 16 point) the plugin adds to each panel.
constexpr float kLabelHeight = 35.0f;

// Keep leaves the child's visibility to the game: the voice icons (one of four is shown, by the game, each frame), the
// host's mark and the member status. Show restores one a label had taken the place of.
enum class Shown { Keep, Show, Hide };
struct Place {
    bool move = false;    // x, y below; otherwise where the game put it
    float x = 0.0f, y = 0.0f;
    float width = 0.0f;   // a TextField's new area; 0 keeps that side
    float height = 0.0f;
    float scale = 0.0f;   // uniform scale of the child; 0 keeps it
    Shown shown = Shown::Keep;
};

constexpr Place At(float x, float y, float width = 0.0f, float height = 0.0f) {
    Place p{};
    p.move = true;
    p.x = x;
    p.y = y;
    p.width = width;
    p.height = height;
    return p;
}
constexpr Place Showing(Place p) {
    p.shown = Shown::Show;
    return p;
}
constexpr Place Hidden() {
    Place p{};
    p.shown = Shown::Hide;
    return p;
}
constexpr Place Visible() {
    Place p{};
    p.shown = Shown::Show;
    return p;
}
constexpr float CentreY(const Plan& p, float height) { return Floor((p.rowHeight - height) / 2.0f); }
// Narrow rows: the name above, the label below, each in a text area of kLineHeight (16-point text) with kLineGap
// between them, the pair centred in the row.
constexpr float kLineHeight = 20.0f, kLineGap = 2.0f;
constexpr float UpperLine(const Plan& p) { return CentreY(p, 2 * kLineHeight + kLineGap); }
constexpr float LowerLine(const Plan& p) { return UpperLine(p) + kLineHeight + kLineGap; }

// Where a child goes. label: this row shows a faction label now (the children it takes the place of are hidden).
constexpr Place PlaceChild(const Plan& p, Role r, bool label) {
    switch (p.mode) {
        case Mode::Stock:
            // The game's layout; a faction label takes the clear rate's place (a PvP room has no use for it).
            if (r == Role::RatioDesc || r == Role::Ratio) return label ? Hidden() : Visible();
            return {};
        case Mode::TwoLine:
            switch (r) {
                case Role::Class:
                case Role::Name:
                case Role::MasterMark:
                case Role::Voice: return {};
                case Role::Armor: return At(11, 52);
                case Role::Location: {  // the member's status (equipping, ready) at 0.4 below the voice icon
                    Place l = At(300, p.rowHeight - 47);
                    l.scale = 0.4f;
                    return l;
                }
                default: return Hidden();
            }
        case Mode::OneLine:
            switch (r) {
                case Role::Class: return At(8, CentreY(p, 35), 104);
                case Role::Name: return At(116, CentreY(p, 35), 170);
                case Role::Armor: return label ? Hidden() : Showing(At(296, CentreY(p, 23)));
                case Role::Voice: return At(417, CentreY(p, 36));
                case Role::MasterMark: return {};
                default: return Hidden();
            }
        case Mode::Narrow:
            switch (r) {
                case Role::Name:
                    return label ? At(8, UpperLine(p), p.rowWidth - 52, kLineHeight) : At(8, CentreY(p, 35), p.rowWidth - 52, 35);
                case Role::Voice: return At(p.rowWidth - 40, CentreY(p, 36));
                case Role::MasterMark: return At(p.rowWidth - 35, -20);
                default: return Hidden();
            }
    }
    return {};
}

// The faction label's place in its row.
constexpr Place PlaceLabel(const Plan& p) {
    switch (p.mode) {
        case Mode::Stock: return At(332, 122, 121);
        case Mode::TwoLine: return At(136, 47, 150);
        case Mode::OneLine: return At(290, CentreY(p, kLabelHeight), 122);
        case Mode::Narrow: return At(8, LowerLine(p), p.rowWidth - 52, kLineHeight);
    }
    return {};
}

}  // namespace multislot::rows
