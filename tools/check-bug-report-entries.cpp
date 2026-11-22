// SPDX-License-Identifier: MIT
// Fix 93d (tools/source-1.0.2/fixes/93d-bug-report-entries.patch): "Logs for a bug report" where
// players look for it. On 1.0.5-work-fix93b the user looked in a game's Settings -> System and in
// the in-game guide's System list, and found it in neither (fix 93 had it only in the general
// System list). It is now a row of its own, set apart from the emulation options (a divider, a
// round "!" mark), at the bottom of the general settings sheet and of a game's Settings tab, and
// an action at the end of the guide's System list that restarts the game with it.
//  (a) the option: in no category (no list of options shows it), a game may have its own value;
//  (b) the general settings sheet: a 15th row after the others, toggled and saved, drawn apart;
//  (c) a game's Settings tab: a row after its subjects for an identified game, toggling the
//      game's own value (saved in its file), drawn apart, with its explanation;
//  (d) the guide: below System's options a raised button (shadow, gradient, top highlight, dark text,
//      centred, not a row of the list; a dark outline only when focused), Cross only,
//      writing this game's own value (the general one without a title ID) and launch-once.txt,
//      then restarting with "Restarting the game...";
//  (e) a game's start takes the game's own value or the general one, and clears both;
//  (f) the six texts are in every language file of the argument's folder, translated.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

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

void CheckLauncher() {
  const std::string launcher = Read("src/launcher.cpp");
  Expect(Has(launcher, "{\"bug_report\", &S::bug_report, -1, W::live, 0, true, \"Logs for a bug report\","),
         "(a) the option is in no category, and a game may have it");
  Expect(Has(launcher, "for (int category = 0; category < int(std::size(kOptionCategories)); ++category)"),
         "(a) a game's subjects are the categories from 0 (never -1)");
  // (b)
  const std::string press = Between(launcher, "const int rows = 15;", "if (key == Key::up || key == Key::down)");
  Expect(Has(press, "else if (settings_row_ == 14) {") && Has(press, "settings_.bug_report = settings_.bug_report ? 0 : 1;") &&
             Has(press, "settings_.Save();"),
         "(b) the settings sheet's 15th row toggles the general value and saves it");
  const std::string sheet = Between(launcher, "void Launcher::DrawSettingsSheet(Canvas& c) {", "c.Wrapped(rows[settings_row_].about");
  Expect(Has(sheet, "{Tr(\"Problem with a game? Logs for a bug report\"), Tr(settings_.bug_report ? \"Next game\" : \"Off||plural\"),"),
         "(b) its row and value");
  Expect(Has(sheet, "for (int n = 0; n < 14; ++n) {") &&
             Has(sheet, "DrawBugReportRow(c, x, 668, rows[14].label, rows[14].value, settings_row_ == 14, settings_.bug_report != 0);"),
         "(b) drawn apart, below the 14 others");
  // (c)
  const std::string game_press = Between(launcher, "// Fix 93d: one more row after the subjects", "if (key == Key::circle) close();");
  Expect(Has(game_press, "const int categories = int(game_categories_.size()) + (identified ? 1 : 0);") &&
             Has(game_press, "if (identified && game_category_ == int(game_categories_.size())) {") &&
             Has(game_press, "overrides_[\"bug_report\"] = 1;") && Has(game_press, "overrides_.erase(\"bug_report\");") &&
             Has(game_press, "SaveGameOverrides(Selected()->title_id, overrides_);"),
         "(c) a game's row toggles its own value and saves it, for an identified game");
  const std::string game_draw = Between(launcher, "// Fix 93d: this game's next start with logs for a bug report", "c.Hint('o', Tr(\"Back\"), hint, 1004);");
  Expect(Has(game_draw, "DrawBugReportRow(c, x, y, Tr(\"Problem with this game? Logs for a bug report\"),") &&
             Has(game_draw, "Tr(armed ? \"Next start\" : \"Off||plural\")") &&
             Has(game_draw, "Tr(\"This game's next start records more details. Afterwards a screen lists the files to attach to your bug report.\")"),
         "(c) drawn apart below the subjects, with its explanation");
  const std::string row = Between(launcher, "void DrawBugReportRow(Surface& c,", "\n}\n");
  Expect(Has(row, "c.Fill(x + 40, y - 8, 780, 2, kWarning, 0.55f);") && Has(row, "c.Text(\"!\", x + 70, y + 7, 22, kWarning, 1.0f, 1);") &&
             Has(row, "c.list->AddCircle("),
         "(b, c) the row's look: a divider above it and a round \"!\" mark");
}

