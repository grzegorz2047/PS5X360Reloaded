// SPDX-License-Identifier: MIT
// Fix 132 (tools/source-1.0.2/fixes/132-console-game-language.patch): the game language
// "Automatic (console)" (xbox360ps5::ConsoleGameLanguage) knew six PS5 languages; any other gave
// English. On a Polish PS5 (c78) Dragon Age II (4541090B) opened locale\en-us with Automatic and
// locale\pl-pl with Polish chosen by hand.
//  (a) every PS5 system language (SCE_SYSTEM_PARAM_LANG_*, 0-29, as the SDK numbers them) gives
//      the Xbox 360 language the core numbers (xenia/xbox.h XLanguage) for it, Polish first; the
//      six from before are unchanged; languages the Xbox 360 list lacks, and unknown values, give
//      English;
//  (b) every language it gives is one the launcher's own Language list offers (kLanguages), with
//      the core's number for it (Polski 11, Русский 12, 한국어 7, 繁體中文 8, 简体中文 17, ...);
//  (c) the interface language stays separate: Settings::Load takes it from ConsoleUiLanguage.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "xbox360ps5/i18n.hpp"

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
  for (char c : s) if (c != '\r') out += c;
  return out;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// SCE_SYSTEM_PARAM_LANG_* (the SDK's sys_service.h) -> XLanguage (xenia/xbox.h).
enum Ps5 {
  kJapanese = 0, kEnglishUs = 1, kFrench = 2, kSpanish = 3, kGerman = 4, kItalian = 5, kDutch = 6,
  kPortuguesePt = 7, kRussian = 8, kKorean = 9, kChineseT = 10, kChineseS = 11, kFinnish = 12,
  kSwedish = 13, kDanish = 14, kNorwegian = 15, kPolish = 16, kPortugueseBr = 17, kEnglishGb = 18,
  kTurkish = 19, kSpanishLa = 20, kArabic = 21, kFrenchCa = 22, kCzech = 23, kHungarian = 24,
  kGreek = 25, kRomanian = 26, kThai = 27, kVietnamese = 28, kIndonesian = 29,
};
const std::map<int, int> kExpected = {
    {kPolish, 11},  // first: the console report
    {kRussian, 12},      {kKorean, 7},        {kChineseT, 8},      {kChineseS, 17},     {kDutch, 16},
    {kTurkish, 14},      {kEnglishGb, 1},     {kEnglishUs, 1},     {kJapanese, 2},      {kFrench, 4},
    {kFrenchCa, 4},      {kSpanish, 5},       {kSpanishLa, 5},     {kGerman, 3},        {kItalian, 6},
    {kPortuguesePt, 9},  {kPortugueseBr, 9},  {kFinnish, 1},       {kSwedish, 1},       {kDanish, 1},
    {kNorwegian, 1},     {kArabic, 1},        {kCzech, 1},         {kHungarian, 1},     {kGreek, 1},
    {kRomanian, 1},      {kThai, 1},          {kVietnamese, 1},    {kIndonesian, 1},
};

void CheckMapping() {
  using xbox360ps5::ConsoleGameLanguage;
  // (a) Polish first, as reported on the console.
  Expect(ConsoleGameLanguage(kPolish) == 11, "(a) a Polish PS5 gives Polish (11), not English");
  for (int ps5 = 0; ps5<= 29; ++ps5) {
    const auto expected = kExpected.find(ps5);
    Expect(expected != kExpected.end(), "(a) every SDK language has an expectation: " + std::to_string(ps5));
    if (expected == kExpected.end()) continue;
    Expect(ConsoleGameLanguage(ps5) == expected->second,
           "(a) PS5 language " + std::to_string(ps5) + " gives " + std::to_string(expected->second) + ", got " +
               std::to_string(ConsoleGameLanguage(ps5)));
  }
  for (const int unknown : {-1, 30, 99, 999})
    Expect(ConsoleGameLanguage(unknown) == 1, "(a) unknown value " + std::to_string(unknown) + " gives English");
}

void CheckTree() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::string xbox = Read(".deps/xenia-canary/src/xenia/xbox.h");
  // (b) the core's numbers.
  for (const auto& [name, id] : std::map<std::string, int>{{"kEnglish", 1}, {"kJapanese", 2}, {"kGerman", 3}, {"kFrench", 4},
                                                           {"kSpanish", 5}, {"kItalian", 6}, {"kKorean", 7}, {"kTChinese", 8},
                                                           {"kPortuguese", 9}, {"kPolish", 11}, {"kRussian", 12},
                                                           {"kTurkish", 14}, {"kDutch", 16}, {"kSChinese", 17}})
    Expect(Has(xbox, "  " + name + " = " + std::to_string(id) + ","), "(b) the core numbers " + name + " " + std::to_string(id));
  // (b) every language Automatic gives is in the launcher's list, with the same number.
  const std::map<int, std::string> names = {{1, "English"}, {2, "日本語"}, {3, "Deutsch"}, {4, "Français"},
                                            {5, "Español (España)"}, {6, "Italiano"}, {7, "한국어"}, {8, "繁體中文"},
                                            {9, "Português (Brasil)"}, {11, "Polski"}, {12, "Русский"}, {14, "Türkçe"},
                                            {16, "Nederlands"}, {17, "简体中文"}};
  for (int ps5 = -1; ps5 <= 29; ++ps5) {
    const int id = xbox360ps5::ConsoleGameLanguage(ps5);
    const auto name = names.find(id);
    Expect(name != names.end() && Has(launcher, "{" + std::to_string(id) + ", \"" + name->second + "\"}"),
           "(b) the launcher's Language list offers " + std::to_string(id) + " (from PS5 " + std::to_string(ps5) + ")");
  }
  // (c)
  Expect(Has(launcher, "console_language = ConsoleGameLanguage(ps5_language);") &&
             Has(launcher, "console_ui_language = ConsoleUiLanguage(ps5_language);"),
         "(c) the game and the interface languages are separate");
}

}  // namespace

int main() {
  CheckMapping();
  CheckTree();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-game-language: all checks passed\n");
  return 0;
}
