// SPDX-License-Identifier: MIT
// Fix 115b (tools/source-1.0.2/fixes/115b-guide-header-shoulders.patch): two notes from the console
// run of 1.0.5-work-fix115 (research/fix115-guide/g1.png):
//  (a) "Playing: <game>" was faint grey at the bottom of the list, on the last row's divider and
//      over "Exit emulator" (seven rows since fix 115). It is now in the header after the guide's
//      name, in the colour of who is signed in, cut at a whole UTF-8 character with "..." before it
//      would reach the text on the right (guide_quick::FitText);
//  (b) L1 and R1 were one tiny "L1 R1" circle in the legend. They are now pills at the two ends of
//      the tabs (L1 at the panel's left edge, R1 at its right edge), the look of the game sheet's
//      tabs (fix 109c): fully round ends, the name in white on the guide's grey.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/guide_quick.hpp"

namespace {
namespace fs = std::filesystem;
using xbox360ps5::guide_quick::FitText;

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
// A width of one unit per character (a UTF-8 lead or single byte), as a fixed-width font.
float Chars(const std::string& s) {
  float n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}
bool ValidUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = s[i];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

void CheckFit() {
  // (a)
  Expect(FitText("Gra: GTA IV", 20, Chars) == "Gra: GTA IV", "(a) a name that fits stays whole");
  Expect(FitText("Gra: GTA IV", 11, Chars) == "Gra: GTA IV", "(a) ... also exactly");
  Expect(FitText("Playing: Grand Theft Auto IV", 16, Chars) == "Playing: Gran...", "(a) a long name is cut with \"...\"");
  Expect(FitText("Playing: Grand Theft Auto IV", 12, Chars) == "Playing:...", "(a) no space left before \"...\"");
  Expect(FitText("abc", 2, Chars).empty(), "(a) nothing when not even \"...\" fits");
  // Polish, Japanese and Russian names: never cut inside a character.
  for (const std::string name : {"Gra: Wiedźmin żółć ąęść", "ゲーム: 龍が如く 見参！ 極",
                                 "Игра: Сталкер Тень Чернобыля"}) {
    for (int room = 3; room <= int(Chars(name)); ++room) {
      const std::string fit = FitText(name, float(room), Chars);
      Expect(ValidUtf8(fit), "(a) whole UTF-8 characters: " + name + " in " + std::to_string(room));
      Expect(Chars(fit) <= room, "(a) fits its room: " + name + " in " + std::to_string(room));
      Expect(fit == name || fit.empty() || (fit.size() > 3 && fit.compare(fit.size() - 3, 3, "...") == 0),
             "(a) whole, cut with \"...\" after some of it, or nothing: " + name);
    }
  }
}

void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  // (a)
  const std::string header = Between(main, "fade(210, 216, 224), who.c_str());", "// The page that is not open");
  Expect(Has(header, "if (!game_.empty()) {") &&
             Has(header, "large->CalcTextSizeA(36 * scale, 4096.0f, 0.0f, Tr(\"PS5X360Reloaded Guide\")).x") &&
             Has(header, "xbox360ps5::guide_quick::FitText(") && Has(header, "Tr(\"Playing: \") + game_") &&
             Has(header, "right * scale - who_size.x - 32 * scale - game_x") &&
             Has(header, "list->AddText(medium, 28 * scale, ImVec2(game_x, 246 * scale), fade(210, 216, 224), running.c_str());"),
         "(a) the running game in the header, after the guide's name, readable, cut before the right-hand text");
  Expect(!Has(main, "at(page_left + strip + 24, bottom - 44), fade(96, 102, 110), running.c_str()"),
         "(a) no longer faint at the bottom of the list");
  // (b)
  const std::string shoulder = Between(main, "static void Shoulder(", "\n  }\n");
  Expect(Has(shoulder, "const float width = size.x + 24 * scale, height = 30 * scale;") &&
             Has(shoulder, "colour, height / 2);") && Has(shoulder, "IM_COL32(255, 255, 255,"),
         "(b) a pill: 30 px high, 12 px each side of its name, fully round ends, white name");
  const std::string legend = Between(main, "const bool in_category = state_.page == 1 && state_.category >= 0;", "kBuildVersionLabel");
  Expect(Has(legend, "Shoulder(list, small, scale, left * scale, y, fade(110, 116, 124), \"L1\", false);") &&
             Has(legend, "Shoulder(list, small, scale, right * scale, y, fade(110, 116, 124), \"R1\", true);"),
         "(b) L1 at the left end of the tabs, R1 at the right end");
  Expect(!Has(main, "? \"L1 R1\" :"), "(b) no squeezed \"L1 R1\" circle any more");
}

}  // namespace

int main() {
  CheckFit();
  CheckGuide();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-guide-header: all checks passed\n");
  return 0;
}
