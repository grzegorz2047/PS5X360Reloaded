// SPDX-License-Identifier: MIT
// Fix 115 (tools/source-1.0.2/fixes/115-guide-quick-actions.patch, issues #33 and #34): the
// in-game guide's first page gets "Restart game" (this game again from its start, without the
// logs for a bug report) and "Mute game sound" (the Sound option, switched at once), and a
// "Game volume" changed in the guide is kept for this game.
//  (a) guide_quick.hpp: a plain restart sets no bug_report value; the one with logs sets the
//      game's own value (the general one without a title ID);
//  (b) guide_quick.hpp: Game volume is kept for the game even when it has no value of its own
//      (with a title ID); every other option keeps the rule from before (the game's own value or
//      its preset: the game; otherwise the general options);
//  (c) guide_quick.hpp: the quick mute row says On while the game is muted;
//  (d) the guide's first page: "Restart game" right after "Close the game and return to the
//      menu", then the Sound option as "Mute game sound", with its value; the restart not over
//      the dashboard; the page scrolls past seven rows;
//  (e) the guide's actions: both restarts go through one helper that sets the flags of (a), then
//      restart_into_game (after a title switch its launch data again, fix 33; otherwise
//      launch-once.txt), with "Restarting the game..." and fix 93i's bounded way out; Cross only;
//  (f) the option rows keep values by WhereToKeep; the mute row is an option row (switched,
//      applied at once, kept as the Sound option is);
//  (g) the two new texts are in every language file of the argument's folder, translated.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
size_t At(const std::string& text, const std::string& part, size_t from = 0) { return text.find(part, from); }

void CheckRules() {
  // (a)
  for (const bool title : {true, false}) {
    const auto plain = gq::RestartFlags(false, title);
    Expect(!plain.game && !plain.general, std::string("(a) a plain restart sets no bug_report value, title ID ") + (title ? "known" : "unknown"));
  }
  const auto logs = gq::RestartFlags(true, true);
  Expect(logs.game && !logs.general, "(a) the restart with logs sets the game's own value");
  const auto logs_unknown = gq::RestartFlags(true, false);
  Expect(!logs_unknown.game && logs_unknown.general, "(a) ... the general one without a title ID");
  // (b)
  using K = gq::Keep;
  Expect(gq::WhereToKeep("volume", false, false, true) == K::game, "(b) Game volume: the game's, with no value of its own yet");
  Expect(gq::WhereToKeep("volume", true, false, true) == K::game, "(b) Game volume: the game's, with its own value");
  Expect(gq::WhereToKeep("volume", false, true, true) == K::game, "(b) Game volume: the game's, with a preset");
  Expect(gq::WhereToKeep("volume", false, false, false) == K::general, "(b) Game volume: general without a title ID");
  for (const char* key : {"mute", "show_fps", "ui_sounds", "ui_sound_volume", "touchpad_menu", "volume_boost", "vol"}) {
    const std::string k = key;
    Expect(gq::WhereToKeep(key, false, false, true) == K::general, "(b) " + k + ": general, as before");
    Expect(gq::WhereToKeep(key, true, false, true) == K::game, "(b) " + k + ": the game's own value stays the game's");
    Expect(gq::WhereToKeep(key, false, true, true) == K::game, "(b) " + k + ": a preset's value is kept for the game");
    Expect(gq::WhereToKeep(key, false, false, false) == K::general, "(b) " + k + ": general without a title ID");
  }
  // (c)
  Expect(std::strcmp(gq::MuteRowValue(1), "On") == 0 && std::strcmp(gq::MuteRowValue(0), "Off") == 0,
         "(c) the quick mute row: On while muted, Off otherwise");
}

