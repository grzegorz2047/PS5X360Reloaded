// SPDX-License-Identifier: MIT
// Fix 149b (tools/source-1.0.2/fixes/149b-settings-tree.patch): the settings as a menu of several
// levels. On 1.0.6-work-fix149 the user found the launcher's settings and a game's settings unreadable:
// long flat pages, and a game's subjects switched with Triangle ("Next subject"). Now Picture and
// smoothness and Emulation (advanced) open a short list of sections, each a short list of options;
// Audio, Controls and System stay one list; Settings > Game settings and Settings > Library and saves are
// rows of the top list (no L1/R1 tabs); a title says where each list is; the guide shows the sections
// as headers and the phone page as headings.
//  (a) option_groups.hpp's tree: every Picture and smoothness and Emulation (advanced) option is in one
//      section of its page, once; the launcher's own rows (frame generation, its output, the general FSI
//      switch) in theirs; four and five sections, each a short list (at most 7 rows);
//  (b) the helpers: SectionOf, SectionsOf, HasSections, SectionKeys, TreeRank, Path, SkipHeader;
//  (c) the launcher: the rows of a list from the tree (TreeRows), the sections first on a long page,
//      Circle one level up, the top list's 13 rows with Game settings and Library and saves, the
//      library's four rows, no tabs, no "Next subject", a game's sections, the game chosen in the
//      library first, the titles (Path);
//  (d) the guide: sections' names as headers the focus passes, in the launcher's order;
//  (e) the phone page: each option's section, a heading where a section starts;
//  (f) the new texts in all 16 language files of the argument's folder, translated (not en);
//  (g) the help card (the user, after 1.0.6-work-fix149: "a small window on the right of the selected
//      option"): every option the guide shows has a short text; the notice and the warning after a change
//      are one short line each; the card beside the guide's focused option and the launcher's help show
//      the name, the choices, the short text and, for an Emulation (advanced) option, the risk.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/option_groups.hpp"

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
  for (char c : s) if (c != '\r') out += c;  // canary_game_main.cpp, launcher.hpp, web_settings.cpp: CR LF.
  return out;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}

void CheckTree() {
  std::map<std::string, int> seen;
  for (const auto& entry : og::kTree) {
    ++seen[entry.key];
    Expect(entry.section >= 0 && entry.section < int(std::size(og::kSections)), std::string("(a) ") + entry.key + " is in a section");
    if (og::IsLauncherRow(entry.key)) continue;
    const og::Entry* option = og::Find(entry.key);
    Expect(option && option->group != og::Group::other, std::string("(a) ") + entry.key + " is a picture or emulation option");
    if (option)
      Expect(og::Category(entry.key, -1) == og::kSections[entry.section].category,
             std::string("(a) ") + entry.key + "'s section is on its page");
  }
  for (const auto& entry : og::kEntries)
    if (entry.group != og::Group::other)
      Expect(seen[entry.key] == 1, std::string("(a) ") + entry.key + " is in one section, once");
    else
      Expect(seen.count(entry.key) == 0, std::string("(a) ") + entry.key + " (Audio, Controls, System) has no section");
  for (const char* row : {og::kFramegenRowKey, og::kFramegenOutputRowKey, og::kFsiRowKey})
    Expect(seen[row] == 1, std::string("(a) the launcher's row ") + row + " is in one section");
  Expect(og::SectionOf(og::kFramegenRowKey) == og::kFrameRate && og::SectionOf(og::kFramegenOutputRowKey) == og::kFrameRate &&
             og::SectionOf("present_limit") == og::kFrameRate,
         "(a) frame generation, its output (the TV's rate) and the frame rate limit together");
  Expect(og::SectionOf(og::kFsiRowKey) == og::kRendererAccuracy && og::SectionOf("fsi") == og::kRendererAccuracy,
         "(a) both FSI rows in Renderer and accuracy");
  Expect(og::SectionOf("video_clock") == og::kVideoClock && og::SectionOf("vsync") == og::kVideoClock &&
             og::SectionOf("internal_resolution") == og::kVideoClock,
         "(a) the emulated video rate, VSync and resolution in Video clock and mode");
  Expect(og::SectionsOf(og::kPicture).size() == 4 && og::SectionsOf(og::kEmulation).size() == 5,
         "(a) four sections of Picture and smoothness, five of Emulation (advanced)");
  for (int section = 0; section < int(std::size(og::kSections)); ++section) {
    const size_t rows = og::SectionKeys(section).size();
    Expect(rows >= 2 && rows <= 7, std::string("(a) ") + og::SectionName(section) + " is a short list (" + std::to_string(rows) + ")");
  }
}

