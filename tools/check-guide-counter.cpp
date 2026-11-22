// SPDX-License-Identifier: MIT
// Fix 115d (tools/source-1.0.2/fixes/115d-guide-rows-counter.patch): two notes from the user (Sims 3
// on 1.0.5-work-fix115b): the guide's two lists showed a different number of rows, and the "1 / 7"
// counter (small, grey, the settings page only) was hard to see, so it was not clear how many rows
// a list has.
//  (a) guide_quick::Window: the same window on both tabs, kGuideRows slots, centred on the focused
//      row while it can be, never past either end; fix 93d's button in a slot of its own after the
//      rows, scrolling with them; "more above" and "more below" exactly when rows are off screen;
//  (b) the guide in the assembled tree: both tabs through Window with kGuideRows; the button drawn
//      in its slot only while it is on screen; a shadow at the list's top or bottom edge while it has
//      more rows that way; "row / rows" (or "- / rows" while the tab bar has the pad) on the tab bar
//      left of R1, 26 px, light on the dark bar, the same place on both tabs, after an up and a down
//      arrow lit while there are more rows that way; the old small grey counter gone.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/guide_quick.hpp"

namespace {
namespace fs = std::filesystem;
using xbox360ps5::guide_quick::Window;

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
std::string Case(int row, int total, int listed) {
  return " (row " + std::to_string(row) + " of " + std::to_string(total) + ", " + std::to_string(listed) + " listed)";
}

void CheckWindow() {
  const int shown = 6;
  // Every list from 1 to 20 rows, with and without the button, every focused row.
  for (int listed = 1; listed <= 20; ++listed) {
    for (const bool button : {false, true}) {
      const int total = listed + (button ? 1 : 0);
      for (int row = 0; row < total; ++row) {
        const auto w = Window(row, total, listed, shown);
        const std::string c = Case(row, total, listed);
        Expect(w.first >= 0 && w.first <= (total > shown ? total - shown : 0), "(a) the window never past either end" + c);
        Expect(row >= w.first && row < w.first + shown, "(a) the focused row (or button) is on screen" + c);
        Expect(w.last == std::min(listed, w.first + shown), "(a) the rows on screen" + c);
        Expect(std::min(total, shown) == (w.last - w.first) + (w.button_shown ? 1 : 0), "(a) the slots are filled" + c);
        Expect(w.button_shown == (button && listed >= w.first && listed < w.first + shown), "(a) the button in its own slot" + c);
        Expect(w.more_above == (w.first > 0) && w.more_below == (w.first + shown < total), "(a) more above / below" + c);
        if (total > shown && row >= shown / 2 && row <= total - shown + shown / 2)
          Expect(w.first == row - shown / 2, "(a) centred on the focused row while it can be" + c);
      }
    }
  }
  // The guide's tabs: Game with BACK (7 rows) and Settings' System category with the button.
  auto game = Window(0, 7, 7, shown);
  Expect(game.first == 0 && game.last == 6 && !game.more_above && game.more_below, "(a) Game, 7 rows: 6 shown, more below");
  game = Window(6, 7, 7, shown);
  Expect(game.first == 1 && game.last == 7 && game.more_above && !game.more_below, "(a) Game, last row: scrolled by one");
  auto system = Window(0, 7, 6, shown);
  Expect(system.last == 6 && !system.button_shown && system.more_below, "(a) System: six options, the button below them");
  system = Window(6, 7, 6, shown);
  Expect(system.first == 1 && system.button_shown, "(a) System, the button focused: on screen in its slot");
  const auto short_list = Window(2, 5, 4, shown);
  Expect(short_list.first == 0 && short_list.last == 4 && short_list.button_shown && !short_list.more_above &&
             !short_list.more_below,
         "(a) a short list: all of it, no arrows");
}

void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  Expect(Has(main, "const int total = int(items.size());") && Has(main, "const int shown_rows = kGuideRows;") &&
             Has(main, "xbox360ps5::guide_quick::Window(state_.row, total, listed, shown_rows);"),
         "(b) both tabs through Window, kGuideRows slots");
  Expect(Has(main, "constexpr int kGuideRows = 6;"), "(b) six slots below the tab bar (410 px: six of 63 px)");
  Expect(!Has(main, "state_.page == 0 ? std::clamp(") && !Has(main, "const int shown_rows = button ? kGuideRows - 1 : kGuideRows;"),
         "(b) no tab with a window of its own");
  const std::string button = Between(main, "    if (button_shown) {", "list->PopClipRect();");
  Expect(Has(button, "const float y = list_top + (listed - first_row) * 63.0f + 4;"), "(b) the button in its slot, while on screen");
  Expect(Has(main, "if (more_above)\n      list->AddRectFilledMultiColor(at(left, list_top), at(right, list_top + 22)") &&
             Has(main, "if (more_below)\n      list->AddRectFilledMultiColor(at(left, bottom - 22), at(right, bottom)"),
         "(b) a shadow at the edge that has more rows");
  const std::string counter = Between(main, "char counter[32];", "// What the buttons do");
  Expect(Has(counter, "if (row_focus) std::snprintf(counter, sizeof(counter), \"%d / %d\", state_.row + 1, total);") &&
             Has(counter, "else std::snprintf(counter, sizeof(counter), \"- / %d\", total);"),
         "(b) \"row / rows\", \"- / rows\" while the bar has the pad");
  Expect(Has(counter, "medium->CalcTextSizeA(26 * scale, 4096.0f, 0.0f, counter);") &&
             Has(counter, "fade(236, 239, 243), counter);") && Has(counter, "const float mid = top + tb::kBarHeight / 2;"),
         "(b) 26 px, light, on the tab bar");
  Expect(Has(main, "counter_right = tb::Draw(painter, left, right, top, tabs, state_.page, state_.on_bar).r1.x - 20;") &&
             Has(counter, "const float text_x = counter_right * scale - size.x;"),
         "(b) left of R1's pill, the same place on both tabs");
  Expect(Has(counter, "more_above ? fade(236, 239, 243) : fade(236, 239, 243, 60)") &&
             Has(counter, "more_below ? fade(236, 239, 243) : fade(236, 239, 243, 60)"),
         "(b) up and down arrows lit while there are more rows that way");
  Expect(!Has(main, "(bottom - 44) * scale),\n                    fade(96, 102, 110), position);"), "(b) the old faint counter is gone");
}

}  // namespace

int main() {
  CheckWindow();
  CheckGuide();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-guide-counter: all checks passed\n");
  return 0;
}
