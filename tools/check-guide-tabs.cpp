// SPDX-License-Identifier: MIT
// Fix 115c (tools/source-1.0.2/fixes/115c-guide-tab-bar.patch): the in-game guide's pages become a
// tab bar along the panel's top (reconstructed/xbox360ps5/tab_bar.hpp, shared with the library's game
// sheet), the user's choice after the console run of 1.0.5-work-fix115b (research/fix115b-guide/):
// L1 and R1 were small, low-contrast and far from the side tabs, and left and right on the first
// page switched pages while on the settings page they changed options ("I change an option by
// accident when I want to change the tab").
//  (a) the pad (tab_bar::Press): L1 and R1 always switch tabs, from the bar or the list, and land on
//      the bar with no row focused; on the bar left and right switch tabs, Down and Cross enter the
//      list at its first row, Up does nothing; in the list Up on the first row goes to the bar, Down
//      stops at the last row, left and right change only a row that has a value (never another row,
//      never the tab), Cross activates; Circle is "back" everywhere; a new Nav starts on the bar, so
//      the first press after a tab opens never changes a value (every sequence of presses checked);
//  (b) the look (tab_bar::Draw with a recording painter): white pills at the bar's two ends with a
//      near-black name and an arrow pointing out ("<" left of L1, ">" right of R1), 38 px high,
//      inside the bar; the tabs between them, in order, full names; the open tab white with a green
//      underline, the others grey; with the bar focused the open tab on the green instead;
//      DrawValue: "< value >" only on a row with a value;
//  (c) the guide in the assembled tree: the bar instead of the side blades, the list below it, the
//      pad through tab_bar::Press, the guide opening with the bar focused, no row lit while the bar
//      has the pad, value rows (options, network) with arrows, no L1 or R1 in the legend.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/tab_bar.hpp"

namespace {
namespace fs = std::filesystem;
namespace tb = xbox360ps5::tab_bar;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string s = text.str(), out;
  out.reserve(s.size());
  for (char c : s) if (c != '\r') out += c;  // Some files of the tree have CR LF lines.
  return out;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}

// (a) ------------------------------------------------------------------------------------------
// The guide's two tabs: the first with 7 rows (only row 4, the mute row, has a value), the second
// with 6 (rows 0, 2 and 5 have one).
struct Model {
  std::vector<std::vector<bool>> values = {{false, false, false, false, true, false, false},
                                           {true, false, true, false, false, true}};
  tb::Nav nav;
  tb::Effect Press(tb::Key key) {
    const auto& rows = values[size_t(nav.tab)];
    return tb::Press(nav, key, int(values.size()), int(rows.size()), [&rows](int row) { return bool(rows[size_t(row)]); });
  }
};