void CheckHelpers() {
  Expect(!og::HasSections(2) && !og::HasSections(3) && !og::HasSections(4) && og::HasSections(og::kPicture) &&
             og::HasSections(og::kEmulation),
         "(b) Audio, Controls and System are one list each");
  Expect(og::SectionOf("volume") == og::kNoSection && og::SectionOf("unknown") == og::kNoSection, "(b) no section");
  const auto aa = og::SectionKeys(og::kAaSharpness);
  Expect(aa.size() == 4 && std::strcmp(aa[0], "antialiasing") == 0 && std::strcmp(aa[1], "image_filter") == 0,
         "(b) a section's rows in their order");
  Expect(std::strcmp(og::SectionName(og::kColorsFit), "Colors and screen fit") == 0 && std::strcmp(og::SectionName(-1), "") == 0 &&
             std::strcmp(og::SectionName(99), "") == 0,
         "(b) section names, none out of range");
  Expect(og::TreeRank("antialiasing") < og::TreeRank("image_filter") && og::TreeRank("image_filter") < og::TreeRank("color_preset") &&
             og::TreeRank("color_preset") < og::TreeRank("video_clock") && og::TreeRank("volume") == og::TreeRank("unknown") &&
             og::TreeRank("zero_page") < og::TreeRank("volume"),
         "(b) the tree's order, rows without a section after");
  Expect(og::Path({"Settings", "Picture and smoothness", "Colors and screen fit"}) ==
             "Settings  >  Picture and smoothness  >  Colors and screen fit",
         "(b) the title joins the levels");
  Expect(og::Path({"Picture and smoothness", ""}) == "Picture and smoothness" && og::Path({}) == "", "(b) an empty level is left out");
  const std::vector<bool> header = {true, false, false, true, false};
  const auto is = [&header](int r) { return bool(header[size_t(r)]); };
  Expect(og::SkipHeader(0, 1, 5, is) == 1, "(b) Down onto the first header: the row below it");
  Expect(og::SkipHeader(3, 1, 5, is) == 4 && og::SkipHeader(3, -1, 5, is) == 2, "(b) a header between rows: passed the way the pad went");
  Expect(og::SkipHeader(0, -1, 5, is) == -1, "(b) Up onto the first header: the tab bar");
  Expect(og::SkipHeader(2, 1, 5, is) == 2, "(b) a row that is no header stays");
  const std::vector<bool> tail = {false, true};
  Expect(og::SkipHeader(1, 1, 2, [&tail](int r) { return bool(tail[size_t(r)]); }) == 0, "(b) a header at the end: back to the last row");
}

