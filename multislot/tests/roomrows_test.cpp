// The room screen's member rows (roomrows.h), without the game: for every room size the rows fit PlayersGroup, never
// overlap, keep the host's panel first, and what each row shows fits that row without overlapping.
#include <cstdio>
#include <string>
#include <vector>

#include "../src/roomrows.h"

using namespace multislot::rows;

namespace {

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool Overlap(const Rect& a, const Rect& b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
bool Inside(const Rect& in, const Rect& r) {
    return r.x >= in.x && r.y >= in.y && r.x + r.w <= in.x + in.w + 0.001f && r.y + r.h <= in.y + in.h + 0.001f;
}

// Where a child ends up in its row, as roomview.cpp applies a Place: moved or the game's, its area replaced side by
// side, then scaled.
Rect Placed(Role role, const Place& p) {
    Rect r = StockRect(role);
    if (p.move) {
        r.x = p.x;
        r.y = p.y;
    }
    if (p.width > 0) r.w = p.width;
    if (p.height > 0) r.h = p.height;
    if (p.scale > 0) {
        r.w *= p.scale;
        r.h *= p.scale;
    }
    return r;
}

struct Drawn {
    std::string what;
    Rect rect;
};

// What a row shows, in row coordinates. The four voice icons share one place (the game shows one of them at a time);
// the host's mark hangs over the panel's top edge as the game draws it, so it is left out of the fit.
std::vector<Drawn> RowContents(const Plan& plan, bool master, bool label) {
    std::vector<Drawn> drawn;
    bool voice = false;
    for (int ordinal = 0; ordinal < (master ? 18 : 17); ++ordinal) {
        const Role role = RoleOf(ordinal, master);
        const Place p = PlaceChild(plan, role, label);
        if (p.shown == Shown::Hide || role == Role::MasterMark) continue;
        if (role == Role::Voice) {
            if (voice) continue;
            voice = true;
        }
        Rect r = Placed(role, p);
        if (role == Role::Weapon) r.y += 22.0f * static_cast<float>(ordinal - 6);
        drawn.push_back({"child " + std::to_string(ordinal), r});
    }
    if (label) {
        const Place p = PlaceLabel(plan);
        drawn.push_back({"faction label", Placed(Role::Name, p)});  // a copy of _pl_Name
    }
    return drawn;
}

}  // namespace

int main() {
    // Four or fewer: the game's own layout, nothing moved.
    for (std::size_t n = 0; n <= kStockRows; ++n) {
        const Plan p = PlanRows(n);
        Rect r{};
        Check(p.mode == Mode::Stock && p.shown == n && !RowRect(p, 0, r), "members " + std::to_string(n) + ": the game's layout");
        for (int ordinal = 0; ordinal < 18; ++ordinal) {
            const Role role = RoleOf(ordinal, true);
            const Place keep = PlaceChild(p, role, false);
            Check(!keep.move && keep.width == 0 && keep.scale == 0 &&
                      keep.shown == (role == Role::RatioDesc || role == Role::Ratio ? Shown::Show : Shown::Keep),
                  "stock rows keep every child where the game put it");
        }
        // A faction label takes the clear rate's place, nothing else.
        Check(PlaceChild(p, Role::Ratio, true).shown == Shown::Hide && PlaceChild(p, Role::RatioDesc, true).shown == Shown::Hide &&
                  PlaceChild(p, Role::Armor, true).shown == Shown::Keep,
              "a stock row with a label hides the clear rate only");
        const Rect label = Placed(Role::Name, PlaceLabel(p));
        Check(Inside({0, 0, kPanelWidth, kMemberHeight}, label) && !Overlap(label, StockRect(Role::Armor)) &&
                  !Overlap(label, StockRect(Role::Name)) && !Overlap(label, StockRect(Role::Voice)),
              "the stock label fits a member panel beside the armor");
    }
    // The stock layout itself fits: one host panel and three member panels.
    Check(kMasterHeight + 3 * kMemberHeight <= kGroupHeight, "four stock panels fit PlayersGroup");

    std::size_t lastShown = 0;
    for (std::size_t n = kStockRows + 1; n <= 1024; n = n < 40 ? n + 1 : n * 2) {
        const Plan p = PlanRows(n);
        const std::string at = "members " + std::to_string(n) + ": ";
        Check(p.mode != Mode::Stock, at + "laid out by the plugin");
        Check(p.shown == (n < kMaxRows ? n : kMaxRows), at + "rows shown up to the cap");
        Check(p.shown >= lastShown, at + "more members never show fewer rows");
        lastShown = p.shown;
        Check(p.columns >= 1 && p.columns <= kMaxColumns && p.perColumn * p.columns >= p.shown, at + "columns hold every row");
        const float least = p.mode == Mode::TwoLine ? kTwoLineMin : p.mode == Mode::OneLine ? kOneLineMin : 45.0f;
        Check(p.rowHeight >= least, at + "rows are high enough to read (" + std::to_string(p.rowHeight) + ")");
        Check((p.mode == Mode::Narrow) == (p.columns == 2), at + "only the narrow rows use a second column");
        std::vector<Rect> placed;
        for (std::size_t i = 0; i < n; ++i) {
            Rect r{};
            const bool shown = RowRect(p, i, r);
            Check(shown == (i < p.shown), at + "row " + std::to_string(i) + " shown below the cap only");
            if (!shown) continue;
            Check(Inside({0, 0, kGroupWidth, kGroupHeight}, r), at + "row " + std::to_string(i) + " inside PlayersGroup");
            for (std::size_t j = 0; j < placed.size(); ++j)
                Check(!Overlap(placed[j], r), at + "rows " + std::to_string(j) + " and " + std::to_string(i) + " do not overlap");
            if (i) {
                const Rect& prev = placed.back();
                Check(r.x > prev.x || (r.x == prev.x && r.y > prev.y), at + "rows follow the game's order");
            }
            placed.push_back(r);
        }
        Check(!placed.empty() && placed[0].x == 0 && placed[0].y == 0, at + "the host's panel is the first row, top left");
        // What a row shows fits it and does not overlap, host or member, with a label or without.
        for (bool master : {true, false})
            for (bool label : {false, true}) {
                const auto drawn = RowContents(p, master, label);
                const Rect row{0, 0, p.rowWidth, p.rowHeight};
                const std::string which = at + (master ? "host row" : "member row") + (label ? " with a label: " : ": ");
                bool name = false;
                for (std::size_t a = 0; a < drawn.size(); ++a) {
                    Check(Inside(row, drawn[a].rect), which + drawn[a].what + " fits the row");
                    for (std::size_t b = a + 1; b < drawn.size(); ++b)
                        Check(!Overlap(drawn[a].rect, drawn[b].rect), which + drawn[a].what + " and " + drawn[b].what + " do not overlap");
                    name = name || drawn[a].what == "child 1";
                }
                Check(name, which + "the name is always shown");
            }
    }
    // The documented thresholds.
    Check(PlanRows(5).mode == Mode::TwoLine && PlanRows(8).mode == Mode::TwoLine, "5..8 members: two-line rows");
    Check(PlanRows(9).mode == Mode::OneLine && PlanRows(17).mode == Mode::OneLine, "9..17 members: one-line rows");
    Check(PlanRows(18).mode == Mode::Narrow && PlanRows(32).mode == Mode::Narrow && PlanRows(32).rowHeight == 45.0f,
          "18..32 members: two columns, 45 high at 32");
    Check(PlanRows(33).shown == kMaxRows && PlanRows(1024).shown == kMaxRows, "past 32 the first 32 are shown");
    // Children the game names by ordinal.
    Check(RoleOf(13, true) == Role::MasterMark && RoleOf(14, true) == Role::Voice && RoleOf(17, true) == Role::Voice &&
              RoleOf(18, true) == Role::Other && RoleOf(13, false) == Role::Voice && RoleOf(16, false) == Role::Voice &&
              RoleOf(17, false) == Role::Other && RoleOf(6, false) == Role::Weapon && RoleOf(11, true) == Role::Weapon,
          "ordinals of the host's mark and the voice icons");

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("room member rows verified\n");
    return 0;
}