void CheckPad() {
  using K = tb::Key;
  {
    Model m;
    Expect(m.nav.on_bar && m.nav.tab == 0, "(a) a new tab bar starts on the bar, first tab");
    auto e = m.Press(K::right);
    Expect(e.tab_changed && m.nav.tab == 1 && m.nav.on_bar && !e.value_step, "(a) right on the bar: the next tab");
    e = m.Press(K::left);
    Expect(e.tab_changed && m.nav.tab == 0 && m.nav.on_bar, "(a) left on the bar: the previous tab");
    e = m.Press(K::left);
    Expect(e.tab_changed && m.nav.tab == 1, "(a) ... wrapping, as L1 and R1 did between the two pages");
    e = m.Press(K::up);
    Expect(!e.tab_changed && m.nav.on_bar && !e.activate && !e.value_step, "(a) Up on the bar does nothing");
    e = m.Press(K::down);
    Expect(!m.nav.on_bar && m.nav.row == 0 && !e.value_step && !e.activate, "(a) Down: the list's first row");
    e = m.Press(K::right);
    Expect(e.value_step == +1 && !e.tab_changed && m.nav.tab == 1, "(a) right on a row with a value: +1, the tab stays");
    e = m.Press(K::left);
    Expect(e.value_step == -1 && !e.tab_changed, "(a) left on a row with a value: -1");
    m.Press(K::down);
    e = m.Press(K::left);
    Expect(!e.value_step && !e.tab_changed && m.nav.row == 1 && !m.nav.on_bar, "(a) left on a row without a value: nothing");
    e = m.Press(K::right);
    Expect(!e.value_step && !e.tab_changed, "(a) right on a row without a value: nothing");
    e = m.Press(K::cross);
    Expect(e.activate && !e.value_step, "(a) Cross on a row: activate");
    e = m.Press(K::up);
    Expect(!m.nav.on_bar && m.nav.row == 0, "(a) Up: the row above");
    e = m.Press(K::up);
    Expect(m.nav.on_bar && m.nav.tab == 1 && !e.tab_changed, "(a) Up on the first row: the bar, same tab");
    m.Press(K::cross);
    Expect(!m.nav.on_bar && m.nav.row == 0, "(a) Cross on the bar: the list's first row");
    for (int n = 0; n < 10; ++n) m.Press(K::down);
    Expect(m.nav.row == 5 && !m.nav.on_bar, "(a) Down stops at the last row (no wrap to the top)");
    e = m.Press(K::l1);
    Expect(e.tab_changed && m.nav.tab == 0 && m.nav.on_bar && m.nav.row == 0, "(a) L1 from the list: the other tab, bar focused");
    m.Press(K::down);
    e = m.Press(K::r1);
    Expect(e.tab_changed && m.nav.tab == 1 && m.nav.on_bar, "(a) R1 from the list: the other tab, bar focused");
    e = m.Press(K::circle);
    Expect(e.back && !e.tab_changed && !e.activate, "(a) Circle on the bar: back");
    m.Press(K::down);
    e = m.Press(K::circle);
    Expect(e.back, "(a) Circle in the list: back");
  }
  {
    // A tab with no rows: Down stays on the bar; one tab: L1 and R1 do nothing.
    tb::Nav nav;
    auto none = [](int) { return true; };
    tb::Press(nav, tb::Key::down, 2, 0, none);
    Expect(nav.on_bar, "(a) Down on an empty tab stays on the bar");
    const auto e = tb::Press(nav, tb::Key::r1, 1, 3, none);
    Expect(!e.tab_changed && nav.tab == 0, "(a) one tab: R1 does nothing");
  }
  // Every sequence of up to 5 presses from a freshly opened tab: a value changes only while a row
  // with a value is focused and left or right was pressed, never on the first press, and a tab
  // changes only by L1, R1, or left/right on the bar.
  const K keys[] = {K::up, K::down, K::left, K::right, K::l1, K::r1, K::cross};
  int sequences = 0;
  std::function<void(Model, int, bool)> walk = [&](Model m, int depth, bool first) {
    if (depth == 0) return;
    for (const K key : keys) {
      Model next = m;
      const bool on_bar = next.nav.on_bar;
      const int row = next.nav.row, tab = next.nav.tab;
      const auto e = next.Press(key);
      ++sequences;
      if (e.value_step) {
        Expect(!first, "(a) the first press after a tab opens never changes a value");
        Expect(!on_bar && (key == K::left || key == K::right) && m.values[size_t(tab)][size_t(row)],
               "(a) a value changes only by left/right on a focused row that has one");
      }
      if (e.tab_changed)
        Expect(key == K::l1 || key == K::r1 || (on_bar && (key == K::left || key == K::right)),
               "(a) a tab changes only by L1, R1, or left/right on the bar");
      if (e.tab_changed) Expect(next.nav.on_bar, "(a) a tab opens with the bar focused");
      walk(next, depth - 1, e.tab_changed);
    }
  };
  walk(Model{}, 5, true);
  Expect(sequences > 10000, "(a) the sequences were walked");
}

// (b) ------------------------------------------------------------------------------------------
struct Op {
  std::string kind, text;
  float x = 0, y = 0, w = 0, h = 0;
  uint32_t rgb = 0;
  float round = 0;
  float ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
};
struct Recorder {
  std::vector<Op> ops;
  void Fill(float x, float y, float w, float h, uint32_t rgb, float, float round) {
    Op op; op.kind = "fill"; op.x = x; op.y = y; op.w = w; op.h = h; op.rgb = rgb; op.round = round; ops.push_back(op);
  }
  void Text(const char* text, float x, float y, float size, uint32_t rgb, float) {
    Op op; op.kind = "text"; op.text = text; op.x = x; op.y = y; op.h = size; op.rgb = rgb; ops.push_back(op);
  }
  float Width(const char* text, float size) { return float(std::string(text).size()) * size * 0.5f; }
  void Triangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgb, float) {
    Op op; op.kind = "triangle"; op.ax = ax; op.ay = ay; op.bx = bx; op.by = by; op.cx = cx; op.cy = cy; op.rgb = rgb;
    ops.push_back(op);
  }
  const Op* TextOf(const std::string& text) const {
    for (const Op& op : ops) if (op.kind == "text" && op.text == text) return &op;
    return nullptr;
  }
};

