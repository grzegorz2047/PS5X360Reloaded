// SPDX-License-Identifier: MIT
// Fix 93g (tools/source-1.0.2/fixes/93g-flush-before-restart.patch): the black screen after
// "Returning to the menu..." on 1.0.5-work-fix93f (research/fix93f/boot.log): the restarted title
// spent 2.9 s checking the saves folder (directory and file probes) and 1.9 s in the emulator's
// setup (0.27 s on a cold start), both slowed by the writes the system still held from the game
// just closed (its caches, about 30 MB). The way out now flushes them (sync) on its worker thread
// after the cache checkpoint, while the notice is drawn, and a start right after a game takes the
// saves folder without the probes.
//  (a) include/xbox360ps5/save_storage.hpp: PrepareSaveStorage with checked_moments_ago takes an
//      existing folder as it is (no staging directory made); a missing one goes the usual way; the
//      usual call is unchanged;
//  (b) the assembled tree: FlushWritesBeforeRestart syncs and logs how long it took; the return to
//      the menu runs it after the checkpoint on the worker the notice is drawn beside; the title
//      switch's hook runs it after its checkpoint, before LoadExec; the start passes whether it
//      follows a game (its remembered notice) to PrepareSaveStorage.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/save_storage.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
int Entries(const fs::path& folder) {
  int count = 0;
  for (const auto& entry : fs::directory_iterator(folder)) { (void)entry; ++count; }
  return count;
}

void CheckSaves(const fs::path& base) {
  const fs::path legacy = base / "content", saves = base / "data/saves";
  fs::create_directories(saves);
  { std::ofstream(saves / "a save") << "save"; }
  const int before = Entries(base / "data");
  const auto quick = xbox360ps5::PrepareSaveStorage(legacy, saves, true);
  Expect(quick.root == saves && Has(quick.notice, "checked by the start before this one"),
         "(a) a folder used moments ago is taken as it is: " + quick.notice);
  Expect(Entries(base / "data") == before && Entries(saves) == 1, "(a) without probes (no staging directory left or made)");
  const auto usual = xbox360ps5::PrepareSaveStorage(legacy, saves);
  Expect(usual.root == saves && usual.notice == "Homebrew save storage ready", "(a) the usual call checks as before: " + usual.notice);
  const fs::path fresh = base / "other/saves";
  const auto made = xbox360ps5::PrepareSaveStorage(legacy, fresh, true);
  Expect(made.root == fresh && fs::is_directory(fresh), "(a) a missing folder is made the usual way");
}

void CheckTree() {
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string flush = Between(main, "void FlushWritesBeforeRestart(const char* why) {", "\n}");
  Expect(Has(flush, "::sync();") && Has(flush, "XELOGW(\"Leaving: pending disk writes flushed in {} ms ({})\""),
         "(b) FlushWritesBeforeRestart syncs and logs how long");
  const std::string back = Between(main, "if (restart && xbox360ps5::leaving_notice::Current() != xbox360ps5::leaving_notice::Kind::none) {",
                                   "} else if (restart) {");
  const size_t checkpoint = back.find("CheckpointExitCache(exit_cache);");
  const size_t flushed = back.find("FlushWritesBeforeRestart(\"back to the menu\");");
  Expect(Has(back, "xbox360ps5::leaving_notice::WhilePainting(") && checkpoint != std::string::npos &&
             flushed != std::string::npos && checkpoint < flushed,
         "(b) the return to the menu flushes after its checkpoint, on the worker the notice is drawn beside");
  const std::string hook = Between(main, "XamModule::restart_hook = [exit_cache", "sceSystemServiceLoadExec(");
  const size_t hook_checkpoint = hook.find("CheckpointExitCache(exit_cache);");
  const size_t hook_flush = hook.find("FlushWritesBeforeRestart(\"title switch\");");
  Expect(hook_checkpoint != std::string::npos && hook_flush != std::string::npos && hook_checkpoint < hook_flush,
         "(b) the title switch flushes after its checkpoint, before LoadExec");
  Expect(Has(main, "xbox360ps5::PrepareSaveStorage(storage / \"content\", XBOX360PS5_DATA_ROOT \"/saves\",\n"
                   "                                                boot_notice.kind != xbox360ps5::leaving_notice::Kind::none);"),
         "(b) a start after a game takes the saves folder without probes");
}
}  // namespace

int main() {
  const fs::path base = fs::temp_directory_path() /
      ("check-flush-before-restart-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(base);
  CheckSaves(base);
  CheckTree();
  std::error_code error;
  fs::remove_all(base, error);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-flush-before-restart: all checks passed\n");
  return 0;
}
