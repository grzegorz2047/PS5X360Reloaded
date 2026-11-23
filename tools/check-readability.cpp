// SPDX-License-Identifier: MIT
// Fix 149c (tools/source-1.0.2/fixes/149c-readability.patch): a light readability pass for a TV at 2-3 m,
// after the user's "maybe larger fonts; check against a light WCAG, without big changes" on
// 1.0.6-work-fix149b (#57). Contrast after WCAG 2.1 (relative luminance, (L1 + 0.05) / (L2 + 0.05)):
// AA is 4.5:1 for normal text and 3:1 for large text (24 px and more here).
//  (a) the launcher's text colours, read from its ApplyTheme (dark and light): text, body, muted, faint,
//      accent and warning reach 4.5:1 on the sheet and on a row (kSurface at 60% over the sheet), 3:1 on the
//      focused row (its values are 24 px); the Play button's ink on the accent 3:1 (36 px);
//  (b) the guide's colours, read from its code: white on the focus bar's green and on the open tab's green
//      (tab_bar.hpp) 4.5:1; the values, the section names and the help card's lines 4.5:1 on the panel;
//  (c) sizes: the settings' option rows 24 px, their help 22 px (the user's "larger fonts"); the guide's
//      rows 30 px and its help card 22 to 26 px as they were;
//  (d) a row's name has the room its value leaves, and the whole name is shown over the help when the row
//      had to cut it.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

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
  for (char c : s) if (c != '\r') out += c;
  return out;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}

struct Rgb {
  double r, g, b;
};
Rgb Hex(uint32_t v) { return {double((v >> 16) & 255), double((v >> 8) & 255), double(v & 255)}; }
double Channel(double c) {
  c /= 255.0;
  return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double Luminance(Rgb c) { return 0.2126 * Channel(c.r) + 0.7152 * Channel(c.g) + 0.0722 * Channel(c.b); }
double Contrast(Rgb a, Rgb b) {
  double x = Luminance(a), y = Luminance(b);
  if (x < y) std::swap(x, y);
  return (x + 0.05) / (y + 0.05);
}
Rgb Blend(Rgb top, Rgb under, double alpha) {
  return {top.r * alpha + under.r * (1 - alpha), top.g * alpha + under.g * (1 - alpha), top.b * alpha + under.b * (1 - alpha)};
}
std::string Ratio(double v) {
  char text[16];
  std::snprintf(text, sizeof(text), "%.2f", v);
  return text;
}

void CheckSelf() {
  Expect(std::fabs(Contrast(Hex(0xffffff), Hex(0x000000)) - 21.0) < 0.01, "the contrast formula: white on black is 21:1");
  Expect(std::fabs(Contrast(Hex(0x767676), Hex(0xffffff)) - 4.54) < 0.01, "the contrast formula: #767676 on white is 4.54:1");
}

void CheckLauncher() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::string theme = Between(launcher, "void ApplyTheme(bool dashboard) {", "\n}\n");
  const size_t split = theme.find("} else {");
  Expect(!theme.empty() && split != std::string::npos, "(a) ApplyTheme is read");
  const std::regex assign("k([A-Za-z]+) = 0x([0-9a-fA-F]{6})");
  const auto read = [&assign](const std::string& part) {
    std::map<std::string, uint32_t> colours;
    for (std::sregex_iterator it(part.begin(), part.end(), assign), end; it != end; ++it)
      colours[(*it)[1].str()] = uint32_t(std::stoul((*it)[2].str(), nullptr, 16));
    return colours;
  };
  const std::map<std::string, std::map<std::string, uint32_t>> themes = {{"light", read(theme.substr(0, split))},
                                                                         {"dark", read(theme.substr(split))}};
  for (const auto& [name, c] : themes) {
    Expect(c.size() >= 9, "(a) the " + name + " theme's colours are read");
    if (c.size() < 9) continue;
    const Rgb sheet = Hex(c.at("Sheet")), row = Blend(Hex(c.at("Surface")), sheet, 0.6), focused = Hex(c.at("SurfaceHigh"));
    for (const char* text : {"Text", "Body", "Muted", "Faint", "Accent", "Warning"}) {
      const Rgb ink = Hex(c.at(text));
      const double on_sheet = Contrast(ink, sheet), on_row = Contrast(ink, row), on_focus = Contrast(ink, focused);
      Expect(on_sheet >= 4.5, "(a) " + name + " k" + text + " on the sheet: " + Ratio(on_sheet) + ":1 (4.5)");
      Expect(on_row >= 4.5, "(a) " + name + " k" + text + " on a row: " + Ratio(on_row) + ":1 (4.5)");
      Expect(on_focus >= 3.0, "(a) " + name + " k" + text + " on the focused row (24 px): " + Ratio(on_focus) + ":1 (3)");
    }
    const double play = Contrast(Hex(0x0a0c11), Hex(c.at("Accent")));
    Expect(play >= 3.0, "(a) " + name + " the Play button's ink on the accent (36 px): " + Ratio(play) + ":1 (3)");
  }
  const std::regex start("uint32_t kInk = 0x0a0c11, kSheet = 0x12161d[^;]*kFaint = 0x([0-9a-f]{6})");
  std::smatch m;
  Expect(std::regex_search(launcher, m, start) && m[1].str() == "8a93a3", "(a) the colours before ApplyTheme are the dark theme's");
}

