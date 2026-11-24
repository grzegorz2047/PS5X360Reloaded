// SPDX-License-Identifier: MIT
// Fix 150g (tools/source-1.0.2/fixes/150g-kinect-sports-1x.patch): Kinect Sports runs at 1x even
// when resolution scale 2x is chosen. At 2x its Kinect camera pictures are copied with copy command
// 2, which the core cannot do scaled (the big screen green, the replay panel black), and its render
// targets do not fit in memory (1.0.6-work-fix150f).
//  (a) include/xbox360ps5/resolution_scale.hpp: TwoXUnfit (Kinect Sports 4D5308C9, in either case;
//      not Season Two, GTA IV, no title) and ChoiceFor (2x becomes 1x for it only, 1x stays);
//  (b) src/launcher.cpp: ForGame gives such a game 1x after the preset and its own settings, so a
//      game chosen in the launcher at 2x starts again at 1x (draw_scale is a start option);
//  (c) src/canary_game_main.cpp: at the launch, when 2x is chosen for it (its own setting or the
//      general one), a log line and the notice through Tr;
//  (d) the notice in every language file (the directory given as the argument).
// Run from the assembled tree's root: check-two-x-unfit <english/lang directory>.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/resolution_scale.hpp"

namespace {
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream text;
  text << input.rdbuf();
  return text.str();
}
std::string Squeezed(const std::string& text) {
  std::string out;
  for (char c : text)
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') out += c;
  return out;
}
const std::string kNotice =
    "Resolution scale (test): this game always runs at 1x. At 2x its Kinect camera pictures cannot be drawn and "
    "its video memory runs out.";

void Rule() {
  namespace rs = xbox360ps5::resolution_scale;
  Expect(rs::TwoXUnfit("4D5308C9") && rs::TwoXUnfit("4d5308c9"), "(a) Kinect Sports (4D5308C9, either case): 1x");
  Expect(!rs::TwoXUnfit("4D5309D6") && !rs::TwoXUnfit("545407F2") && !rs::TwoXUnfit("") &&
             !rs::TwoXUnfit("4D5308C"),
         "(a) Season Two (not measured), GTA IV, no title, a short ID: 2x allowed");
  Expect(rs::ChoiceFor(1, "4D5308C9") == 0 && rs::ChoiceFor(0, "4D5308C9") == 0,
         "(a) Kinect Sports: 2x chosen gives 1x, 1x stays 1x");
  Expect(rs::ChoiceFor(1, "545407F2") == 1 && rs::ChoiceFor(0, "545407F2") == 0 && rs::ChoiceFor(1, "") == 1,
         "(a) another game and the launcher (no title) keep their choice");
}

void Wiring() {
  const std::string launcher = Squeezed(Read("src/launcher.cpp"));
  const size_t for_game = launcher.find(Squeezed(
      "Settings ForGame(const Settings& general, const std::string& title_id, const GameOverrides& own) {"));
  const size_t merged =
      launcher.find(Squeezed("auto effective = WithOverrides(WithOverrides(general, GamePreset(title_id)), own);"), for_game);
  const size_t capped = launcher.find(
      Squeezed("effective.draw_scale = xbox360ps5::resolution_scale::ChoiceFor(effective.draw_scale, title_id);"), for_game);
  const size_t returned = launcher.find(Squeezed("return effective;"), for_game);
  Expect(for_game != std::string::npos && merged != std::string::npos && capped != std::string::npos &&
             returned != std::string::npos && for_game < merged && merged < capped && capped < returned,
         "(b) ForGame caps draw_scale after the preset and the game's own settings");

  const std::string main = Squeezed(Read("src/canary_game_main.cpp"));
  const size_t launch = main.find(Squeezed("XELOGI(\"GAME LAUNCH {:08X}\", status);"));
  const size_t chosen = main.find(Squeezed(
      "!status && xbox360ps5::resolution_scale::TwoXUnfit(settings_title) && (own_scale != overrides.end() ? "
      "own_scale->second : global.draw_scale) == 1"), launch);
  const size_t line = main.find(Squeezed("Resolution scale (fix 150g): {} runs at 1x although 2x is chosen ({})"), launch);
  const size_t whose =
      main.find(Squeezed("own_scale != overrides.end() ? \"its own setting\" : \"the general setting\""), line);
  const size_t notice = main.find(Squeezed("xbox360ps5::Notify({XBOX360PS5_PRODUCT_NAME, Tr(\"" + kNotice + "\"), \"\", false});"),
                                  launch);
  Expect(launch != std::string::npos && chosen != std::string::npos && line != std::string::npos &&
             notice != std::string::npos && launch < chosen && chosen < line && line < notice,
         "(c) the launch logs it and shows the notice through Tr when 2x is chosen for it");
  Expect(whose != std::string::npos && whose < notice,
         "(c) the line says whose choice 2x was: the game's own setting or the general one");
}

std::string Escaped(const std::string& text) {
  std::string out;
  for (char c : text) out += c == ' ' ? std::string("\\s") : std::string(1, c);
  return out;
}

void Languages(const std::filesystem::path& lang) {
  const std::vector<std::string> codes = {"en", "pt-BR", "pt", "es", "es-419", "de", "fr", "it",
                                          "ja", "ko", "nl", "pl", "ru", "tr", "zh-Hans", "zh-Hant"};
  const std::string key = Escaped(kNotice) + " = ";
  for (const std::string& code : codes) {
    const std::string file = Read(lang / (code + ".txt"));
    const size_t at = file.find("\n" + key);
    const size_t end = at == std::string::npos ? at : file.find('\n', at + 1 + key.size());
    const std::string value = at == std::string::npos ? std::string() : file.substr(at + 1 + key.size(), end - at - 1 - key.size());
    Expect(!value.empty() && value.find("1x") != std::string::npos && value.find("2x") != std::string::npos &&
               value.find("Kinect") != std::string::npos,
           "(d) " + code + ".txt translates the notice (1x, 2x, Kinect kept)");
  }
  const std::string pl = Read(lang / "pl.txt");
  Expect(pl.find(key + "Skala\\srenderowania\\s(test):") != std::string::npos,
         "(d) pl.txt names the option as its own label does (Skala renderowania (test))");
}
}  // namespace

int main(int argc, char** argv) {
  Rule();
  if (std::filesystem::exists("src/launcher.cpp")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root (src/launcher.cpp)");
  }
  if (argc > 1) {
    Languages(argv[1]);
  } else {
    Expect(false, "the english/lang directory as the argument");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: Kinect Sports runs at 1x, with a notice, when 2x is chosen\n");
  return 0;
}