void CheckLauncher() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::string hpp = Read("include/xbox360ps5/launcher.hpp");
  const std::string tree = Between(launcher, "std::vector<int> TreeRows(int category, int section, bool game) {\n", "\n}\n");
  Expect(Has(tree, "if (key == option_groups::kFramegenRowKey) { if (!game) rows.push_back(kFramegenRow); return; }") &&
             Has(tree, "if (key == option_groups::kFramegenOutputRowKey) { if (!game) rows.push_back(kFramegenOutputRow); return; }") &&
             Has(tree, "if (key == option_groups::kFsiRowKey) { if (!game) rows.push_back(kFsiRow); return; }") &&
             Has(tree, "for (const char* key : option_groups::SectionKeys(section)) add(key);") &&
             Has(tree, "(!game || options[size_t(n)].per_game)"),
         "(c) a list's rows from the tree; a game's only those it may own, without the launcher's rows");
  const std::string options = Between(launcher, "    case Mode::options: {\n", "    case Mode::saves: {");
  Expect(Has(options, "if (option_groups::HasSections(category_) && section_ == option_groups::kNoSection) {") &&
             Has(options, "section_ = sections[size_t(section_row_)];") && Has(options, "category_rows_ = TreeRows(category_, section_, false);"),
         "(c) a long page lists its sections; X opens one");
  Expect(Has(options, "if (key == Key::circle && section_ != option_groups::kNoSection) section_ = option_groups::kNoSection;"),
         "(c) Circle in a section goes back to the sections");
  const std::string settings = Between(launcher, "    case Mode::settings: {\n", "    case Mode::options: {\n");
  Expect(!Has(settings, "settings_tab_ = 1 - settings_tab_;") && !Has(launcher, "const char* const tabs[] = {\"General\", \"Per game\"};"),
         "(c) no L1/R1 tabs in the settings");
  Expect(Has(settings, "const int rows = 13;") &&
             Has(settings, "else if (settings_row_ == 7 && key == Key::cross) { RefreshPerGame(); per_game_row_ = 0; settings_tab_ = 1; }") &&
             Has(settings, "else if (settings_row_ == 8 && key == Key::cross) { library_row_ = 0; settings_tab_ = 2; }"),
         "(c) Game settings and Library and saves are rows of the top list");
  Expect(Has(settings, "if (key == Key::circle) settings_tab_ = 0;") && Has(settings, "if (key == Key::square) CloseSettings();"),
         "(c) Circle goes up from a level, Square closes the settings");
  const std::string library = Between(settings, "if (settings_tab_ == 2) {", "break;");
  Expect(Has(library, "if (library_row_ == 0) StartCoverDownload();") && Has(library, "mode_ = Mode::paths;") &&
             Has(library, "RefreshLibrary(false);") && Has(library, "mode_ = Mode::saves;"),
         "(c) Library and saves: covers, folders, refresh, saved data");
  const std::string sheet = Between(launcher, "void Launcher::DrawSettingsSheet(Canvas& c) {", "c.Wrapped(rows[settings_row_].about");
  size_t at = 0;
  for (const char* name : {"Player profile", "Language", "Picture and smoothness", "Audio", "Controls", "System", "Emulation (advanced)",
                           "Game settings", "Library and saves", "Restore recommended", "Theme", "Restart emulator",
                           "Problem with a game? Logs for a bug report"}) {
    const size_t found = sheet.find(std::string("{Tr(\"") + name + "\")", at);
    Expect(found != std::string::npos, std::string("(c) the top list's row ") + name + ", in order");
    if (found != std::string::npos) at = found;
  }
  Expect(!Has(sheet, "{Tr(\"Download covers\")"), "(c) the library's rows left the top list");
  Expect(Has(launcher, "void Launcher::DrawLibraryMenu(Canvas& c, float x) {") && Has(launcher, "if (settings_tab_ == 2) { DrawLibraryMenu(c, x); return; }"),
         "(c) the library's level is drawn");
  Expect(!Has(launcher, "Tr(\"Next subject\")") && !Has(launcher, "// The next subject."), "(c) no hidden Next subject on Triangle");
  const std::string game = Between(launcher, "        // Fix 149b: a subject with sections: X opens one", "        break;\n        }");
  Expect(Has(game, "game_section_ = game_sections_[size_t(game_section_row_)];") && Has(game, "if (key == Key::circle) game_category_open_ = false;"),
         "(c) a game's subject lists its sections; X opens one, Circle goes back");
  Expect(Has(launcher, "if (game_section_ != option_groups::kNoSection) game_section_ = option_groups::kNoSection;\n          else game_category_open_ = false;"),
         "(c) Circle in a game's section goes back to its sections");
  Expect(Has(launcher, "if (!TreeRows(category, section, true).empty()) game_sections_.push_back(section);"),
         "(c) a game's subject lists only sections with options it may own");
  Expect(Has(launcher, "std::rotate(per_game_.begin(), per_game_.begin() + std::ptrdiff_t(n), per_game_.begin() + std::ptrdiff_t(n) + 1);"),
         "(c) Game settings: the game chosen in the library first");
  Expect(Has(launcher, "c.Text(option_groups::Path({Tr(\"Settings\"), Tr(kOptionCategories[category_]),") &&
             Has(launcher, "c.Text(option_groups::Path({Tr(kOptionCategories[category]),") &&
             Has(launcher, "c.Text(option_groups::Path({Tr(\"Settings\"), name}), x + 56, 30, 22, kMuted, 1.0f, 0, 750);"),
         "(c) each list says where it is");
  Expect(Has(hpp, "int section_ = -1, section_row_ = 0;") && Has(hpp, "int game_section_ = -1, game_section_row_ = 0;") &&
             Has(hpp, "int library_row_ = 0;") && Has(hpp, "bool GameSectionList() const;"),
         "(c) the launcher's state for the levels");
}

