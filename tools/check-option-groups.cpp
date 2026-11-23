// SPDX-License-Identifier: MIT
// Fix 149 (tools/source-1.0.2/fixes/149-option-groups.patch): the launcher's and the guide's options
// in two groups players cannot take for each other. A player set GTA IV's "Emulated video rate" to
// 120 Hz in the guide, taking it for the TV's rate; it is the emulated console's vblank clock and
// some games run fast with it. Audit: research/option-groups/AUDIT.md.
//  (a) option_groups.hpp: every option of the launcher's table has a group (a new option fails here
//      until it is given one), no key twice; the groups are the audit's: Picture and smoothness only
//      changes the finished picture, Emulation (advanced) how the emulated console runs the game
//      (the video rate, VSync, the resolution the game is told and every former Performance option);
//  (b) the rules: Category moves picture options to page 0 and emulation options to page 1, the
//      others keep their row's; Label and About replace only the texts the table names; the pages'
//      order (Emulation (advanced) last), Position, SortByOrder, the warning's countdown;
//  (c) the guide as the tree's table and rules give it: the Picture and smoothness page has no
//      emulation option (the video rate is on the Emulation (advanced) page), every live option of
//      the old Performance page is on the Emulation (advanced) page;
//  (d) the launcher's code: Options() regroups through option_groups.hpp, the settings sheet's rows
//      are the pages in kOrder with the categories' names, frame generation and its output on the
//      Picture and smoothness page and the general FSI row on Emulation (advanced), a game's sheet
//      sorted by kOrder, the category names;
//  (e) the guide's code: categories in kOrder, the notice row first on Emulation (advanced) and
//      inert, the warning after an emulation option's change (logged), the phone page's order;
//  (f) the new texts in all 16 language files of the argument's folder, translated (not en).
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
  for (char c : s) if (c != '\r') out += c;  // canary_game_main.cpp and launcher.hpp have CR LF lines.
  return out;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}

struct Row {
  std::string key, when;
  int category = 0;
};
// The launcher's option rows as the fixes wrote them (the category before the regrouping).
std::vector<Row> Table(const std::string& launcher) {
  std::vector<Row> rows;
  const std::regex row("\\{\"([a-z_0-9]+)\", &S::[a-z_0-9]+, (-?[0-9]+), W::([a-z]+), [0-9]+, (true|false),");
  for (std::sregex_iterator it(launcher.begin(), launcher.end(), row), end; it != end; ++it)
    rows.push_back({(*it)[1].str(), (*it)[3].str(), std::stoi((*it)[2].str())});
  return rows;
}

const std::set<std::string> kPictureKeys = {"image_filter", "antialiasing", "color_preset", "overscan_crop", "sharpness",
                                            "dither", "stretch", "anisotropic", "present_limit", "draw_scale"};
const std::set<std::string> kEmulationKeys = {
    "renderer",     "internal_resolution", "video_clock",     "vsync",           "gamma",      "depth_precision",
    "decal_bias",   "mulsc",               "fast_locks",      "memory_boost",    "dynamic_buffers",
    "memory_window", "occlusion",          "readback",        "memexport",       "async_shaders", "clear_pages",
    "released_memory", "guest_yield",      "threaded_driver", "pipeline_threads", "fast_dot",  "no_promotion",
    "zero_page",    "fsi"};

void CheckTable(const std::vector<Row>& rows) {
  Expect(rows.size() >= 50, "(a) the launcher's option table is read (" + std::to_string(rows.size()) + " rows)");
  std::set<std::string> keys, seen;
  for (const auto& entry : og::kEntries) {
    Expect(seen.insert(entry.key).second, std::string("(a) ") + entry.key + " is in the table once");
    if (entry.group == og::Group::picture) Expect(kPictureKeys.count(entry.key) == 1, std::string("(a) ") + entry.key + ": Picture and smoothness as the audit says");
    if (entry.group == og::Group::emulation) Expect(kEmulationKeys.count(entry.key) == 1, std::string("(a) ") + entry.key + ": Emulation (advanced) as the audit says");
  }
  for (const auto& key : kPictureKeys) Expect(og::Find(key) && og::Find(key)->group == og::Group::picture, "(a) " + key + " is a picture option");
  for (const auto& key : kEmulationKeys) Expect(og::Find(key) && og::Find(key)->group == og::Group::emulation, "(a) " + key + " is an emulation option");
  for (const auto& row : rows) {
    keys.insert(row.key);
    Expect(og::Find(row.key) != nullptr, "(a) " + row.key + " has a group in option_groups.hpp (a new option needs one)");
    // Every option of the old Performance page is emulation; the old Video page holds no "other".
    if (row.category == 1) Expect(og::Category(row.key, row.category) == og::kEmulation, "(a) " + row.key + " (Performance) stays emulation");
    if (row.category == 0) Expect(og::Find(row.key) && og::Find(row.key)->group != og::Group::other, "(a) " + row.key + " (Video) is picture or emulation");
  }
  // The table may name present_limit before fix 146 is in the tree; nothing else it does not have.
  for (const auto& entry : og::kEntries)
    Expect(keys.count(entry.key) || std::strcmp(entry.key, "present_limit") == 0,
           std::string("(a) ") + entry.key + " is an option of the launcher's table");
}

