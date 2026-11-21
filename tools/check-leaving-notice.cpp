// SPDX-License-Identifier: MIT
// Fix 93b (tools/source-1.0.2/fixes/93b-leaving-notice.patch): a message while a game is left.
// Closing a game, exiting the emulator, or a title switch left the screen black or frozen for
// seconds (the Vulkan cache checkpoint, fix 07, up to 30 s; the network steps, fix 22) before the
// title restarted. Now "Returning to the menu..." (or "Restarting the game...", "Closing the
// emulator...") is drawn over everything with dots turning beside it, and the UI thread keeps
// drawing while those steps run on another thread.
//  (a) include/xbox360ps5/leaving_notice.hpp: nothing shown until a kind is set, Show/Current;
//  (b) DotAlpha: between 0.15 and 1, the brightest dot goes round once a second, the ones behind
//      it dimmer in order;
//  (c) WhilePainting: the task runs on another thread, the calling thread paints frame after
//      frame until it ends (about 30 frames a second or more), and returns only after it ended;
//  (d) the assembled tree: the overlay with its four texts, made with the game's guide, the notice
//      shown as the game loop ends (menu after the guide's Close, closing otherwise) and by the
//      title switch's restart hook before its checkpoint, the network wait, the sessions' removal
//      and every checkpoint of that way out drawn through;
//  (e) the four texts are in every language file of the argument's folder, translated.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include "xbox360ps5/leaving_notice.hpp"

#include <chrono>
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
  return text.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a);
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}

void CheckState() {
  Expect(notice::Current() == notice::Kind::none, "(a) nothing is shown at first");
  notice::Show(notice::Kind::menu);
  Expect(notice::Current() == notice::Kind::menu, "(a) Show sets what is shown");
  notice::Show(notice::Kind::restart_game);
  Expect(notice::Current() == notice::Kind::restart_game, "(a) a later Show replaces it");
  std::thread([] { notice::Show(notice::Kind::closing); }).join();
  Expect(notice::Current() == notice::Kind::closing, "(a) set from another thread");
  notice::Show(notice::Kind::none);
}

void CheckDots() {
  for (double t = 0.0; t < 3.0; t += 0.01)
    for (int dot = 0; dot < 8; ++dot) {
      const float a = notice::DotAlpha(dot, 8, t);
      Expect(a >= 0.149f && a <= 1.0001f, "(b) between 0.15 and 1");
    }
  Expect(notice::DotAlpha(0, 8, 0.0) > 0.999f, "(b) dot 0 is the brightest at the start of a second");
  Expect(notice::DotAlpha(4, 8, 0.5) > 0.999f && notice::DotAlpha(4, 8, 2.5) > 0.999f,
         "(b) half way round after half a second, once a second");
  for (int behind = 1; behind < 8; ++behind)
    Expect(notice::DotAlpha((4 - behind + 8) % 8, 8, 0.5) < notice::DotAlpha((4 - behind + 9) % 8, 8, 0.5),
           "(b) each dot behind the brightest is dimmer");
  Expect(notice::DotAlpha(5, 8, 0.5) < 0.3f, "(b) the dot just ahead is the dimmest (the tail's end)");
  Expect(notice::DotAlpha(0, 0, 1.0) == 0.0f, "(b) no dots");
}

void CheckPainting() {
  const auto caller = std::this_thread::get_id();
  std::thread::id ran_on;
  bool ended = false, painted_after_end = false;
  int frames = 0;
  const auto start = std::chrono::steady_clock::now();
  notice::WhilePainting(
      [&] {
        ran_on = std::this_thread::get_id();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ended = true;
      },
      [&] {
        Expect(std::this_thread::get_id() == caller, "(c) frames are painted on the calling thread");
        ++frames;
      });
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  painted_after_end = !ended;
  Expect(ran_on != caller && ran_on != std::thread::id(), "(c) the task runs on another thread");
  Expect(ended && !painted_after_end, "(c) WhilePainting returns after the task ended");
  Expect(seconds >= 0.29, "(c) and not before");
  Expect(frames >= 9, "(c) frames keep coming while the task runs: " + std::to_string(frames));
  Expect(frames <= 40, "(c) at most about 60 frames a second when painting returns at once: " + std::to_string(frames));
  // A paint that waits for the display (here 40 ms) is not slowed further.
  int slow_frames = 0;
  notice::WhilePainting([] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); },
                        [&] { ++slow_frames; std::this_thread::sleep_for(std::chrono::milliseconds(40)); });
  Expect(slow_frames >= 5 && slow_frames <= 9, "(c) a slow paint sets the pace: " + std::to_string(slow_frames));
  // A task that ends at once: at most a frame.
  int quick_frames = 0;
  notice::WhilePainting([] {}, [&] { ++quick_frames; });
  Expect(quick_frames <= 2, "(c) a task that ends at once costs a frame at most");
}