void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  Expect(Has(main, "                         section,  // fix 149b"), "(d) the header action");
  const std::string items = Between(main, "std::vector<GuideItem> GuideItems(int page,", "\n}\n");
  Expect(Has(items, "for (const int section : xbox360ps5::option_groups::SectionsOf(category)) {") &&
             Has(items, "for (const char* key : xbox360ps5::option_groups::SectionKeys(section))") &&
             Has(items, "if (!header) items.push_back({GuideAction::section, Tr(xbox360ps5::option_groups::SectionName(section)), \"\"});"),
         "(d) a page with sections: each section's name, then its options in the launcher's order");
  Expect(Has(items, "if (sections && xbox360ps5::option_groups::SectionOf(option.key) != xbox360ps5::option_groups::kNoSection) continue;"),
         "(d) an option is listed once");
  Expect(Has(main, "case GuideAction::section: changed = false; break;"), "(d) a header does nothing");
  Expect(Has(main, "const int row = xbox360ps5::option_groups::SkipHeader(guide.row, (pressed & 0x10) ? -1 : 1, rows,") &&
             Has(main, "if (row < 0) { guide.on_bar = true; guide.row = 0; } else guide.row = row;"),
         "(d) Up and Down pass a header (Up past the first: the tab bar)");
  Expect(Has(main, "guide.row = std::max(0, xbox360ps5::option_groups::SkipHeader(0, 1, int(opened.size()),"),
         "(d) a page opens on its first row that is not a header");
  const std::string draw = Between(main, "if (items[size_t(n)].action == GuideAction::section) {", "continue;");
  Expect(Has(draw, "items[size_t(n)].label.c_str()"), "(d) a header is drawn as its name");
  const std::string web = Between(main, "std::string WebState(", "json += \"],\\\"games\\\":[\";");
  Expect(Has(web, "return groups::TreeRank(a->key) < groups::TreeRank(b->key);") && Has(web, ",\\\"section\\\":"),
         "(e) the phone page gets the options in the tree's order, each with its section");
  const std::string page = Read("platform/ps5/web_settings.cpp");
  Expect(Has(page, ".map((o,i,list)=>{") && Has(page, "(o.section&&(!i||list[i-1].section!=o.section)?'<h3>'+esc(o.section)+'</h3>':'')"),
         "(e) the phone page puts a heading where a section starts");
}