void CheckRules() {
  Expect(og::Category("video_clock", 0) == og::kEmulation, "(b) Emulated video rate goes to Emulation (advanced)");
  Expect(og::Category("vsync", 0) == og::kEmulation, "(b) VSync (the emulated vblank pacing) goes to Emulation (advanced)");
  Expect(og::Category("internal_resolution", 0) == og::kEmulation && og::Category("renderer", 0) == og::kEmulation,
         "(b) the resolution the game is told and the renderer go to Emulation (advanced)");
  Expect(og::Category("antialiasing", 0) == og::kPicture && og::Category("draw_scale", 0) == og::kPicture,
         "(b) anti-aliasing and the resolution scale stay on Picture and smoothness");
  Expect(og::Category("readback", 1) == og::kEmulation, "(b) a Performance option stays on its page (1)");
  Expect(og::Category("volume", 2) == 2 && og::Category("save_drive", 4) == 4 && og::Category("bug_report", -1) == -1,
         "(b) the others keep their row's category, -1 too");
  Expect(og::Category("not_an_option", 3) == 3, "(b) an unknown key keeps its row's category");
  Expect(std::strcmp(og::Label("video_clock", "Emulated video rate"), "Emulated video rate (game clock, not your TV)") == 0,
         "(b) the video rate's label says it is the game's clock, not the TV");
  Expect(std::strcmp(og::Label("vsync", "VSync"), "VSync (emulated, game speed)") == 0, "(b) VSync's label");
  Expect(std::strcmp(og::Label("readback", "Image readback by the game"), "Image readback by the game") == 0,
         "(b) other labels are the table's");
  Expect(Has(og::About("video_clock", "x"), "not your TV's refresh rate") && Has(og::About("video_clock", "x"), "Frame generation output"),
         "(b) the video rate's help points to the TV's real rate");
  Expect(Has(og::About("internal_resolution", "x"), "not your TV's resolution"), "(b) the emulated resolution is not the TV's");
  Expect(Has(og::About("vsync", "x"), "not your TV's"), "(b) VSync is not the TV's");
  Expect(std::strcmp(og::About("fast_locks", "own"), "own") == 0, "(b) other help texts are the table's");
  Expect(std::size(og::kOrder) == 5 && og::kOrder[0] == og::kPicture && og::kOrder[4] == og::kEmulation,
         "(b) five pages: Picture and smoothness first, Emulation (advanced) last");
  std::set<int> pages(std::begin(og::kOrder), std::end(og::kOrder));
  Expect(pages == std::set<int>{0, 1, 2, 3, 4}, "(b) each category once");
  Expect(og::Position(0) == 0 && og::Position(2) == 1 && og::Position(3) == 2 && og::Position(4) == 3 && og::Position(1) == 4 &&
             og::Position(-1) == -1,
         "(b) the pages' places; no category stays -1");
  std::vector<int> categories = {0, 1, 2, 3, 4};
  og::SortByOrder(categories);
  Expect(categories == std::vector<int>{0, 2, 3, 4, 1}, "(b) a game's subjects in the pages' order");
  Expect(og::ChangedLeft(og::kChangedSeconds, 0.1f) < og::kChangedSeconds && og::ChangedLeft(0.05f, 0.1f) == 0.0f &&
             og::ChangedLeft(0.0f, 0.0f) == 0.0f,
         "(b) the warning counts down to 0");
  Expect(og::kChangedSeconds >= 5.0f && og::kChangedSeconds <= 12.0f, "(b) the warning stays up a few seconds");
}

