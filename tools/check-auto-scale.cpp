// SPDX-License-Identifier: MIT
// Fix 163 (tools/source-1.0.2/fixes/163-automatic-resolution-scale.patch): "Automatic" resolution
// scale, the default: 2x unless 2x did not suit the game (memory or under 30 frames), then 1x from
// its next start, remembered per game until its own settings are reset.
//  (a) include/xbox360ps5/auto_scale.hpp: Resolve (explicit 1x and 2x as they are; Automatic: the
//      launcher at 2x, a game at 2x, at 1x with a remembered verdict); the watch: nothing while
//      inactive, the memory trigger once, the speed trigger on the median of 6 windows of 30 s
//      after the first 60 s with the margin (29.5 a second, a game held at 30, is fine; 27.9 is
//      not), windows of a pause or a stall left out, a loading window outvoted by the median, a
//      short window left out; the verdict's text;
//  (b) the tree: launcher.hpp's default Automatic and the verdict functions; launcher.cpp's option
//      (Automatic the third choice and recommended, the help), the once-only migration of 1x to
//      Automatic (2x kept), ForGame resolving Automatic before fix 150g's cap, the verdict file
//      under game-settings, the game's reset clearing it; canary_game_main.cpp: the start line,
//      the watch begun only for Automatic at 2x with a title, fed every second and every 10 s,
//      fix 117d's notice not shown while it watches, the verdict saved, logged and told once;
//  (c) the three texts in all 16 language files (the argument's folder).
// Run from the assembled tree's root: check-auto-scale <english/lang directory>.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/auto_scale.hpp"