void CheckHelp() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::regex row("\\{\"([a-z_0-9]+)\", &S::[a-z_0-9]+, (-?[0-9]+), W::([a-z]+), [0-9]+, (true|false),");
  int live = 0;
  for (std::sregex_iterator it(launcher.begin(), launcher.end(), row), end; it != end; ++it) {
    const std::string key = (*it)[1].str();
    if ((*it)[3].str() != "live" || key == "detailed_logs" || key == "bug_report") continue;
    ++live;
    Expect(og::BriefOf(key) != nullptr, "(g) " + key + " (shown in the guide) has a short text");
  }
  Expect(live >= 30, "(g) the guide's options are read (" + std::to_string(live) + ")");
  for (const auto& brief : og::kBriefs) {
    Expect(og::Find(brief.key) != nullptr, std::string("(g) ") + brief.key + " is an option");
    Expect(std::strlen(brief.text) <= 100, std::string("(g) ") + brief.key + "'s text is short");
  }
  Expect(std::strlen(og::kGuideNotice) <= 50 && std::strlen(og::kGuideChanged) <= 60 && std::strlen(og::kRiskLine) <= 70,
         "(g) the notice, the warning and the risk are one short line each");
  Expect(og::ChangesHowGameRuns("video_clock") && og::ChangesHowGameRuns("readback") && !og::ChangesHowGameRuns("antialiasing") &&
             !og::ChangesHowGameRuns("volume"),
         "(g) the risk line for Emulation (advanced) options only");
  Expect(og::JoinChoices({"Off", "FXAA", "", "SMAA 1x"}) == "Off  ·  FXAA  ·  SMAA 1x" && og::JoinChoices({}) == "",
         "(g) the choices on one line");
  Expect(og::CardTop(400, 200, 238, 860) == 400 && og::CardTop(800, 200, 238, 860) == 660 && og::CardTop(100, 200, 238, 860) == 238,
         "(g) the card beside its row, inside the band");
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string card = Between(main, "void DrawHelpCard(", "\n}\n");
  Expect(Has(card, "groups::JoinChoices(choices)") && Has(card, "groups::BriefOf(option.key)") &&
             Has(card, "groups::ChangesHowGameRuns(option.key)") && Has(card, "Tr(groups::kRiskLine)") &&
             Has(card, "text(text_font, 24, colour(44, 48, 54), Tr(brief));"),
         "(g) the card: name, choices, short text (24 px), risk");
  Expect(Has(main, "DrawHelpCard(list, medium, large, scale, reveal, *items[size_t(state_.row)].option, right + 14, state_.focus_y, 238, 860);") &&
             main.find("DrawHelpCard(list, medium") > main.find("    list->PopClipRect();\n    // Fix 149b: beside the focused option") &&
             main.find("DrawHelpCard(list, medium") < main.find("    // The 3D unfold:"),
         "(g) beside the guide, outside its clip, unfolding with it");
  Expect(Has(main, "list->AddText(large, 30 * scale, at(left + 78, bottom - 57), fade(60, 44, 10, a), Tr(xbox360ps5::option_groups::kGuideChanged));"),
         "(g) the warning after a change: one line, large");
  Expect(Has(launcher, "if (const char* brief = option_groups::BriefOf(option.key)) wrapped(Tr(brief), 22, kText);") &&
             Has(launcher, "wrapped(option_groups::JoinChoices(choices), 20, kMuted);") &&
             Has(launcher, "if (option_groups::ChangesHowGameRuns(option.key)) wrapped(Tr(option_groups::kRiskLine), 20, kWarning);") &&
             Has(launcher, "c.list->PushClipRect(c.At(x + 40, 620), c.At(x + 820, 852), true);"),
         "(g) the launcher's help: short text, choices, risk, then the long help cut above the recommended value");
}

void CheckLanguages(const fs::path& folder) {
  std::vector<std::string> keys;
  for (int section = 0; section < int(std::size(og::kSections)); ++section) keys.push_back(og::SectionName(section));
  for (const char* key : {"Library and saves",
                          "Each identified game's own settings, in the same pages as these. What a game does not change follows these general settings.",
                          "Covers, game folders, the game list and saved data.",
                          "Each game can have its own picture, audio, controls, system and emulation settings. What it does not change follows the general settings. In the library, Triangle on a game opens the same settings.",
                          "Each game can have its own value: Settings > Game settings.", og::kGuideNotice, og::kGuideChanged,
                          og::kRiskLine})
    keys.push_back(key);
  for (const auto& brief : og::kBriefs) keys.push_back(brief.text);
  int files = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++files;
    const std::string code = entry.path().stem().string();
    std::map<std::string, std::string> table;
    std::istringstream lines(Read(entry.path()));
    for (std::string line; std::getline(lines, line);) {
      const size_t split = line.find(" = ");
      if (split == std::string::npos || line[0] == '#') continue;
      table[line.substr(0, split)] = line.substr(split + 3);
    }
    for (const std::string& key : keys) {
      std::string escaped;
      for (char c : key) escaped += c == ' ' ? std::string("\\s") : std::string(1, c);
      const auto found = table.find(escaped);
      Expect(found != table.end() && !found->second.empty(), "(f) " + code + " has " + key.substr(0, 40));
      if (found != table.end() && code != "en" && key != "Processor")
        Expect(found->second != escaped, "(f) " + code + " translates " + key.substr(0, 40));
    }
  }
  Expect(files == 16, "(f) the 16 language files");
}

}  // namespace

int main(int argc, char** argv) {
  CheckTree();
  CheckHelpers();
  CheckLauncher();
  CheckGuide();
  CheckHelp();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "(f) the language folder is the argument");
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-settings-tree: all checks passed\n");
  return 0;
}