void CheckGuidePages(const std::vector<Row>& rows) {
  // GuideItems' `shown`: live, not the logs.
  std::set<std::string> picture, emulation;
  for (const auto& row : rows) {
    if (row.when != "live" || row.key == "detailed_logs" || row.key == "bug_report") continue;
    const int page = og::Category(row.key, row.category);
    if (page == og::kPicture) picture.insert(row.key);
    if (page == og::kEmulation) emulation.insert(row.key);
    if (row.category == 1) Expect(page == og::kEmulation, "(c) " + row.key + " is on the guide's Emulation (advanced) page");
  }
  Expect(!picture.count("video_clock") && emulation.count("video_clock"),
         "(c) the guide's Picture and smoothness page no longer has the emulated video rate");
  for (const auto& key : picture) Expect(kPictureKeys.count(key) == 1, "(c) " + key + " on the guide's picture page is a picture option");
  for (const char* key : {"image_filter", "antialiasing", "color_preset", "overscan_crop", "anisotropic"})
    Expect(picture.count(key) == 1, std::string("(c) ") + key + " is on the guide's Picture and smoothness page");
  for (const char* key : {"readback", "memexport", "clear_pages", "async_shaders"})
    Expect(emulation.count(key) == 1, std::string("(c) ") + key + " is on the guide's Emulation (advanced) page");
}

void CheckLauncher() {
  const std::string hpp = Read("include/xbox360ps5/launcher.hpp");
  Expect(Has(hpp, "kOptionCategories[] = {\"Picture and smoothness\", \"Emulation (advanced)\", \"Audio\", \"Controls\", \"System\"};"),
         "(d) the categories' names; 0 and 1 renamed, their numbers kept");
  const std::string launcher = Read("src/launcher.cpp");
  const std::string grouped = Between(launcher, "static const std::vector<Option> grouped = [] {", "return grouped;");
  Expect(Has(grouped, "option.category = option_groups::Category(option.key, option.category);") &&
             Has(grouped, "option.label = option_groups::Label(option.key, option.label);") &&
             Has(grouped, "option.about = option_groups::About(option.key, option.about);"),
         "(d) Options() regroups every row through option_groups.hpp");
  Expect(!Has(launcher, "  };\n  return options;\n}"), "(d) the table is not returned unregrouped");
  Expect(Has(launcher, "category_ = option_groups::kOrder[settings_row_ - 2];"), "(d) the settings rows open the pages in kOrder");
  const std::string sheet = Between(launcher, "// Fix 149: the pages in option_groups::kOrder", "{Tr(\"Restore recommended\")");
  const char* names[] = {"Picture and smoothness", "Audio", "Controls", "System", "Emulation (advanced)"};
  size_t at = 0;
  for (int n = 0; n < 5; ++n) {
    const size_t found = sheet.find(std::string("{Tr(\"") + names[n] + "\"), changed(", at);
    Expect(found != std::string::npos, std::string("(d) the settings row ") + std::to_string(n + 2) + " is " + names[n]);
    if (found != std::string::npos) at = found;
  }
  Expect(!Has(sheet, "{Tr(\"Video\")") && !Has(sheet, "{Tr(\"Performance\")"), "(d) no Video or Performance row any more");
  const std::string rows = Between(launcher, "category_ = option_groups::kOrder[settings_row_ - 2];", "mode_ = Mode::options;");
  // Fix 149b: the rows come from the settings tree, where frame generation and its output are rows of
  // Picture and smoothness's "Frame rate and 120 Hz" and the general FSI switch of Emulation (advanced)'s
  // "Renderer and accuracy" (check-settings-tree.cpp).
  const bool tree = Has(rows, "category_rows_ = TreeRows(category_, option_groups::kNoSection, false);") &&
                    Has(launcher, "if (key == option_groups::kFramegenRowKey) { if (!game) rows.push_back(kFramegenRow); return; }") &&
                    Has(launcher, "if (key == option_groups::kFsiRowKey) { if (!game) rows.push_back(kFsiRow); return; }");
  Expect((tree || (Has(rows, "if (category_ == option_groups::kPicture) {\n              category_rows_.push_back(kFramegenRow);\n"
                             "              category_rows_.push_back(kFramegenOutputRow);\n            }") &&
                   Has(rows, "if (category_ == option_groups::kEmulation) category_rows_.push_back(kFsiRow);"))) &&
             !Has(rows, "category_ == 1"),
         "(d) frame generation and its output on Picture and smoothness, the general FSI row on Emulation (advanced)");
  Expect(Has(launcher, "option_groups::SortByOrder(game_categories_);"), "(d) a game's sheet in the pages' order");
  Expect(!Has(launcher, "category == 0 ? \"Graphics\" : kOptionCategories[category]"), "(d) a game's sheet names the pages as the settings do");
  Expect(Has(launcher, "#include \"xbox360ps5/option_groups.hpp\""), "(d) the launcher includes option_groups.hpp");
}

