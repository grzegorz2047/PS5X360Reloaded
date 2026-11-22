// SPDX-License-Identifier: MIT
// Fixes 109d and 115c: one tab bar for the library's game sheet (launcher.cpp, fix 109) and the
// in-game guide (canary_game_main.cpp, fix 115) (the user's choice, "variant C", after the console
// run of 1.0.5-work-fix115b, research/fix115b-guide/g1.png). It lives in the repository
// (tools/source-1.0.2/reconstructed/xbox360ps5, copied into every assembled tree), not in a fix
// patch, so neither fix depends on the other's number: both branches add these same bytes.
//
//   [< L1]   Game   Settings   ...   [R1 >]
//
//  - a horizontal bar at the top of the panel; full tab names; the open tab white with a green
//    underline (the selection colour), the others grey; while the bar has the pad, the open tab
//    sits on the green itself, as a focused row does;
//  - L1 and R1 at the bar's two ends as high-contrast pills (white, near-black name, as tall as the
//    legend's Cross and Circle), with an arrow pointing out of the bar: "<" before L1, ">" after R1;
//  - the pad (Nav::Press): L1 and R1 always switch tabs; in a list, left and right only change the
//    focused row's value (rows that have one show "< value >"), and do nothing on other rows; Up on
//    the first row goes to the bar, where left and right switch tabs and Down (or Cross) goes back
//    to the list; a tab opens with the bar focused, so the first press never changes a value.
//
// Renderer-neutral: the guide draws with ImGui, the launcher with its own canvas. Each passes a
// painter with Fill, Text, Width and Triangle (Draw below); sizes are in 1080p units and colours
// are 0xRRGGBB.
#ifndef XBOX360PS5_TAB_BAR_HPP_
#define XBOX360PS5_TAB_BAR_HPP_

#include <cstdint>
#include <string>
#include <vector>