void CheckGame() {
  const std::string main = Read("src/canary_game_main.cpp");
  // (d)
  Expect(Has(main, "bug_report_restart };  // fix 93d") && Has(main, "bool apart = false;"), "(d) the guide's action");
  const std::string items = Between(main, "if (category == 4) {", "return items;");
  Expect(Has(items, "GuideItem restart{GuideAction::bug_report_restart, Tr(\"Restart this game with logs for a bug report\"), \"\"};") &&
             Has(items, "restart.apart = true;"),
         "(d) at the end of System's list, set apart");
  // The user's choice: a quiet button below the list, centred, not a row of it.
  Expect(Has(main, "const bool button = !items.empty() && items.back().apart;") &&
             Has(main, "const int listed = int(items.size()) - (button ? 1 : 0);") &&
             ((Has(main, "std::max(0, listed - kGuideRows)") && Has(main, "std::min(listed, first_row + kGuideRows)")) ||
              // Fix 115c (check-guide-tabs.cpp): the rows below the tab bar, one fewer above the button.
              (Has(main, "const int shown_rows = button ? kGuideRows - 1 : kGuideRows;") &&
               Has(main, "std::max(0, listed - shown_rows)") && Has(main, "std::min(listed, first_row + shown_rows)")) ||
              // Fix 115d (check-guide-counter.cpp): the button in a slot of its own after the rows.
              Has(main, "xbox360ps5::guide_quick::Window(state_.row, total, listed, shown_rows);")),
         "(d) the rows are the others; the action is not one of them (no green focus bar on it)");
  std::string draw = Between(main, "    if (button) {", "list->PopClipRect();");
  if (draw.empty()) draw = Between(main, "    if (button_shown) {", "list->PopClipRect();");  // fix 115d
  Expect((Has(draw, "const bool focused = state_.row == listed;") || Has(draw, "const bool focused = row_focus && state_.row == listed;")) &&
             Has(draw, "const float x = (page_left + strip + page_right) / 2 - width / 2 + slide_x;") &&
             (Has(draw, "top + std::min(listed - first_row, kGuideRows) * 63.0f + 24") ||
              Has(draw, "list_top + std::min(listed - first_row, shown_rows) * 63.0f + 24") ||
              Has(draw, "list_top + (listed - first_row) * 63.0f + 4")),
         "(d) a button centred below the list");
  // The approved mock-up (bug-report-guide-raised), without the "!" mark: a raised button.
  Expect(Has(draw, "for (int n = 4; n >= 1; --n)") && Has(draw, "fade(40, 44, 52, a * 7 / 100)"),
         "(d) a soft shadow below it");
  Expect(Has(draw, "list->AddRectFilled(at(x, y), at(x + width, y + height), fade(214, 217, 221, a), round * scale);") &&
             Has(draw, "ImDrawFlags_RoundCornersTop") &&
             Has(draw, "list->AddRectFilledMultiColor(at(x, y + round), at(x + width, y + height - round), fade(246, 247, 249, a),"),
         "(d) a light-to-grey gradient face with rounded corners");
  Expect(Has(draw, "list->AddLine(at(x + round, y + 2), at(x + width - round, y + 2), fade(255, 255, 255,"),
         "(d) a highlight along its top");
  Expect(Has(draw, "if (focused) list->AddRect(at(x, y), at(x + width, y + height), fade(70, 76, 84, a), round * scale, 0, 3.0f * scale);") &&
             Has(draw, "(width * scale - size.x) / 2") && Has(draw, "fade(44, 48, 54, a), label);"),
         "(d) dark text centred (never greyed out); the dark outline only when focused");
  Expect(!Has(draw, "\"!\"") && !Has(draw, "AddCircle(") && !Has(draw, "fade(118, 188, 62") && !Has(draw, "fade(200, 120, 40"),
         "(d) no mark, none of the green or orange");
  std::string action = Between(main, "case GuideAction::bug_report_restart:", "case GuideAction::network: {");
  Expect(Has(action, "if (!(pressed & 0x4000)) break;"), "(d) Cross only");
  // Fix 115 (check-guide-quick.cpp): the restart is a helper shared with "Restart game"; with logs
  // it sets the same values (guide_quick::RestartFlags), and restart_into_game writes launch-once.txt
  // (or, after a title switch, the switch's launch data again).
  const bool shared = Has(action, "restart_this_game(true);");
  if (shared) {
    const std::string helper = Between(main, "const auto restart_this_game = [&](bool with_logs) {", "};");
    const std::string into = Between(main, "const auto restart_into_game = [&] {", "};");
    action = helper + into;
    Expect(Has(helper, "RestartFlags(with_logs, !settings_title.empty())") && Has(helper, "restart_into_game();"),
           "(d) the shared restart: the flags of a restart with logs, then restart_into_game");
  }
  Expect(Has(action, "overrides[\"bug_report\"] = 1;") && Has(action, "xbox360ps5::SaveGameOverrides(settings_title, overrides);") &&
             Has(action, "global.bug_report = 1;"),
         "(d) this game's own value (the general one without a title ID)");
  Expect(Has(action, "std::ofstream(storage / \"launch-once.txt\", std::ios::trunc)") &&
             Has(action, "xbox360ps5::forward::OnceText(game.string(), settings_title, forwarded, exit_after_game)") &&
             Has(action, "restart_same_game = true;") && Has(action, "restart = true;"),
         "(d) then this game again from its start");
  Expect(Has(main, "leaving::Show(restart ? (restart_same_game ? leaving::Kind::restart_game : leaving::Kind::menu)"),
         "(d) with \"Restarting the game...\"");
  // (e)
  const std::string start = Between(main, "if (settings.bug_report) {", "if (game_log->session.Begin(log_root, chosen.name");
  Expect(Has(start, "if (global.bug_report) {") && Has(start, "global.bug_report = 0;") &&
             Has(start, "if (overrides.erase(\"bug_report\")) xbox360ps5::SaveGameOverrides(settings_title, overrides);") &&
             Has(start, "bug_report::active.store(true);"),
         "(e) a start takes the game's own value or the general one, and clears both");
}