void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string items = Between(main, "std::vector<GuideItem> GuideItems(int page,", "\n}\n");
  Expect(Has(items, "for (int position = 0; position < categories; ++position) {\n"
                    "      const int c = xbox360ps5::option_groups::kOrder[position];"),
         "(e) the guide lists the categories in the pages' order");
  const std::string page = Between(items, "if (category == xbox360ps5::option_groups::kEmulation) {", "  }\n");
  const size_t notice = page.find("items.push_back({GuideAction::notice, Tr(xbox360ps5::option_groups::kGuideNotice), \"\"});");
  const size_t measure = page.find("items.push_back({GuideAction::measure, Tr(\"Measure performance (10 s)\"), \"\"});");
  Expect(notice != std::string::npos && measure != std::string::npos && notice < measure,
         "(e) Emulation (advanced) opens with the notice, then the measurement");
  Expect(!Has(main, "kOptionCategories[category], \"Performance\""), "(e) no page found by the name Performance");
  Expect(Has(main, "                         notice,  // fix 149") && Has(main, "bug_report_restart };  // fix 93d"),
         "(e) the notice action (fix 93d's line as it was)");
  Expect(Has(main, "case GuideAction::notice: changed = false; break;"), "(e) the notice row does nothing when pressed");
  Expect(Has(main, "return item.action == GuideAction::option || item.action == GuideAction::network;"),
         "(e) left and right do nothing on it");
  const std::string draw = Between(main, "if (items[size_t(n)].action == GuideAction::notice) {", "continue;");
  // Fix 149b: one short line, large, instead of two small wrapped ones.
  Expect(Has(draw, "items[size_t(n)].label.c_str(), nullptr, (page_right - page_left - strip - 76) * scale);") ||
             Has(draw, "list->AddText(large, 28 * scale, at(page_left + strip + 56 + slide_x, y + 16), fade(60, 44, 10, a), items[size_t(n)].label.c_str());"),
         "(e) the notice is drawn over its row");
  const std::string option = Between(main, "case GuideAction::option: {", "case GuideAction::capture:");
  const std::string warn = Between(option, "if (option.category == xbox360ps5::option_groups::kEmulation) {", "\n                  }");
  Expect(Has(warn, "guide.group_changed = xbox360ps5::option_groups::kChangedSeconds;") &&
             Has(warn, "is an emulation option (fix 149)"),
         "(e) an emulation option changed in the guide raises the warning and is logged");
  Expect(option.find("XELOGW(\"Guide: {} set to {}") < option.find("guide.group_changed"), "(e) after the change and its log line");
  const std::string toast = Between(main, "state_.group_changed = xbox360ps5::option_groups::ChangedLeft(state_.group_changed, frame_time);",
                                    "// Fix 115d: \"row / rows\"");
  Expect(Has(toast, "if (state_.group_changed > 0.0f) {") && Has(toast, "Tr(xbox360ps5::option_groups::kGuideChanged)"),
         "(e) the warning is drawn over the list while its time lasts");
  // After the list's clip (fix 93d's button region, check-bug-report-entries.cpp, ends at it).
  Expect(Has(main, "                                    fade(0, 0, 0, 70), fade(0, 0, 0, 70));\n    list->PopClipRect();\n"
                   "    // Fix 149: after an Emulation (advanced) option is changed"),
         "(e) drawn after the list's clip, outside fix 93d's button region");
  Expect(Has(main, "float group_changed = 0.0f;"), "(e) the guide's state holds the warning's time");
  const std::string web = Between(main, "std::string WebState(", "json += \"],\\\"games\\\":[\";");
  Expect(Has(web, "JsonText(Tr(xbox360ps5::kOptionCategories[groups::kOrder[n]]))") &&
             Has(web, "std::to_string(groups::Position(option.category))"),
         "(e) the phone page's tabs in the pages' order, each option on its page's tab");
}

void CheckLanguages(const fs::path& folder) {
  const char* keys[] = {"Picture and smoothness", "Emulation (advanced)", og::kVideoRateLabel, og::kVideoRateAbout,
                        og::kVsyncLabel, og::kVsyncAbout, og::kResolutionAbout};
  // The guide's notice and warning: fix 149b made them short; check-settings-tree.cpp checks their translations.
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
    for (const char* key : keys) {
      std::string escaped;
      for (const char* c = key; *c; ++c) escaped += *c == ' ' ? std::string("\\s") : std::string(1, *c);
      const auto found = table.find(escaped);
      Expect(found != table.end() && !found->second.empty(), "(f) " + code + " has " + std::string(key).substr(0, 40));
      if (found != table.end() && code != "en") Expect(found->second != escaped, "(f) " + code + " translates " + std::string(key).substr(0, 40));
    }
  }
  Expect(files == 16, "(f) the 16 language files");
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<Row> rows = Table(Read("src/launcher.cpp"));
  CheckTable(rows);
  CheckRules();
  CheckGuidePages(rows);
  CheckLauncher();
  CheckGuide();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "(f) the language folder is the argument");
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-option-groups: all checks passed\n");
  return 0;
}