void CheckLook() {
  const float x0 = 412, x1 = 1508, y = 304;
  const std::vector<std::string> tabs = {"Gra", "Ustawienia"};
  Recorder r;
  const tb::Layout layout = tb::Draw(r, x0, x1, y, tabs, 0, false);
  // Pills.
  Expect(layout.l1.x >= x0 && layout.l1.x - x0 <= 16 && x1 - (layout.r1.x + layout.r1.w) <= 16 && layout.r1.x + layout.r1.w <= x1,
         "(b) L1 at the bar's left end, R1 at its right end");
  Expect(layout.l1.h == 38 && layout.r1.h == 38 && layout.l1.y >= y && layout.l1.y + layout.l1.h <= y + tb::kBarHeight,
         "(b) the pills are 38 px high, inside the bar");
  int pills = 0;
  for (const Op& op : r.ops)
    if (op.kind == "fill" && op.rgb == 0xF5F6F8 && op.round == op.h / 2) ++pills;
  Expect(pills == 2, "(b) two white pills with fully round ends");
  const Op* l1 = r.TextOf("L1");
  const Op* r1 = r.TextOf("R1");
  Expect(l1 && r1 && l1->rgb == 0x1E2125 && r1->rgb == 0x1E2125, "(b) their names near-black");
  std::vector<Op> arrows;
  for (const Op& op : r.ops) if (op.kind == "triangle") arrows.push_back(op);
  Expect(arrows.size() == 2, "(b) one arrow per pill");
  if (arrows.size() == 2 && l1 && r1) {
    Expect(arrows[0].ax < arrows[0].bx && arrows[0].bx < l1->x && arrows[0].ax >= layout.l1.x,
           "(b) \"<\" before L1, pointing left, inside its pill");
    Expect(arrows[1].ax > arrows[1].bx && arrows[1].bx > r1->x && arrows[1].ax <= layout.r1.x + layout.r1.w,
           "(b) \">\" after R1, pointing right, inside its pill");
  }
  // Tabs.
  const Op* game = r.TextOf("Gra");
  const Op* settings = r.TextOf("Ustawienia");
  Expect(game && settings && layout.l1.x + layout.l1.w < game->x && game->x < settings->x &&
             settings->x + 10 * 28 * 0.5f < layout.r1.x,
         "(b) the tabs between the pills, in order, full names");
  Expect(game && settings && game->rgb == 0xFFFFFF && settings->rgb == 0xA9AFB7, "(b) the open tab white, the other grey");
  int underlines = 0, green_fills = 0;
  for (const Op& op : r.ops) {
    if (op.kind != "fill" || op.rgb != 0x76BC3E) continue;
    if (op.h == tb::kUnderline) ++underlines; else ++green_fills;
    if (game) Expect(op.x <= game->x && op.x + op.w >= game->x + 3 * 28 * 0.5f, "(b) the green under the open tab");
  }
  Expect(underlines == 1 && green_fills == 0, "(b) the list has the pad: a green underline under the open tab");
  Recorder focused;
  tb::Draw(focused, x0, x1, y, tabs, 1, true);
  underlines = green_fills = 0;
  for (const Op& op : focused.ops)
    if (op.kind == "fill" && op.rgb == 0x76BC3E) (op.h == tb::kUnderline ? underlines : green_fills)++;
  const Op* open = focused.TextOf("Ustawienia");
  Expect(underlines == 0 && green_fills == 1 && open && open->rgb == 0xFFFFFF, "(b) the bar has the pad: the open tab on the green");
  // Values.
  Recorder plain, arrowed;
  tb::DrawValue(plain, "Off", 1484, 400, 28, 0x5A6068, 1, false);
  tb::DrawValue(arrowed, "Off", 1484, 400, 28, 0x5A6068, 1, true);
  int plain_arrows = 0, value_arrows = 0;
  for (const Op& op : plain.ops) plain_arrows += op.kind == "triangle";
  for (const Op& op : arrowed.ops) value_arrows += op.kind == "triangle";
  const Op* value = arrowed.TextOf("Off");
  Expect(plain_arrows == 0 && plain.TextOf("Off") && plain.TextOf("Off")->x + 3 * 28 * 0.5f == 1484,
         "(b) a row without a value to change: the text alone, right-aligned");
  Expect(value_arrows == 2 && value && arrowed.ops.front().ax == 1484 && arrowed.ops.back().ax < value->x,
         "(b) a row with a value: \"< value >\", \">\" at the right edge");
}