std::map<std::string, std::string> Language(const fs::path& file) {
  std::map<std::string, std::string> table;
  std::istringstream lines(Read(file));
  const auto unescape = [](std::string text) {
    std::string out;
    for (size_t n = 0; n < text.size(); ++n) {
      if (text[n] == '\\' && n + 1 < text.size() && text[n + 1] == 's') { out += ' '; ++n; }
      else out += text[n];
    }
    return out;
  };
  for (std::string line; std::getline(lines, line);) {
    const size_t equals = line.find(" = ");
    if (equals != std::string::npos) table[unescape(line.substr(0, equals))] = unescape(line.substr(equals + 3));
  }
  return table;
}
void CheckLanguages(const fs::path& folder) {
  const std::vector<std::string> keys = {
      "Problem with a game? Logs for a bug report",
      "The next game you start records more details. Afterwards a screen lists the files to attach to your bug report.",
      "Problem with this game? Logs for a bug report", "Next start",
      "This game's next start records more details. Afterwards a screen lists the files to attach to your bug report.",
      "Restart this game with logs for a bug report"};
  int languages = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++languages;
    const std::string code = entry.path().stem().string();
    const auto table = Language(entry.path());
    for (const auto& key : keys) {
      const auto found = table.find(key);
      Expect(found != table.end() && !found->second.empty(), "(f) " + code + " has: " + key);
      if (found != table.end() && code != "en") Expect(found->second != key, "(f) " + code + " translates: " + key);
    }
  }
  Expect(languages >= 16, "(f) every language file: " + std::to_string(languages));
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: check-bug-report-entries <tools/source-1.0.2/english/lang>\n");
    return 2;
  }
  CheckLauncher();
  CheckGame();
  CheckLanguages(argv[1]);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-bug-report-entries: all checks passed\n");
  return 0;
}
