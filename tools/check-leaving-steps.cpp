// SPDX-License-Identifier: MIT
// Fix 93h (tools/source-1.0.2/fixes/93h-leaving-steps.patch): the leaving notice says what is being
// done, in plain words, as a second line that changes with each step: saving the shaders (the
// cache checkpoint), writing to the drive (fix 93g's flush), signing out of online play, saving
// the bug report's logs (fix 93e), then, in the next process, starting the menu or the game. "Exit
// emulator" saves the shaders and flushes as the return to the menu does (93g flushed only before
// a restart), and its notice says the PS button would not keep the shaders.
//  (a) include/xbox360ps5/leaving_notice.hpp: ShowStep/CurrentStep, none at first, from any thread;
//  (b) the assembled tree: the overlay's text for each step and the closing tip; each step said
//      where it begins (the bug report's file, online play, each checkpoint and flush: the end, the
//      exit, the return to the menu, the title switch's hook; the next process's start) and
//      cleared when the next process's notice ends; the exit flushes after its checkpoint;
//  (c) the seven texts are in every language file of the argument's folder, translated.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include "xbox360ps5/leaving_notice.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace notice = xbox360ps5::leaving_notice;

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
bool InOrder(const std::string& text, const std::vector<std::string>& parts) {
  size_t at = 0;
  for (const auto& part : parts) {
    at = text.find(part, at);
    if (at == std::string::npos) return false;
    at += part.size();
  }
  return true;
}

void CheckSteps() {
  Expect(notice::CurrentStep() == notice::Step::none, "(a) no step at first");
  notice::ShowStep(notice::Step::shaders);
  Expect(notice::CurrentStep() == notice::Step::shaders, "(a) ShowStep sets the step");
  std::thread([] { notice::ShowStep(notice::Step::writing); }).join();
  Expect(notice::CurrentStep() == notice::Step::writing, "(a) from another thread");
  notice::ShowStep(notice::Step::none);
}

void CheckTree() {
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string overlay = Between(main, "class LeavingNoticeOverlay final", "\n};");
  for (const char* text : {"Tr(\"Saving shaders for faster loading next time...\")", "Tr(\"Writing data to the drive...\")",
                           "Tr(\"Signing out of online play...\")", "Tr(\"Saving the logs for the bug report...\")",
                           "Tr(\"Starting the game...\")", "Tr(\"Starting the menu...\")",
                           "Tr(\"Please wait, this can take a few seconds.\")"})
    Expect(Has(overlay, text), std::string("(b) the overlay says ") + text);
  Expect(Has(overlay, "if (kind == notice::Kind::closing) {") &&
             Has(overlay, "Tr(\"Tip: leave with Exit emulator rather than the PS button, so the shaders are kept for next time.\")"),
         "(b) closing: the tip about the PS button");
  const std::string way_out = Between(main, "hang_watch_thread.reset();  // fix 47", "if (!restart && emulator.is_title_open())");
  Expect(InOrder(way_out, {"leaving::ShowStep(leaving::Step::bug_logs);", "leaving::PaintUntil(recent_calls->done"}),
         "(b) the bug report's logs");
  Expect(InOrder(way_out, {"leaving::ShowStep(leaving::Step::online);", "DeleteAllSessionsByMac()"}), "(b) online play");
  Expect(InOrder(way_out, {"leaving::Show(leaving::Kind::closing);", "leaving::ShowStep(leaving::Step::shaders);",
                           "CheckpointExitCache(exit_cache);", "leaving::ShowStep(leaving::Step::writing);",
                           "FlushWritesBeforeRestart(\"exit emulator\");", "sceSystemServiceLoadExec(\"exit\", nullptr);"}),
         "(b) Exit emulator: the shaders saved, then the drive written to, then the title ends");
  Expect(InOrder(way_out, {"if (!restart) {", "leaving::ShowStep(leaving::Step::shaders);", "CheckpointExitCache(exit_cache)"}),
         "(b) the end's checkpoint says so");
  const std::string back = Between(main, "if (restart && xbox360ps5::leaving_notice::Current() != xbox360ps5::leaving_notice::Kind::none) {",
                                   "} else if (restart) {");
  Expect(InOrder(back, {"ShowStep(xbox360ps5::leaving_notice::Step::shaders);", "CheckpointExitCache(exit_cache);",
                        "ShowStep(xbox360ps5::leaving_notice::Step::writing);", "FlushWritesBeforeRestart(\"back to the menu\");"}),
         "(b) the return to the menu: shaders, then the drive");
  const std::string hook = Between(main, "XamModule::restart_hook = [exit_cache", "sceSystemServiceLoadExec(");
  Expect(InOrder(hook, {"ShowStep(xbox360ps5::leaving_notice::Step::shaders);", "CheckpointExitCache(exit_cache);",
                        "ShowStep(xbox360ps5::leaving_notice::Step::writing);", "FlushWritesBeforeRestart(\"title switch\");"}),
         "(b) the title switch: shaders, then the drive");
  Expect(InOrder(main, {"xbox360ps5::leaving_notice::Show(boot_notice.kind);",
                        "xbox360ps5::leaving_notice::ShowStep(xbox360ps5::leaving_notice::Step::starting);"}),
         "(b) the next process: starting the menu or the game");
  const std::string end = Between(main, "const auto end_boot_notice = ", "};");
  Expect(Has(end, "ShowStep(xbox360ps5::leaving_notice::Step::none);"), "(b) cleared when the next process's notice ends");
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
      "Saving shaders for faster loading next time...", "Writing data to the drive...", "Signing out of online play...",
      "Saving the logs for the bug report...", "Starting the menu...", "Starting the game...",
      "Tip: leave with Exit emulator rather than the PS button, so the shaders are kept for next time."};
  int languages = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++languages;
    const std::string code = entry.path().stem().string();
    const auto table = Language(entry.path());
    for (const auto& key : keys) {
      const auto found = table.find(key);
      Expect(found != table.end() && !found->second.empty(), "(c) " + code + " has: " + key);
      if (found != table.end() && code != "en") Expect(found->second != key, "(c) " + code + " translates: " + key);
    }
  }
  Expect(languages >= 16, "(c) every language file: " + std::to_string(languages));
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: check-leaving-steps <tools/source-1.0.2/english/lang>\n");
    return 2;
  }
  CheckSteps();
  CheckTree();
  CheckLanguages(argv[1]);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-leaving-steps: all checks passed\n");
  return 0;
}