namespace {
namespace as = xbox360ps5::auto_scale;
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
// Fix 165 moves the user's files to UserData() (/data/homebrew/PPSA83600): a path may be either.
size_t FindStored(const std::string& squeezed_source, const std::string& text) {
  const size_t internal = squeezed_source.find(Squeezed(text));
  if (internal != std::string::npos) return internal;
  std::string moved = text;
  for (size_t at = moved.find("kStorage /"); at != std::string::npos; at = moved.find("kStorage /", at))
    moved.replace(at, 10, "UserData() /");
  return squeezed_source.find(Squeezed(moved));
}
const char* kChoice = "Automatic (2x while the game keeps 30 fps)";
const char* kNotice = "This game runs better at 1x: from its next start it runs at 1x.";
const char* kAbout =
    "Draws the game at twice its own resolution for a sharper picture. Automatic, the recommended choice, uses 2x "
    "and switches a game to 1x from its next start when 2x makes it run under 30 frames a second or runs out of "
    "memory; resetting the game's own settings tries 2x again. 2x uses much more video memory and graphics power; "
    "some effects (glow, shadows, pictures the game reads back) may look wrong.";

// Feeds `seconds` seconds of `swaps` a second from `from`; returns the trigger at the end.
as::Trigger Feed(as::Watch& w, int from, int seconds, double swaps) {
  as::Trigger t = as::Trigger::none;
  for (int s = from; s < from + seconds; ++s) t = w.Second(s, swaps);
  return t;
}

void Decisions() {
  Expect(as::Resolve(0, "4D5308C9", false).choice == 0 && as::Resolve(1, "4D5308C9", true).choice == 1,
         "(a) an explicit 1x or 2x is used as it is (a verdict does not touch it)");
  Expect(as::Resolve(2, "", false).choice == 1, "(a) Automatic in the launcher (no game): 2x");
  Expect(as::Resolve(2, "545407F2", false).choice == 1 && as::Resolve(2, "545407F2", true).choice == 0,
         "(a) Automatic for a game: 2x, or 1x with a remembered verdict");

  as::Watch idle;
  idle.Begin(false);
  Expect(Feed(idle, 0, 600, 5) == as::Trigger::none && idle.Memory(true, "x") == as::Trigger::none,
         "(a) not watching (explicit scale, 1x, no title): never decides");

  as::Watch memory;
  memory.Begin(true);
  Expect(memory.Memory(false, "no") == as::Trigger::none, "(a) memory: nothing while the floors hold");
  Expect(memory.Memory(true, "render target classes kept at 1x 3") == as::Trigger::memory &&
             memory.numbers() == "render target classes kept at 1x 3",
         "(a) memory: a floor hit decides 1x, with its numbers");
  Expect(memory.Memory(true, "again") == as::Trigger::none && memory.numbers() == "render target classes kept at 1x 3",
         "(a) memory: decided once");

  as::Watch slow;
  slow.Begin(true);
  Expect(Feed(slow, 0, 60, 5) == as::Trigger::none, "(a) speed: the first 60 s (loading, menus) do not count");
  Expect(Feed(slow, 60, 6 * 30, 20) == as::Trigger::none, "(a) speed: six windows only close when the next begins");
  Expect(Feed(slow, 240, 1, 20) == as::Trigger::speed && slow.windows().size() == as::kWindows,
         "(a) speed: 20 a second over six windows of 30 s: 1x");
  Expect(slow.numbers().find("median 20.0 game swaps/s over 6 windows of 30 s (20.0 20.0 20.0 20.0 20.0 20.0), under 28") == 0,
         "(a) speed: the numbers in the verdict: " + slow.numbers());

  as::Watch capped;
  capped.Begin(true);
  Feed(capped, 0, 60 + 10 * 30, 29.5);
  Expect(!capped.decided(), "(a) speed: 29.5 a second (a game held at 30) is fine");
  as::Watch under;
  under.Begin(true);
  Feed(under, 0, 60 + 7 * 30, 27.9);
  Expect(under.trigger() == as::Trigger::speed, "(a) speed: 27.9 a second is under the margin");

  as::Watch paused;
  paused.Begin(true);
  Feed(paused, 0, 60, 30);
  Feed(paused, 60, 3 * 30, 20);
  Feed(paused, 150, 4 * 30, 0.5);  // the guide, a pause, a stall
  Expect(!paused.decided() && paused.windows().size() == 3, "(a) speed: windows under 1 a second do not count");
  Feed(paused, 270, 4 * 30, 20);
  Expect(paused.trigger() == as::Trigger::speed, "(a) speed: decided once six windows of play are in");

  as::Watch loading;
  loading.Begin(true);
  Feed(loading, 0, 60, 60);
  Feed(loading, 60, 30, 8);  // a loading screen at 8 a second
  Feed(loading, 90, 6 * 30, 60);
  Expect(!loading.decided(), "(a) speed: one slow loading window among fast ones is outvoted by the median");

  Expect(as::Median({3, 1, 2}) == 2 && as::Median({4, 1, 2, 3}) == 2.5 && as::Median({}) == 0, "(a) the median");
  Expect(as::VerdictText(as::Trigger::speed, "median 20.0") == "1x speed: median 20.0" &&
             as::SaysOneX(as::VerdictText(as::Trigger::memory, "m")) && !as::SaysOneX("") && !as::SaysOneX("2x"),
         "(a) the verdict's text");
}

void Wiring() {
  const std::string header = Squeezed(Read("include/xbox360ps5/launcher.hpp"));
  Expect(header.find(Squeezed("int draw_scale = 2;  // fix 163")) != std::string::npos,
         "(b) the general setting's default is Automatic");
  Expect(header.find(Squeezed("std::string ScaleVerdict(const std::string& title_id);")) != std::string::npos &&
             header.find(Squeezed("void ClearScaleVerdict(const std::string& title_id);")) != std::string::npos,
         "(b) the verdict functions");
  const std::string launcher = Squeezed(Read("src/launcher.cpp"));
  Expect(launcher.find(Squeezed(std::string("{\"draw_scale\", &S::draw_scale, 0, W::start, 2, true, \"Resolution scale (test)\", \"") +
                                kAbout + "\", {\"1x (720p, native)\", \"2x (1440p, demanding)\", \"" + kChoice + "\"}},")) !=
             std::string::npos,
         "(b) the option: Automatic its third choice and recommended, the help says what it does");
  const size_t marker = FindStored(launcher, "const fs::path marker = kStorage / \"draw_scale.fix163\";");
  const size_t migrate = launcher.find(Squeezed("if (draw_scale == 0) draw_scale = xbox360ps5::auto_scale::kAutomatic;"), marker);
  Expect(marker != std::string::npos && migrate != std::string::npos &&
             launcher.find(Squeezed("Save();"), migrate) != std::string::npos,
         "(b) once (a marker file), 1x becomes Automatic in settings.txt; 2x stays");
  const size_t for_game = launcher.find(Squeezed("Settings ForGame(const Settings& general"));
  const size_t resolve = launcher.find(Squeezed("effective.draw_scale = xbox360ps5::auto_scale::Resolve("), for_game);
  const size_t verdict = launcher.find(Squeezed("xbox360ps5::auto_scale::SaysOneX(ScaleVerdict(title_id))).choice;"), resolve);
  const size_t cap = launcher.find(Squeezed("effective.draw_scale = xbox360ps5::resolution_scale::ChoiceFor("), for_game);
  Expect(for_game != std::string::npos && resolve != std::string::npos && verdict != std::string::npos &&
             (cap == std::string::npos || resolve < cap),
         "(b) ForGame resolves Automatic with the game's verdict (before fix 150g's cap)");
  Expect(FindStored(launcher, "std::ifstream input(kStorage / \"game-settings\" / (title_id + \".scale.txt\"));") !=
                 std::string::npos &&
             FindStored(launcher, "fs::remove(kStorage / \"game-settings\" / (title_id + \".scale.txt\"), error);") !=
                 std::string::npos,
         "(b) the verdict in game-settings/<TITLE ID>.scale.txt");
  Expect(launcher.find(Squeezed("overrides_.clear(); SaveGameOverrides(Selected()->title_id, overrides_); "
                                "ClearScaleVerdict(Selected()->title_id);")) != std::string::npos,
         "(b) resetting the game's own settings clears its verdict");

  const std::string main = Squeezed(Read("src/canary_game_main.cpp"));
  const size_t launch = main.find(Squeezed("XELOGI(\"GAME LAUNCH {:08X}\", status);"));
  const size_t line = main.find(Squeezed("\"Resolution scale (fix 163): {} runs at {}x: {} ({}){}{}\""), launch);
  const size_t begin = main.find(Squeezed("as::session().Begin(settings_title, chosen_scale == as::kAutomatic && running == 2 && "
                                          "!settings_title.empty());"),
                                 launch);
  Expect(launch != std::string::npos && line != std::string::npos && begin != std::string::npos && line < begin,
         "(b) the launch logs the scale and why, and watches Automatic at 2x with a title only");
  const size_t second = main.find(Squeezed("NoteAutoScaleVerdict(xbox360ps5::auto_scale::session().watch.Second("
                                           "std::chrono::duration<double>(at - watch_started).count(), swaps_this_second));"));
  const size_t memory = main.find(Squeezed("NoteAutoScaleVerdict(xbox360ps5::auto_scale::session().watch.Memory("));
  const size_t warn = main.find(Squeezed("if (!xbox360ps5::auto_scale::session().watch.active() && "
                                         "xbox360ps5::resolution_scale::ShouldWarn("));
  Expect(second != std::string::npos && memory != std::string::npos && warn != std::string::npos && memory < warn,
         "(b) fed every second (swaps) and every 10 s (the floors); fix 117d's notice not while it watches");
  const size_t note = main.find(Squeezed("void NoteAutoScaleVerdict(xbox360ps5::auto_scale::Trigger trigger) {"));
  Expect(note != std::string::npos &&
             main.find(Squeezed("if (trigger == as::Trigger::none || session.reported) return;"), note) != std::string::npos &&
             main.find(Squeezed("xbox360ps5::SaveScaleVerdict(session.title, text);"), note) != std::string::npos &&
             main.find(Squeezed(std::string("Tr(\"") + kNotice + "\")"), note) != std::string::npos,
         "(b) the verdict saved, logged and told once a session");
}

std::string Escaped(const std::string& text) {
  std::string out;
  for (char c : text) out += c == ' ' ? std::string("\\s") : std::string(1, c);
  return out;
}
void Languages(const std::filesystem::path& lang) {
  const std::vector<std::string> codes = {"en", "pt-BR", "pt", "es", "es-419", "de", "fr", "it",
                                          "ja", "ko", "nl", "pl", "ru", "tr", "zh-Hans", "zh-Hant"};
  for (const std::string& code : codes) {
    const std::string file = Read(lang / (code + ".txt"));
    bool all = true;
    for (const char* key : {kChoice, kNotice, kAbout}) {
      const std::string prefix = "\n" + Escaped(key) + " = ";
      const size_t at = file.find(prefix);
      const size_t end = at == std::string::npos ? at : file.find('\n', at + prefix.size());
      const std::string value = at == std::string::npos ? std::string() : file.substr(at + prefix.size(), end - at - prefix.size());
      // The choice names 2x, the help and the notice 1x.
      all = all && !value.empty() && value.find(key == kChoice ? "2x" : "1x") != std::string::npos;
    }
    Expect(all, "(c) " + code + ".txt translates the choice, the help and the notice");
  }
  const std::string pl = Read(lang / "pl.txt");
  Expect(pl.find(Escaped(kChoice) + " = Automatyczna") != std::string::npos &&
             pl.find(Escaped(kNotice) + " = Ta\\sgra\\sdziała\\slepiej\\sw\\s1x") != std::string::npos,
         "(c) Polish: Automatyczna, and the notice as the player asked");
}
}  // namespace

int main(int argc, char** argv) {
  Decisions();
  if (std::filesystem::exists("src/launcher.cpp")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root");
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
  std::printf("PASS: Automatic resolution scale: 2x unless memory or speed says 1x, remembered per game\n");
  return 0;
}