namespace xbox360ps5::tab_bar {

// The look.
constexpr float kBarHeight = 64;      // the bar's band
constexpr float kPillHeight = 38;     // as tall as the legend's Cross and Circle (30) and a bit more
constexpr float kPillPad = 14;        // inside the pill, each side
constexpr float kPillText = 22;
constexpr float kArrow = 7;           // half the height of an arrow
constexpr float kArrowGap = 8;        // between an arrow and the shoulder's name
constexpr float kEdge = 12;           // from the bar's ends to the pills
constexpr float kTabText = 28;
constexpr float kTabGap = 44;         // between tab names
constexpr float kUnderline = 4;
constexpr uint32_t kPillFill = 0xF5F6F8;
constexpr uint32_t kPillInk = 0x1E2125;
constexpr uint32_t kOpen = 0xFFFFFF;
constexpr uint32_t kClosed = 0xA9AFB7;
constexpr uint32_t kGreen = 0x76BC3E;  // the guide's and the library's focus colour
constexpr uint32_t kGreenDark = 0x4C9222;

// Where things go in a bar from x0 to x1 (top y), given each text's width (Width(text, size)).
struct Box {
  float x = 0, y = 0, w = 0, h = 0;
};
struct Layout {
  Box l1, r1;              // the pills
  std::vector<Box> tabs;   // each tab's name (its text box)
  float tab_text = kTabText;  // the names' size and the gap between them: smaller when they do not
  float tab_gap = kTabGap;    // fit between the pills (seven tabs in the 860 px game sheet)
};
template <typename Width>
Layout Arrange(float x0, float x1, float y, const std::vector<std::string>& tabs, Width&& width) {
  Layout layout;
  const float pill_w_l1 = 2 * kPillPad + 2 * kArrow + kArrowGap + width("L1", kPillText);
  const float pill_w_r1 = 2 * kPillPad + 2 * kArrow + kArrowGap + width("R1", kPillText);
  const float pill_y = y + (kBarHeight - kPillHeight) / 2;
  layout.l1 = {x0 + kEdge, pill_y, pill_w_l1, kPillHeight};
  layout.r1 = {x1 - kEdge - pill_w_r1, pill_y, pill_w_r1, kPillHeight};
  // The names and the gaps shrink together when they do not fit between the pills (which keep
  // their size).
  float names = 0;
  for (const std::string& name : tabs) names += width(name.c_str(), kTabText);
  const float room = layout.r1.x - (layout.l1.x + layout.l1.w);
  const float natural = names + kTabGap * float(tabs.size() + 1);
  const float fit = natural > room && natural > 0 ? room / natural : 1.0f;
  layout.tab_text = kTabText * fit;
  layout.tab_gap = kTabGap * fit;
  float x = layout.l1.x + layout.l1.w + layout.tab_gap;
  for (const std::string& name : tabs) {
    const float w = width(name.c_str(), kTabText) * fit;
    layout.tabs.push_back({x, y + (kBarHeight - layout.tab_text) / 2 - 2, w, layout.tab_text});
    x += w + layout.tab_gap;
  }
  return layout;
}

// Draws the bar's tabs and pills (not its band: each panel paints its own). The painter has
//   void Fill(float x, float y, float w, float h, uint32_t rgb, float alpha, float round);
//   void Text(const char* text, float x, float y, float size, uint32_t rgb, float alpha);  // top left
//   float Width(const char* text, float size);
//   void Triangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgb, float alpha);
template <typename Painter>
Layout Draw(Painter& p, float x0, float x1, float y, const std::vector<std::string>& tabs, int open, bool bar_focused,
            float alpha = 1.0f) {
  const Layout layout = Arrange(x0, x1, y, tabs, [&p](const char* text, float size) { return p.Width(text, size); });
  for (size_t n = 0; n < tabs.size(); ++n) {
    const Box& t = layout.tabs[n];
    const bool is_open = int(n) == open;
    if (is_open && bar_focused) {
      // The bar has the pad: the open tab on the focus colour, as a focused row.
      const float margin = layout.tab_gap / 2 < 16 ? layout.tab_gap / 2 : 16;
      p.Fill(t.x - margin, y + 8, t.w + 2 * margin, kBarHeight - 16, kGreen, alpha, 8);
    } else if (is_open) {
      p.Fill(t.x - 4, y + kBarHeight - 10, t.w + 8, kUnderline, kGreen, alpha, kUnderline / 2);
    }
    p.Text(tabs[n].c_str(), t.x, t.y, layout.tab_text, is_open ? kOpen : kClosed, alpha);
  }
  for (const bool right : {false, true}) {
    const Box& b = right ? layout.r1 : layout.l1;
    const char* name = right ? "R1" : "L1";
    p.Fill(b.x, b.y, b.w, b.h, kPillFill, alpha, b.h / 2);
    const float name_w = p.Width(name, kPillText);
    const float cy = b.y + b.h / 2;
    if (!right) {
      const float ax = b.x + kPillPad;  // "<", then the name
      p.Triangle(ax, cy, ax + 2 * kArrow * 0.8f, cy - kArrow, ax + 2 * kArrow * 0.8f, cy + kArrow, kPillInk, alpha);
      p.Text(name, ax + 2 * kArrow + kArrowGap, cy - kPillText / 2 - 1, kPillText, kPillInk, alpha);
    } else {
      const float nx = b.x + kPillPad;  // the name, then ">"
      p.Text(name, nx, cy - kPillText / 2 - 1, kPillText, kPillInk, alpha);
      const float ax = nx + name_w + kArrowGap;
      p.Triangle(ax + 2 * kArrow * 0.8f, cy, ax, cy - kArrow, ax, cy + kArrow, kPillInk, alpha);
    }
  }
  return layout;
}

// "< value >" on a row whose value left and right change: the value's text right-aligned at
// right_x, the arrows around it. Returns the value's left edge.
template <typename Painter>
float DrawValue(Painter& p, const char* value, float right_x, float y, float size, uint32_t rgb, float alpha,
                bool arrows) {
  const float w = p.Width(value, size);
  const float cy = y + size / 2 + 1;
  if (!arrows) {
    p.Text(value, right_x - w, y, size, rgb, alpha);
    return right_x - w;
  }
  const float a = kArrow * 0.85f, gap = 12;
  const float rx = right_x - 2 * a;  // ">" at the right edge
  p.Triangle(right_x, cy, rx, cy - a, rx, cy + a, rgb, alpha);
  const float vx = rx - gap - w;
  p.Text(value, vx, y, size, rgb, alpha);
  const float lx = vx - gap - 2 * a;  // "<" before the value
  p.Triangle(lx, cy, lx + 2 * a, cy - a, lx + 2 * a, cy + a, rgb, alpha);
  return lx;
}

// The pad.
enum class Key { up, down, left, right, l1, r1, cross, circle };
struct Nav {
  int tab = 0;
  bool on_bar = true;  // a tab opens with the bar focused
  int row = 0;         // the focused row while !on_bar
};
struct Effect {
  bool tab_changed = false;  // the caller shows the new tab (its list from the top, nothing focused)
  int value_step = 0;        // -1 or +1: change the focused row's value
  bool activate = false;     // Cross on a row
  bool back = false;         // Circle: the caller goes back (leaves a category, or closes)
};

// One press. tabs: how many tabs; rows: the open tab's rows; has_value: whether the focused row's
// value can be changed with left and right (asked only while a row is focused).
template <typename HasValue>
Effect Press(Nav& nav, Key key, int tabs, int rows, HasValue&& has_value) {
  Effect effect;
  const auto switch_tab = [&](int step) {
    if (tabs < 2) return;
    nav.tab = (nav.tab + step + tabs) % tabs;
    nav.on_bar = true;
    nav.row = 0;
    effect.tab_changed = true;
  };
  if (key == Key::l1) { switch_tab(-1); return effect; }
  if (key == Key::r1) { switch_tab(+1); return effect; }
  if (key == Key::circle) { effect.back = true; return effect; }
  if (nav.on_bar) {
    if (key == Key::left) switch_tab(-1);
    else if (key == Key::right) switch_tab(+1);
    else if ((key == Key::down || key == Key::cross) && rows > 0) { nav.on_bar = false; nav.row = 0; }
    return effect;
  }
  if (rows <= 0) { nav.on_bar = true; nav.row = 0; return effect; }
  if (nav.row >= rows) nav.row = rows - 1;
  switch (key) {
    case Key::up:
      if (nav.row == 0) nav.on_bar = true;
      else --nav.row;
      break;
    case Key::down:
      if (nav.row + 1 < rows) ++nav.row;
      break;
    case Key::left:
    case Key::right:
      if (has_value(nav.row)) effect.value_step = key == Key::left ? -1 : +1;
      break;
    case Key::cross:
      effect.activate = true;
      break;
    default:
      break;
  }
  return effect;
}

}  // namespace xbox360ps5::tab_bar

#endif  // XBOX360PS5_TAB_BAR_HPP_