// (c) ------------------------------------------------------------------------------------------
void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  Expect(Has(main, "#include \"xbox360ps5/tab_bar.hpp\""), "(c) the guide uses tab_bar.hpp");
  Expect(Has(main, "bool on_bar = true;  // Fix 115c"), "(c) the guide's state knows whether the bar has the pad");
  const std::string has_value = Between(main, "inline bool GuideRowHasValue(const GuideItem& item) {", "}");
  Expect(Has(has_value, "item.action == GuideAction::option || item.action == GuideAction::network"),
         "(c) options and the network row have a value; actions and categories do not");
  // Drawing.
  const std::string draw = Between(main, "// Fix 115c: the pages are tabs on a bar", "const auto items = GuideItems(state_.page, settings_, state_.category);");
  Expect(Has(draw, "const float list_top = top + tb::kBarHeight;") &&
             Has(draw, "std::vector<std::string> tabs = {Tr(kGuidePages[0]), Tr(kGuidePages[1])};") &&
             Has(draw, "tb::Draw(painter, left, right, top, tabs, state_.page, state_.on_bar)") &&
             Has(draw, "list->PushClipRect(at(left, list_top), at(right, bottom), true);"),
         "(c) the tab bar along the panel's top, the list below it");
  Expect(!Has(main, "Sideways(list, medium, 28 * scale, at(other_left + blade - 14, top + 24)") &&
             !Has(main, "const float strip = 94;"),
         "(c) the side blades are gone");
  Expect(Has(main, "const float focus_target = list_top + ") && Has(main, "const float y = list_top + (n - first_row) * 63.0f;") &&
             Has(main, "const bool focused = row_focus && n == state_.row;") &&
             Has(main, "if (row_focus && state_.row >= first_row && state_.row < last_row)"),
         "(c) rows below the bar; none lit while the bar has the pad");
  Expect((Has(main, "const int shown_rows = button ? kGuideRows - 1 : kGuideRows;") &&
              Has(main, "list_top + std::min(listed - first_row, shown_rows) * 63.0f + 24")) ||
             // Fix 115d (check-guide-counter.cpp): the button in a slot of its own.
             (Has(main, "const int shown_rows = kGuideRows;") && Has(main, "list_top + (listed - first_row) * 63.0f + 4")),
         "(c) six rows below the bar (five above fix 93d's button), both pages scroll");
  Expect(Has(main, "tb::DrawValue(painter, items[size_t(n)].value.c_str(), page_right - 24 + slide_x, y + 16, 28,") &&
             Has(main, "GuideRowHasValue(items[size_t(n)]));"),
         "(c) \"< value >\" on the rows that have one");
  const std::string legend = Between(main, "// What the buttons do, in the pad's own colours.", "kBuildVersionLabel");
  Expect(!legend.empty() && !Has(legend, "\"L1") && !Has(legend, "\"R1\"") && !Has(legend, "Shoulder("),
         "(c) no L1 or R1 in the legend");
  // The pad.
  const std::string pad = Between(main, "tb::Nav nav{guide.page, guide.on_bar, guide.row};", "const int step = effect.value_step < 0 ? -1 : 1;");
  Expect(Has(pad, "{0x400, tb::Key::l1},  {0x800, tb::Key::r1},   {0x2000, tb::Key::circle}, {0x10, tb::Key::up},") &&
             Has(pad, "{0x40, tb::Key::down}, {0x80, tb::Key::left}, {0x20, tb::Key::right},    {0x4000, tb::Key::cross}};") &&
             Has(pad, "effect = tb::Press(nav, key, 2, rows, [&items](int row) { return GuideRowHasValue(items[size_t(row)]); });") &&
             Has(pad, "guide.on_bar = nav.on_bar;") && Has(pad, "guide.row = nav.row;"),
         "(c) the pad's keys through tab_bar::Press");
  Expect(Has(pad, "if (effect.tab_changed) {") && Has(pad, "guide.page = nav.tab; guide.row = 0; guide.category = -1;") &&
             Has(pad, "else if ((effect.activate || effect.value_step != 0) && guide.row < rows) {") &&
             Has(pad, "} else if (effect.back) {"),
         "(c) a tab change, an activation or a value step, and back");
  Expect(!Has(main, "if ((pressed & 0xC00) || (guide.page == 0 && (pressed & 0xA0))) {") &&
             !Has(main, "guide.row = (guide.row + rows - 1) % rows;"),
         "(c) left and right no longer switch pages from the list; Up no longer wraps to the bottom");
  Expect(Has(main, "guide.on_bar = true;  // fix 115c: no row focused yet"), "(c) the guide opens with the bar focused");
}

}  // namespace

int main() {
  CheckPad();
  CheckLook();
  CheckGuide();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-guide-tabs: all checks passed\n");
  return 0;
}
