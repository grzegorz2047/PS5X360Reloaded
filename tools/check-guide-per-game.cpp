// SPDX-License-Identifier: MIT
// Fix 143 (tools/source-1.0.2/fixes/143-guide-keeps-for-game.patch): an option changed in the
// in-game guide is kept for the running game (its own options, game-settings/<TITLE ID>.txt),
// not for every game. Before, the guide kept it for every game unless the game already had a
// value of its own or a preset, so a first change there silently changed every game (Dragon Age
// II, 4541090B: the video rate's 120 Hz test and anisotropic 16x went to settings.txt).
//  (a) guide_quick.hpp: WhereToKeep keeps a per-game option for the game when the title ID is
//      known; an option without the per-game flag, or a game without a title ID, goes to the
//      general options;
//  (b) the launcher's table (src/launcher.cpp): every option the guide shows is decided by its
//      flag; the ones kept for every game are exactly the console's own (the interface sounds and
//      their volume, Sound, the touchpad click, Show FPS, achievement notifications, Smooth
//      gradients), and the ones the report named (video rate, anisotropic filtering) and Game
//      volume are kept for the game;
//  (c) a game's file cannot hold an option without the flag (LoadGameOverrides drops it), which
//      is why those stay general;
//  (d) the guide's option rows: WhereToKeep with the option's flag, the old rule (the game's own
//      value or its preset) gone, the place logged; still applied at once (Apply, ApplyPicture)
//      and shown on the phone's page (publish_web), whose "own" values are the game's file, so
//      the change shows there as this game's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/guide_quick.hpp"

namespace {
namespace fs = std::filesystem;
namespace gq = xbox360ps5::guide_quick;

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
  for (char c : s) if (c != '\r') out += c;  // canary_game_main.cpp has CR LF lines.
  return out;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}

void CheckRule() {
  using K = gq::Keep;
  Expect(gq::WhereToKeep(true, true) == K::game, "(a) a per-game option with a title ID: the game's");
  Expect(gq::WhereToKeep(true, false) == K::general, "(a) ... without a title ID: general (no file to keep it in)");
  Expect(gq::WhereToKeep(false, true) == K::general, "(a) an option without the per-game flag: general");
  Expect(gq::WhereToKeep(false, false) == K::general, "(a) ... also without a title ID");
}

struct Row {
  std::string key, when;
  bool per_game = false;
};
std::vector<Row> Table(const std::string& launcher) {
  std::vector<Row> rows;
  const std::regex row("\\{\"([a-z_0-9]+)\", &S::[a-z_0-9]+, [0-9]+, W::([a-z]+), [0-9]+, (true|false),");
  for (std::sregex_iterator it(launcher.begin(), launcher.end(), row), end; it != end; ++it)
    rows.push_back({(*it)[1].str(), (*it)[2].str(), (*it)[3].str() == "true"});
  return rows;
}

void CheckTable() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::vector<Row> rows = Table(launcher);
  Expect(rows.size() >= 40, "(b) the launcher's option table is read (" + std::to_string(rows.size()) + " rows)");
  // The rows the guide's settings tab lists (GuideItems' `shown`) and the quick mute row.
  std::set<std::string> general, game;
  for (const auto& row : rows) {
    if (row.when != "live" || row.key == "detailed_logs" || row.key == "bug_report") continue;
    (gq::WhereToKeep(row.per_game, true) == gq::Keep::game ? game : general).insert(row.key);
  }
  const std::set<std::string> console_own = {"dither",        "mute",     "ui_sounds",         "ui_sound_volume",
                                             "touchpad_menu", "show_fps", "achievement_toasts"};
  Expect(general == console_own,
         "(b) the guide keeps for every game exactly the console's own options (a new option without the per-game "
         "flag needs a decision here)");
  for (const char* key : {"video_clock", "anisotropic", "volume", "image_filter", "sharpness", "stretch", "readback",
                          "vibration", "deadzone_left", "save_drive"})
    Expect(game.count(key) == 1, std::string("(b) ") + key + " changed in the guide is kept for the game");
  // (c)
  Expect(Has(launcher, "if (option && option->per_game && value >= 0 && value < int(option->choices.size())) overrides[key] = value;"),
         "(c) a game's file keeps only options with the per-game flag");
}

void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string option = Between(main, "case GuideAction::option: {", "case GuideAction::capture:");
  Expect(!option.empty(), "(d) the guide's option rows are found");
  Expect(Has(option, "const bool for_game = xbox360ps5::guide_quick::WhereToKeep(option.per_game, !settings_title.empty()) ==") &&
             Has(option, "xbox360ps5::guide_quick::Keep::game;") && Has(option, "if (for_game) {"),
         "(d) the guide decides by the option's flag");
  const std::string game_branch = Between(option, "if (for_game) {", "} else {");
  Expect(Has(game_branch, "overrides[option.key] = value;") &&
             Has(game_branch, "xbox360ps5::SaveGameOverrides(settings_title, overrides);") && !Has(game_branch, "global"),
         "(d) the game's value goes to its own options only");
  const std::string general_branch = Between(option, "} else {", "}\n");
  Expect(Has(general_branch, "global.*option.field = value;") && Has(general_branch, "global.Save();"),
         "(d) the console's own options go to the general ones");
  Expect(!Has(option, "GamePreset(settings_title).count(option.key)") && !Has(option, "overrides.count(option.key)"),
         "(d) the rule from before (the game's own value or its preset) is gone");
  Expect(Has(option, "kept for {}") && Has(option, "for_game ? settings_title : std::string(\"every game\")"),
         "(d) the log says where the value is kept");
  Expect(Has(main, "              if (changed) {\n                settings.Apply();\n"
                   "                ApplyPicture(settings, emulator.graphics_system()->presenter());\n"
                   "                publish_web(settings_title, game_name);"),
         "(d) still applied at once and shown on the phone's page");
  Expect(Has(main, "{\"own\", xbox360ps5::LoadGameOverrides(id)}"),
         "(d) the phone's page shows a game's own values from its file (the guide's change as this game's)");
}

}  // namespace

int main() {
  CheckRule();
  CheckTable();
  CheckGuide();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("guide keeps for the game: all checks passed\n");
  return 0;
}
