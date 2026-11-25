// SPDX-License-Identifier: MIT
// Fix 149d (tools/source-1.0.2/fixes/149d-help-card-sides.patch), from the review of 1.0.6 candidate 3:
//  (a) the guide's help card (fix 149b) no longer covers the panels the in-game overlay draws beside the
//      guide: the motion panel (camera motion on; top right, or top left with "Motion panel: Top left")
//      and fix 147c's Harmonix music keys panel (on the other side). option_groups.hpp's BusySides,
//      HelpCardSide and CardX: right of the guide when free, else left, else no card; with the real boxes
//      (phone_qr_panel.hpp) the card never meets a panel that is drawn;
//  (b) the guide draws the card through that rule, with the overlay's own conditions, and the overlay
//      still puts the two panels where the rule expects them;
//  (c) "Emulated console resolution"'s help says what fixes 140b and 140c made true: the mode applies when
//      the game starts, and 540p is not a mode a real console reports (the game is told 720p), checked
//      against video_mode.hpp;
//  (d) its translations in all 16 language files of the argument's folder.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "xbox360ps5/option_groups.hpp"
#include "xbox360ps5/phone_qr_panel.hpp"
#include "xbox360ps5/video_mode.hpp"

namespace {
namespace fs = std::filesystem;
namespace og = xbox360ps5::option_groups;

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

struct Box {
  float left, right;
};
bool Meet(Box a, Box b) { return a.left < b.right && b.left < a.right; }

void CheckSides() {
  using S = og::CardSide;
  const auto side = [](bool motion, int panel, bool keys) { return og::HelpCardSide(og::BusySides(motion, panel, keys)); };
  Expect(side(false, 0, false) == S::right, "(a) nothing drawn beside the guide: the card on the right, as before");
  Expect(side(true, 0, false) == S::left, "(a) the motion panel top right: the card on the left");
  Expect(side(true, 2, false) == S::left, "(a) motion panel off: its phone QR code may still show at the right");
  Expect(side(true, 1, false) == S::right, "(a) the motion panel top left: the card on the right");
  Expect(side(false, 0, true) == S::right && side(false, 1, true) == S::left,
         "(a) the keys panel alone: on the motion panel's other side, the card on the free one");
  Expect(side(true, 1, true) == S::none && side(true, 0, true) == S::none, "(a) both sides used: no card");
  // The real boxes, in 1080p units: the guide is x 412-1508.
  const auto& at = xbox360ps5::motion::kQrPanel;
  const Box right_panel{at.left, at.right}, left_panel{at.left + xbox360ps5::motion::kQrPanelLeftShift,
                                                       at.right + xbox360ps5::motion::kQrPanelLeftShift};
  const float x_right = og::CardX(S::right, 412, 1508), x_left = og::CardX(S::left, 412, 1508);
  const Box card_right{x_right, x_right + og::kCardWidth}, card_left{x_left, x_left + og::kCardWidth};
  Expect(x_right == 1522 && card_right.right <= 1920, "(a) the right card as in 149b, on screen");
  Expect(x_left >= 0 && card_left.right + og::kCardGap == 412, "(a) the left card ends where the guide starts, on screen");
  Expect(Meet(card_right, right_panel) && Meet(card_left, left_panel), "(a) the review's overlap: a card meets the panel on its side");
  for (bool motion : {false, true})
    for (int panel : {0, 1, 2})
      for (bool keys : {false, true}) {
        const og::SidePanels busy = og::BusySides(motion, panel, keys);
        const S chosen = og::HelpCardSide(busy);
        if (chosen == S::none) continue;
        const Box card = chosen == S::right ? card_right : card_left;
        Expect(!(busy.right && Meet(card, right_panel)) && !(busy.left && Meet(card, left_panel)),
               "(a) the card meets no drawn panel (motion " + std::to_string(motion) + ", panel " + std::to_string(panel) +
                   ", keys " + std::to_string(keys) + ")");
      }
}

void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string draw = Between(main, "// Fix 149d: on the side of the guide no overlay panel", "state_.focus_y, 238, 860);");
  Expect(Has(draw, "groups::HelpCardSide(groups::BusySides(") &&
             Has(draw, "xbox360ps5::motion::enabled.load(), settings_.motion_panel, xbox360ps5::harmonix_keys::needed.load()));") &&
             Has(draw, "if (card_side != groups::CardSide::none && !state_.on_bar") &&
             Has(draw, "groups::CardX(card_side, left, right),"),
         "(b) the guide places the card by the rule, from the overlay's own conditions");
  Expect(!Has(main, "*items[size_t(state_.row)].option, right + 14,"), "(b) no card fixed at the right any more");
  Expect(Has(main, "const float width = groups::kCardWidth, pad = 18,"), "(b) the card's width is the rule's");
  // The overlay: the motion panel where its option says, the keys panel on the other side.
  Expect(Has(main, "shift_ = motion_panel_ == 1 ? xbox360ps5::motion::kQrPanelLeftShift : 0.0f;") &&
             Has(main, "motion_panel_ == 1 ? 0.0f : xbox360ps5::motion::kQrPanelLeftShift);") &&
             Has(main, "if (xbox360ps5::harmonix_keys::needed.load()) {") && Has(main, "if (xbox360ps5::motion::enabled.load()) {"),
         "(b) the overlay draws the panels where the rule expects them");
}

void CheckResolution() {
  namespace vm = xbox360ps5::video_mode;
  const std::string about = og::kResolutionAbout;
  Expect(og::About("internal_resolution", "x") == og::kResolutionAbout, "(c) the option's help is this text");
  const vm::Size size540 = vm::ForChoice(3), size480 = vm::ForChoice(2);
  Expect(!vm::ReportedMode(size540.width, size540.height).reported && Has(about, "540p is not a mode a real console reports: the game is told 720p"),
         "(c) 540p: the game is told 720p (fix 140b), and the help says so");
  Expect(Has(about, "It applies when the game starts."), "(c) the mode is latched when the game starts (fix 140c)");
  Expect(vm::ReportedMode(size480.width, size480.height).reported && Has(about, "at 480p some draw fewer"),
         "(c) 480p is still a mode some games draw fewer pixels for");
  Expect(!Has(about, "480p or 540p"), "(c) 540p no longer listed as drawing fewer pixels");
}

void CheckLanguages(const fs::path& folder) {
  std::string escaped;
  for (const char* c = og::kResolutionAbout; *c; ++c) escaped += *c == ' ' ? std::string("\\s") : std::string(1, *c);
  int files = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++files;
    const std::string code = entry.path().stem().string();
    std::map<std::string, std::string> table;
    std::istringstream lines(Read(entry.path()));
    for (std::string line; std::getline(lines, line);) {
      const size_t split = line.find(" = ");
      if (split != std::string::npos && line[0] != '#') table[line.substr(0, split)] = line.substr(split + 3);
    }
    const auto found = table.find(escaped);
    Expect(found != table.end() && !found->second.empty(), "(d) " + code + " has the resolution's help");
    if (found != table.end() && code != "en") Expect(found->second != escaped, "(d) " + code + " translates it");
  }
  Expect(files == 16, "(d) the 16 language files");
}

}  // namespace

int main(int argc, char** argv) {
  CheckSides();
  CheckGuide();
  CheckResolution();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "(d) the language folder is the argument");
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-help-card: all checks passed\n");
  return 0;
}