void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  const Rgb white = Hex(0xffffff), panel_top = {242, 243, 244}, panel_bottom = {214, 216, 219};
  Expect(Has(main, "fade(60, 130, 28, int(255 * content)), fade(60, 130, 28, int(255 * content)),\n"
                   "                                    fade(46, 110, 18, int(255 * content)), fade(46, 110, 18, int(255 * content)));"),
         "(b) the guide's focus bar green");
  for (const Rgb green : {Rgb{60, 130, 28}, Rgb{46, 110, 18}})
    Expect(Contrast(white, green) >= 4.5, "(b) white on the focus bar: " + Ratio(Contrast(white, green)) + ":1 (4.5)");
  for (const uint32_t green : {tb::kGreen, tb::kGreenDark})
    Expect(Contrast(white, Hex(green)) >= 4.5, "(b) the open tab's white name on its green: " + Ratio(Contrast(white, Hex(green))) + ":1 (4.5)");
  Expect(Has(main, "lit ? 0xFFFFFFu : 0x484E56u"), "(b) the rows' values");
  Expect(Contrast(Hex(0x484E56), panel_bottom) >= 4.5, "(b) a value on the panel's darkest grey");
  Expect(Has(main, "fade(84, 90, 98, a), items[size_t(n)].label.c_str());"), "(b) the section names");
  Expect(Contrast({84, 90, 98}, panel_bottom) >= 4.5, "(b) a section's name on the panel's darkest grey");
  Expect(Contrast({44, 48, 54}, panel_bottom) >= 4.5, "(b) a row's name on the panel");
  const std::string card = Between(main, "void DrawHelpCard(", "\n}\n");
  Expect(Has(card, "colour(242, 243, 244)") && Has(card, "colour(30, 34, 40)") && Has(card, "colour(96, 102, 110)") &&
             Has(card, "colour(44, 48, 54)") && Has(card, "colour(150, 90, 0)"),
         "(b) the help card's colours");
  for (const Rgb ink : {Rgb{30, 34, 40}, Rgb{96, 102, 110}, Rgb{44, 48, 54}, Rgb{150, 90, 0}})
    Expect(Contrast(ink, panel_top) >= 4.5, "(b) a help card line: " + Ratio(Contrast(ink, panel_top)) + ":1 (4.5)");
  Expect(Contrast({60, 44, 10}, {255, 214, 120}) >= 4.5, "(b) the notice's and the warning's ink on amber");
  // (c) the guide's sizes stay.
  Expect(Has(main, "list->AddText(large, (state_.page == 0 ? 34 : 30) * scale,"), "(c) the guide's rows 30 px or more");
  Expect(Has(card, "text(text_font, 24, colour(44, 48, 54), Tr(brief));") && Has(card, "text(title_font, 26, colour(30, 34, 40), title.c_str());"),
         "(c) the help card 24 and 26 px");
}

void CheckSizes() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::string sheet = Between(launcher, "void Launcher::DrawOptionsSheet(Canvas& c) {", "\nvoid Launcher::");
  Expect(!Has(sheet, "y + 10, 22, focused ? kText : kBody") && !Has(sheet, "x + 796, y + 10, 22,"),
         "(c) no 22 px option row left in the settings");
  Expect(Has(sheet, "c.Text(shown, x + 796, y + 9, 24,") && Has(sheet, "x + 64, y + 9, 24, focused ? kText : kBody"),
         "(c) the settings' option rows 24 px");
  Expect(!Has(sheet, ", 20, kBody, 750);") && Has(sheet, "c.Wrapped(Tr(option.about), x + 56, y + 4, 22, kBody, 750);"),
         "(c) the settings' help 22 px");
  const std::string game = Between(launcher, "void Launcher::DrawGameOptions(Canvas& c, float x) {", "\nvoid Launcher::");
  Expect(Has(game, "tab_bar::DrawValue(c, text.c_str(), x + 796, y + 9, 24,") &&
             Has(game, "c.Wrapped(Tr(option.about), x + 56, cut ? 818 : 786, 22, kBody, 750);") && !Has(game, ", 20, kBody, 750);"),
         "(c) a game's option rows 24 px, its help 22 px");
  Expect(Has(launcher, "c.Wrapped(rows[settings_row_].about, x + 56, 720, 22, kBody, 750);"), "(c) the top list's help 22 px");
  // (d)
  Expect(Has(sheet, "(732.0f - std::min(c.Width(shown, 24), 320.0f) - 28.0f)") &&
             Has(game, "(732.0f - std::min(c.Width(text, 24), 420.0f) - 28.0f) - 40"),
         "(d) a row's name has the room its value leaves");
  Expect(Has(sheet, "wrapped(Tr(option.label), 24, kText);") && Has(game, "if (cut) c.Text(Tr(option.label), x + 56, 784, 24, kText, 1.0f, 0, 750);"),
         "(d) the whole name over the help when the row cut it");
  // The room: a short value ("Off") leaves a long name more than the 440 px it had; a long one keeps 320 px for itself.
  const double short_value = 732.0 - 40.0 - 28.0, long_value = 732.0 - 320.0 - 28.0;
  Expect(short_value > 440.0 && long_value >= 380.0, "(d) the room a name gets");
}

}  // namespace

int main() {
  CheckSelf();
  CheckLauncher();
  CheckGuide();
  CheckSizes();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readability: all checks passed\n");
  return 0;
}
