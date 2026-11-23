// SPDX-License-Identifier: MIT
// Fix 115e (tools/source-1.0.2/fixes/115e-guide-measure-in-performance.patch): the user (Sims 3 on
// 1.0.5-work-fix125) asked for "Measure performance (10 s)" to be in the guide's Performance
// section, with the options it measures, for consistency. It was the last row of the Settings tab's
// list of categories.
//  (a) the list of categories no longer has it; the Performance category opens with it as its
//      first row, before the options; no other category has it;
//  (b) it stays an action row: no value (left and right do nothing, tab_bar's has_value is false
//      for it), Cross closes the guide and measures 4 s later, as before;
//  (c) its text is the one fix 54 made, translated in all 16 language files of the argument's folder.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

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
size_t Count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++n;
  return n;
}

void CheckGuide() {
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string launcher_hpp = Read("include/xbox360ps5/launcher.hpp");
  // Fix 149 renamed the Performance page Emulation (advanced) (option_groups.hpp: category 1 either way).
  const bool groups = Has(launcher_hpp, "kOptionCategories[] = {\"Picture and smoothness\", \"Emulation (advanced)\", \"Audio\", \"Controls\", \"System\"};");
  Expect(groups || Has(launcher_hpp, "kOptionCategories[] = {\"Video\", \"Performance\", \"Audio\", \"Controls\", \"System\"};"),
         "the categories: Performance (fix 149: Emulation (advanced)) is one of them");
  const std::string items = Between(main, "std::vector<GuideItem> GuideItems(int page,", "\n}\n");
  // (a)
  const std::string categories = Between(items, "  if (category < 0) {", "    return items;\n  }");
  Expect(!categories.empty() && !Has(categories, "GuideAction::measure"), "(a) not in the list of categories any more");
  // Fix 149: on the Emulation (advanced) page, after the notice row that only tells.
  const std::string row = groups ? "  if (category == xbox360ps5::option_groups::kEmulation) {\n"
                                   "    items.push_back({GuideAction::notice, Tr(xbox360ps5::option_groups::kGuideNotice), \"\"});\n"
                                   "    items.push_back({GuideAction::measure, Tr(\"Measure performance (10 s)\"), \"\"});"
                                 : "  if (std::strcmp(xbox360ps5::kOptionCategories[category], \"Performance\") == 0)\n"
                                   "    items.push_back({GuideAction::measure, Tr(\"Measure performance (10 s)\"), \"\"});";
  const size_t measure = items.find(row);
  const size_t options = items.find("  for (const auto& option : xbox360ps5::Options()) {\n    if (!shown(option) || option.category != category) continue;");
  Expect(measure != std::string::npos && options != std::string::npos && measure < options,
         "(a) the Performance category's first row (fix 149: after the notice), before its options");
  Expect(Count(main, "GuideAction::measure, Tr(") == 1, "(a) in one place only");
  // (b)
  Expect(Has(main, "return item.action == GuideAction::option || item.action == GuideAction::network;"),
         "(b) an action row: left and right do nothing on it");
  const std::string action = Between(main, "case GuideAction::measure:", "break;");
  Expect(Has(action, "guide.open = false;") && Has(action, "measure_at = std::chrono::steady_clock::now() + std::chrono::seconds(4);"),
         "(b) Cross closes the guide and measures, as before");
}

void CheckLanguages(const fs::path& folder) {
  int files = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++files;
    const std::string code = entry.path().stem().string();
    std::istringstream lines(Read(entry.path()));
    std::string value;
    for (std::string line; std::getline(lines, line);)
      if (line.rfind("Measure\\sperformance\\s(10\\ss) = ", 0) == 0) value = line.substr(line.find(" = ") + 3);
    Expect(!value.empty(), "(c) " + code + " has the row's text");
    if (code != "en") Expect(value != "Measure\\sperformance\\s(10\\ss)", "(c) " + code + " translates it");
  }
  Expect(files == 16, "(c) the 16 language files");
}

}  // namespace

int main(int argc, char** argv) {
  CheckGuide();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "(c) the language folder is the argument");
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-guide-measure: all checks passed\n");
  return 0;
}