void CheckGame() {
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string launcher = Read("src/launcher.cpp");
  Expect(Has(main, "#include \"xbox360ps5/guide_quick.hpp\""), "the guide uses guide_quick.hpp");
  // The Sound option the mute row is.
  Expect(Has(launcher, "{\"mute\", &S::mute, 2, W::live, 0, false, \"Sound\", \"Mute game audio.\", {\"On\", \"Muted\"}},"),
         "(f) the Sound option: key mute, live, 0 on and 1 muted");
  // (d)
  const std::string page = Between(main, "  if (page == 0) {", "return items;");
  const size_t shelf = At(page, "{GuideAction::shelf, Tr(\"Close the game and return to the menu\"), \"\"}};");
  const size_t restart = At(page, "if (g_guide_restart) items.push_back({GuideAction::restart_game, Tr(\"Restart game\"), \"\"});");
  const size_t mute = At(page, "items.push_back({GuideAction::option, Tr(\"Mute game sound\"), Tr(xbox360ps5::guide_quick::MuteRowValue(settings.mute)), &option});");
  const size_t back = At(page, "GuideAction::back_button");
  Expect(shelf != std::string::npos && restart != std::string::npos && shelf < restart,
         "(d) \"Restart game\" right after \"Close the game and return to the menu\"");
  Expect(mute != std::string::npos && restart < mute && mute < back, "(d) then \"Mute game sound\", before the other rows");
  Expect(Has(page, "if (std::strcmp(option.key, \"mute\") == 0)"), "(d) the mute row is the Sound option");
  Expect(Has(main, "bool g_guide_restart = true;") && Has(main, "g_guide_restart = !dashboard;"),
         "(d) no restart over the dashboard");
  Expect((Has(main, "constexpr int kGuideGameRows = 7;") &&
              Has(main, "std::clamp(state_.row - kGuideGameRows / 2, 0, std::max(0, listed - kGuideGameRows))") &&
              Has(main, "std::min(listed, first_row + kGuideGameRows)")) ||
             // Fix 115c (check-guide-tabs.cpp): both pages scroll the same way below the tab bar.
             (Has(main, "const int first_row = std::clamp(state_.row - shown_rows / 2, 0, std::max(0, listed - shown_rows));") &&
              Has(main, "const int last_row = std::min(listed, first_row + shown_rows);")),
         "(d) the first page scrolls past seven rows (the panel holds seven and a half)");
  // (e)
  Expect(Has(main, "measure, capture, network, restart_game,"), "(e) the restart action");
  const std::string helper = Between(main, "const auto restart_this_game = [&](bool with_logs) {", "};");
  Expect(Has(helper, "xbox360ps5::guide_quick::RestartFlags(with_logs, !settings_title.empty())") &&
             Has(helper, "if (flags.game) {") && Has(helper, "overrides[\"bug_report\"] = 1;") &&
             Has(helper, "xbox360ps5::SaveGameOverrides(settings_title, overrides);") && Has(helper, "if (flags.general) {") &&
             Has(helper, "global.bug_report = 1;") && Has(helper, "global.Save();"),
         "(e) the helper sets only the flags guide_quick says");
  Expect(Has(helper, "restart_into_game();") && Has(helper, "restart_same_game = true;") && Has(helper, "restart = true;"),
         "(e) the helper restarts into the game that runs now, with \"Restarting the game...\"");
  Expect(!Has(helper, "launch-once.txt"), "(e) the helper leaves the start's file to restart_into_game");
  const std::string into = Between(main, "const auto restart_into_game = [&] {", "};");
  Expect(Has(into, "if (switched) {") && Has(into, "xbox360ps5::switch_title::RestorePending(") &&
             Has(into, "std::ofstream(storage / \"launch-once.txt\", std::ios::trunc)") &&
             Has(into, "xbox360ps5::forward::OnceText(game.string(), settings_title, forwarded, exit_after_game)"),
         "(e) after a title switch its launch data again, otherwise launch-once.txt");
  const size_t into_at = At(main, "const auto restart_into_game = [&] {");
  const size_t loop_at = At(main, "while (!launched && !restart && !context.HasQuitFromUIThread()) {");
  Expect(into_at != std::string::npos && loop_at != std::string::npos && into_at < loop_at,
         "(e) restart_into_game is defined before the launch loop, so the guide can use it");
  const std::string with_logs = Between(main, "case GuideAction::bug_report_restart:", "case GuideAction::restart_game:");
  Expect(Has(with_logs, "if (!(pressed & 0x4000)) break;") && Has(with_logs, "restart_this_game(true);") &&
             !Has(with_logs, "launch-once.txt"),
         "(e) the restart with logs goes through the helper, Cross only");
  const std::string plain = Between(main, "case GuideAction::restart_game:", "break;\n                case");
  Expect(Has(main, "case GuideAction::restart_game:") && Has(main, "restart_this_game(false);") &&
             Has(Between(main, "case GuideAction::restart_game:", "restart_this_game(false);"), "if (!(pressed & 0x4000)) break;"),
         "(e) \"Restart game\": the helper without logs, Cross only");
  Expect(!Has(Between(main, "case GuideAction::restart_game:", "restart_this_game(false);"), "bug_report"),
         "(e) \"Restart game\" touches no bug_report value");
  Expect(Has(main, "leaving::Show(restart ? (restart_same_game ? leaving::Kind::restart_game : leaving::Kind::menu)"),
         "(e) \"Restarting the game...\"");
  Expect(Has(main, "ShutdownLogging") , "(e) fix 93i's bounded way out is in the tree");
  // (f)
  const std::string option = Between(main, "case GuideAction::option: {", "case GuideAction::capture:");
  Expect(Has(option, "xbox360ps5::guide_quick::WhereToKeep(option.key, overrides.count(option.key) != 0,") &&
             Has(option, "xbox360ps5::GamePreset(settings_title).count(option.key) != 0,") &&
             Has(option, "!settings_title.empty()) == xbox360ps5::guide_quick::Keep::game) {") &&
             Has(option, "overrides[option.key] = value;") && Has(option, "global.*option.field = value;"),
         "(f) the option rows keep values by WhereToKeep");
  Expect(!Has(option, "if (overrides.count(option.key) || xbox360ps5::GamePreset(settings_title).count(option.key)) {"),
         "(f) the rule from before is gone from the guide");
  Expect(Has(option, "value = (value + step + count) % count;"), "(f) a press switches the option (the mute row: on, off)");
  Expect(Has(main, "              if (changed) {\n                settings.Apply();"), "(f) and it applies at once");
}

std::map<std::string, std::string> ReadLang(const fs::path& path) {
  std::map<std::string, std::string> texts;
  std::istringstream lines(Read(path));
  for (std::string line; std::getline(lines, line);) {
    const size_t eq = line.find(" = ");
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    texts[line.substr(0, eq)] = line.substr(eq + 3);
  }
  return texts;
}

void CheckLanguages(const fs::path& folder) {
  const std::vector<std::string> keys = {"Restart\\sgame", "Mute\\sgame\\ssound"};
  int files = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++files;
    const std::string code = entry.path().stem().string();
    const auto texts = ReadLang(entry.path());
    for (const auto& key : keys) {
      const auto found = texts.find(key);
      Expect(found != texts.end() && !found->second.empty(), "(g) " + code + " has " + key);
      if (found != texts.end() && code != "en")
        Expect(found->second != key, "(g) " + code + " translates " + key);
    }
  }
  Expect(files == 16, "(g) the 16 language files");
}

}  // namespace

int main(int argc, char** argv) {
  CheckRules();
  CheckGame();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "(g) the language folder is the argument");
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("guide quick actions: all checks passed\n");
  return 0;
}