void CheckTree() {
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string overlay = Between(main, "class LeavingNoticeOverlay final", "class Guide final");
  for (const char* text : {"Tr(\"Returning to the menu...\")", "Tr(\"Restarting the game...\")",
                           "Tr(\"Closing the emulator...\")", "Tr(\"Please wait, this can take a few seconds.\")",
                           "notice::DotAlpha(dot, 8, seconds)", "if (kind == notice::Kind::none) return;"})
    Expect(Has(overlay, text), std::string("(d) the overlay has ") + text);
  const size_t guide = main.find("new Guide(&drawer, guide,");
  const size_t made = main.find("new LeavingNoticeOverlay(&drawer);  // fix 93b");
  Expect(guide != std::string::npos && made != std::string::npos && made > guide, "(d) the overlay is made after the guide (drawn over it)");
  const std::string way_out = Between(main, "hang_watch_thread.reset();  // fix 47", "if (!restart && emulator.is_title_open())");
  // Fix 93d: "Restarting the game..." when the guide restarts this game.
  Expect(Has(way_out, "leaving::Show(restart ? leaving::Kind::menu : leaving::Kind::closing);") ||
             Has(way_out, "leaving::Show(restart ? (restart_same_game ? leaving::Kind::restart_game : leaving::Kind::menu)"),
         "(d) the notice as the game loop ends");
  Expect(way_out.find("leaving::Show(restart") < way_out.find("controller_keyboard::Cancel()"),
         "(d) shown before the first wait");
  Expect(Has(way_out, "leaving::WhilePainting([] { xbox360ps5::guide_network::WaitIdle(std::chrono::seconds(25)); },"),
         "(d) the network change's wait is drawn through");
  Expect(Has(way_out, "std::chrono::steady_clock::now() < limit) paint_notice();") && !Has(way_out, "usleep(20000)"),
         "(d) the sessions' removal is drawn through");
  // Fix 93h puts the end's checkpoint in a block (its step said first) and the exit's in a lambda
  // with the flush: either form.
  Expect(Has(way_out, "leaving::Show(leaving::Kind::closing);") &&
             (Has(way_out, "if (!restart) leaving::WhilePainting([&exit_cache] { CheckpointExitCache(exit_cache); }, paint_notice);") ||
              Has(way_out, "leaving::WhilePainting([&exit_cache] { CheckpointExitCache(exit_cache); }, paint_notice);  // fix 93b")),
         "(d) the exit's and the end's checkpoints are drawn through");
  // Every checkpoint of the way out runs inside a WhilePainting a few lines before it.
  bool all_drawn = true;
  for (size_t at = way_out.find("CheckpointExitCache(exit_cache)"); at != std::string::npos;
       at = way_out.find("CheckpointExitCache(exit_cache)", at + 1)) {
    const size_t painting = way_out.rfind("WhilePainting(", at);
    all_drawn = all_drawn && painting != std::string::npos && at - painting < 160;
  }
  Expect(all_drawn, "(d) no checkpoint of the way out blocks the screen");
  Expect(Has(main, "if (restart && xbox360ps5::leaving_notice::Current() != xbox360ps5::leaving_notice::Kind::none) {"),
         "(d) the return's checkpoint is drawn through");
  const std::string hook = Between(main, "XamModule::restart_hook = [exit_cache, xam", "CheckpointExitCache(exit_cache);");
  Expect(Has(hook, "xbox360ps5::leaving_notice::Show(xam->loader_data().launch_path.empty()"),
         "(d) a title switch shows the notice before its checkpoint");
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
  const std::vector<std::string> keys = {"Returning to the menu...", "Restarting the game...", "Closing the emulator...",
                                         "Please wait, this can take a few seconds."};
  int languages = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++languages;
    const std::string code = entry.path().stem().string();
    const auto table = Language(entry.path());
    for (const auto& key : keys) {
      const auto found = table.find(key);
      Expect(found != table.end() && !found->second.empty(), "(e) " + code + " has: " + key);
      if (found != table.end() && code != "en") Expect(found->second != key, "(e) " + code + " translates: " + key);
    }
  }
  Expect(languages >= 16, "(e) every language file: " + std::to_string(languages));
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: check-leaving-notice <tools/source-1.0.2/english/lang>\n");
    return 2;
  }
  CheckState();
  CheckDots();
  CheckPainting();
  CheckTree();
  CheckLanguages(argv[1]);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-leaving-notice: all checks passed\n");
  return 0;
}
