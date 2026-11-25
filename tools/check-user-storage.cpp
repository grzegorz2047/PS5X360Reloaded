// SPDX-License-Identifier: MIT
// Fix 165 (tools/source-1.0.2/fixes/165-user-data-folder.patch): the user's files (options, each
// game's options, profile, recent list, game folders, patch choices, game configs, covers, avatars,
// the FSI and frame generation choices) are kept in the data folder /data/homebrew/PPSA83600, which
// a reinstall keeps and FTP reaches, instead of the internal storage /download0/xbox360ps5, which a
// ShadowMount reinstall makes anew. user_storage.hpp on temporary folders:
//  (a) the path: Root() is the data folder until Use(), then what Use() was given; the moved items'
//      table (unique names on both sides; the patch choices as patch-choices/, apart from
//      assets/patches; only framegen.conf replaces);
//  (b) the first start: each item in the internal storage and missing in the data folder is copied
//      (folders file by file), one line per item, the old copies left in place; a file the data
//      folder has is never replaced (framegen.conf: the internal storage's copy, the one in use,
//      replaces it and the old one is kept as framegen.conf.before-move); a leftover temporary
//      (.<name>.moving) of an interrupted start is not taken for the file; the marker is written;
//  (c) once: a second start copies nothing, also after the user deleted a moved file;
//  (d) a start that could not copy everything writes no marker; the next copies what is missing;
//  (e) the fallback: a data folder that cannot be made (a file in the way) or written (the probe
//      fails) -> the internal storage, with the reason, nothing written in the data folder;
//  (f) the assembled tree (when run from its root): no file of the moved items names the internal
//      storage any more; the /download0/xbox360ps5 literals left are the kept ones; main chooses the
//      folder before the options are read and logs a line per moved item.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh); it also runs on
// its own (then (f) says it has no tree).
#undef NDEBUG
#include "xbox360ps5/user_storage.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace us = xbox360ps5::user_storage;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

void Put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}
std::string Get(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
size_t Count(const std::vector<std::string>& lines, const std::string& part) {
  size_t n = 0;
  for (const auto& line : lines) n += Has(line, part);
  return n;
}

// Writes as WriteFile does, but refuses every path that contains `refused`.
std::string refused;
bool RefusingWriter(const fs::path& path, const std::string& bytes, std::string& why) {
  if (!refused.empty() && Has(path.generic_string(), refused)) {
    why = "refused by the check: " + path.string();
    return false;
  }
  return us::WriteFile(path, bytes, why);
}

void PathChecks() {
  Expect(us::Root() == fs::path(XBOX360PS5_DATA_ROOT), "(a) Root() is the data folder until Use()");
  Expect(std::string(us::kDataRoot) == "/data/homebrew/PPSA83600", "(a) the data folder is /data/homebrew/PPSA83600");
  Expect(std::string(us::kInternalRoot) == "/download0/xbox360ps5", "(a) the internal storage is /download0/xbox360ps5");
  us::Use("/tmp/elsewhere");
  Expect(us::Root() == fs::path("/tmp/elsewhere") && us::Root() / "settings.txt" == fs::path("/tmp/elsewhere/settings.txt"),
         "(a) Use() sets Root()");
  us::Use(us::kDataRoot);
  std::set<std::string> from, to;
  int replacing = 0;
  for (const auto& item : us::kMoved) {
    Expect(from.insert(item.from).second, std::string("(a) one entry for ") + item.from);
    Expect(to.insert(item.to).second, std::string("(a) one target for ") + item.to);
    replacing += item.replace;
    if (item.replace) Expect(std::string(item.from) == "framegen.conf", "(a) only framegen.conf replaces");
  }
  Expect(replacing == 1, "(a) one replacing item");
  for (const char* name : {"settings.txt", "game-settings", "profile.txt", "recent.txt", "game_paths.txt",
                           "game_paths.fix60", "resolution-scale", "fsi.c", "framegen.conf", "patches",
                           "game-configs", "avatars", "covers"})
    Expect(from.count(name) == 1, std::string("(a) moved: ") + name);
  for (const char* name : {"library", "disc", "content", "boot.log", "launch-once.txt", "next-title.txt",
                           "autotest-state.txt", "fsi.1", "dash-skin", "LOGS", "bug-report.txt"})
    Expect(from.count(name) == 0, std::string("(a) kept in the internal storage: ") + name);
  Expect(to.count("patch-choices") == 1 && to.count("patches") == 0, "(a) the patch choices go to patch-choices/");
}

void MigrationChecks(const fs::path& base) {
  const fs::path internal = base / "download0" / "xbox360ps5", data = base / "data" / "PPSA83600";
  fs::remove_all(base);
  Put(internal / "settings.txt", "language=pl\nvsync=1\n");
  Put(internal / "game-settings" / "4D5307E6.txt", "draw_scale=1\n");
  Put(internal / "game-settings" / "545407F2.txt", "fast_locks=1\n");
  Put(internal / "profile.txt", "E000012345678901\n");
  Put(internal / "recent.txt", "/mnt/ext1/xbox360/Halo 3.iso\n");
  Put(internal / "game_paths.txt", "/mnt/ext1/xbox360\n");
  Put(internal / "patches" / "4D5307E6.txt", "1|60 FPS\n");
  Put(internal / "covers" / "4D5307E6.jpg", std::string("\xFF\xD8\xFF\xE0jpeg", 8));
  Put(internal / "avatars" / "E000012345678901.txt", "body=2\n");
  Put(internal / "game-configs" / "4D5307E6.config.toml", "[GPU]\n");
  Put(internal / "framegen.conf", "1\n2\n");
  Put(internal / "fsi.c", "render_target_path_vulkan=fsi\n");
  // Kept in the internal storage: not copied.
  Put(internal / "library" / "abc.txt", "cache\n");
  Put(internal / "launch-once.txt", "/mnt/ext1/game.iso\n");
  Put(internal / "content" / "E000012345678901" / "save.bin", "save");
  // The data folder already has some: its own are kept.
  Put(data / "game-settings" / "4D5307E6.txt", "draw_scale=0\n");
  Put(data / "framegen.conf", "0\n2\n");  // fix 46's import from the official PS5X360
  Put(data / "network.conf", "mode=online\n");
  // An interrupted start's temporary.
  Put(data / ".settings.txt.moving", "lang");

  const us::Outcome first = us::Choose(data, internal);
  Expect(!first.fallback && first.root == data, "(b) the data folder is used");
  Expect(first.marked && fs::is_regular_file(data / us::kMarker), "(b) the marker is written");
  Expect(!fs::exists(data / us::kProbe), "(b) the probe file is removed again");
  Expect(Get(data / "settings.txt") == "language=pl\nvsync=1\n", "(b) settings.txt copied (not the leftover temporary)");
  Expect(!fs::exists(data / ".settings.txt.moving"), "(b) the leftover temporary is gone (written again and renamed)");
  Expect(Get(data / "game-settings" / "4D5307E6.txt") == "draw_scale=0\n", "(b) the data folder's game settings are not replaced");
  Expect(Get(data / "game-settings" / "545407F2.txt") == "fast_locks=1\n", "(b) a missing game's settings are copied");
  Expect(Get(data / "profile.txt") == "E000012345678901\n", "(b) profile.txt copied");
  Expect(Get(data / "recent.txt") == "/mnt/ext1/xbox360/Halo 3.iso\n", "(b) recent.txt copied");
  Expect(Get(data / "game_paths.txt") == "/mnt/ext1/xbox360\n", "(b) game_paths.txt copied");
  Expect(Get(data / "patch-choices" / "4D5307E6.txt") == "1|60 FPS\n" && !fs::exists(data / "patches"),
         "(b) the patch choices copied to patch-choices/");
  Expect(Get(data / "covers" / "4D5307E6.jpg") == std::string("\xFF\xD8\xFF\xE0jpeg", 8), "(b) covers copied byte for byte");
  Expect(Get(data / "avatars" / "E000012345678901.txt") == "body=2\n", "(b) avatars copied");
  Expect(Get(data / "game-configs" / "4D5307E6.config.toml") == "[GPU]\n", "(b) game configs copied");
  Expect(Get(data / "fsi.c") == "render_target_path_vulkan=fsi\n", "(b) fsi.c copied");
  Expect(Get(data / "framegen.conf") == "1\n2\n" && Get(data / "framegen.conf.before-move") == "0\n2\n",
         "(b) framegen.conf: the one in use replaces the data folder's, which is kept aside");
  Expect(Get(data / "network.conf") == "mode=online\n", "(b) the data folder's own files untouched");
  Expect(!fs::exists(data / "library") && !fs::exists(data / "launch-once.txt") && !fs::exists(data / "content"),
         "(b) the kept items are not copied");
  Expect(fs::exists(internal / "settings.txt") && fs::exists(internal / "game-settings" / "545407F2.txt") &&
             fs::exists(internal / "covers" / "4D5307E6.jpg"),
         "(b) the old copies stay in the internal storage");
  Expect(first.moved == 11, "(b) 11 items moved (game-settings in part), got " + std::to_string(first.moved));
  Expect(first.lines.size() == 11, "(b) one line per moved item, got " + std::to_string(first.lines.size()));
  Expect(Count(first.lines, "moved settings.txt: copied to ") == 1, "(b) the settings.txt line");
  Expect(Count(first.lines, "moved game-settings/: 1 file copied to ") == 1 &&
             Count(first.lines, "1 already there (kept)") == 1,
         "(b) the game-settings line counts what was kept");
  Expect(Count(first.lines, "kept as framegen.conf.before-move") == 1, "(b) the framegen.conf line says what was kept");
  Expect(Count(first.lines, "the old cop") == 11, "(b) each line says the old copy stays");
  Expect(Has(us::Summary(first), "11 items moved from ") && !Has(us::Summary(first), "not finished"),
         "(b) the summary: " + us::Summary(first));
  Expect(Has(Get(data / us::kMarker), "moved settings.txt"), "(b) the marker lists what was moved");

  // (c) Once: the user deletes a moved file and changes one in the internal storage.
  fs::remove(data / "profile.txt");
  Put(internal / "recent.txt", "changed\n");
  const us::Outcome second = us::Choose(data, internal);
  Expect(!second.fallback && second.already && second.lines.empty() && second.moved == 0, "(c) nothing done again");
  Expect(!fs::exists(data / "profile.txt"), "(c) a file the user deleted does not come back");
  Expect(Get(data / "recent.txt") == "/mnt/ext1/xbox360/Halo 3.iso\n", "(c) a moved file is not copied again");
  Expect(us::Summary(second) == data.string(), "(c) the summary names the folder only");
}

void PartialChecks(const fs::path& base) {
  const fs::path internal = base / "internal", data = base / "data";
  fs::remove_all(base);
  Put(internal / "settings.txt", "a=1\n");
  Put(internal / "covers" / "4D5307E6.jpg", "jpg1");
  Put(internal / "covers" / "545407F2.jpg", "jpg2");
  refused = "545407F2";
  const us::Outcome partial = us::Choose(data, internal, RefusingWriter);
  Expect(!partial.fallback && partial.root == data, "(d) the data folder is still used");
  Expect(!partial.marked && !fs::exists(data / us::kMarker), "(d) no marker when a copy failed");
  Expect(Get(data / "settings.txt") == "a=1\n" && Get(data / "covers" / "4D5307E6.jpg") == "jpg1",
         "(d) the rest is copied");
  Expect(!fs::exists(data / "covers" / "545407F2.jpg") && !fs::exists(data / "covers" / ".545407F2.jpg.moving"),
         "(d) no file and no temporary for the failed copy");
  Expect(Count(partial.lines, "could not move covers/") == 1, "(d) the failure is logged");
  Expect(Has(us::Summary(partial), "not finished"), "(d) the summary says it is not finished");
  refused.clear();
  Put(data / "settings.txt", "a=2\n");  // changed by the user meanwhile
  const us::Outcome retry = us::Choose(data, internal);
  Expect(retry.marked && Get(data / "covers" / "545407F2.jpg") == "jpg2", "(d) the next start copies what is missing");
  Expect(Get(data / "settings.txt") == "a=2\n", "(d) and replaces nothing");
  Expect(retry.moved == 1 && Count(retry.lines, "moved covers/: 1 file copied") == 1, "(d) one line for the retry");
}

void FallbackChecks(const fs::path& base) {
  fs::remove_all(base);
  const fs::path internal = base / "internal";
  Put(internal / "settings.txt", "a=1\n");
  // A file where the data folder should be made.
  Put(base / "blocked", "file");
  const fs::path blocked = base / "blocked" / "PPSA83600";
  const us::Outcome cannot_make = us::Choose(blocked, internal);
  Expect(cannot_make.fallback && cannot_make.root == internal, "(e) a data folder that cannot be made: internal storage");
  Expect(Has(cannot_make.why, blocked.string()), "(e) the reason names the folder: " + cannot_make.why);
  Expect(cannot_make.lines.empty() && cannot_make.moved == 0 && !cannot_make.marked, "(e) nothing moved");
  Expect(Has(us::Summary(cannot_make), "(the internal storage: "), "(e) the summary: " + us::Summary(cannot_make));
  // A data folder that takes no file.
  const fs::path data = base / "refused-data";
  refused = "refused-data";
  const us::Outcome cannot_write = us::Choose(data, internal, RefusingWriter);
  refused.clear();
  Expect(cannot_write.fallback && cannot_write.root == internal, "(e) a data folder that takes no file: internal storage");
  Expect(Has(cannot_write.why, "refused by the check"), "(e) the reason is the write's: " + cannot_write.why);
  Expect(!fs::exists(data / "settings.txt") && !fs::exists(data / us::kMarker) && !fs::exists(data / us::kProbe),
         "(e) nothing written in the data folder");
  Expect(fs::exists(internal / "settings.txt"), "(e) the internal storage's files untouched");
  // The internal storage missing (a fresh install): nothing to copy, the marker written.
  fs::remove_all(base);
  const us::Outcome fresh = us::Choose(base / "data2", base / "none");
  Expect(!fresh.fallback && fresh.marked && fresh.moved == 0 && fresh.lines.empty(), "(e) a fresh install: marker only");
}

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  std::string out = text.str(), plain;
  for (char c : out) if (c != '\r') plain += c;
  return plain;
}

void TreeChecks() {
  const std::string launcher = ReadText("src/launcher.cpp"), game_main = ReadText("src/canary_game_main.cpp");
  if (launcher.empty() || game_main.empty()) {
    std::printf("no assembled tree here; the sources not checked\n");
    return;
  }
  // The /download0/xbox360ps5 literals left in the title, and why each stays.
  struct Kept { const char* file; const char* part; };
  const Kept kept[] = {
      {"include/xbox360ps5/user_storage.hpp", "kInternalRoot[] = \"/download0/xbox360ps5\""},
      {"src/canary_game_main.cpp", "\"/download0/xbox360ps5/hang-watch-sync.txt\""},  // fix 153: read after the data folder's
      {"src/canary_game_main.cpp", "\"/download0/xbox360ps5/boot.log\""},              // crash report fallback
      {"src/canary_game_main.cpp", "chdir(\"/download0/xbox360ps5\")"},               // the working folder
      {"src/launcher.cpp", "kFsiOnceFile = \"/download0/xbox360ps5/fsi.1\""},           // one restart
      {"src/launcher.cpp", "Tr(\"Removing a location does not delete games. Logs: /download0/xbox360ps5/LOGS\")"},
      {"src/title_switch.cpp", "\"/download0/xbox360ps5/next-title.txt\""},            // one restart
      {"src/autotest.cpp", "\"/download0/xbox360ps5/autotest-state.txt\""},           // a test's state
      {"src/native_game_main.cpp", "\"/download0/xbox360ps5"},                        // not built
      {"port/kinect_state.inc", "\"/download0/xbox360ps5/dash-skin\""},               // a cache
      {"include/xbox360ps5/launcher.hpp", "save_root_ = \"/download0/xbox360ps5/content\""},  // saves stay
      {"platform/ps5/crash_report.cpp", "\"/download0/xbox360ps5/boot.log\""},       // crash report fallback
  };
  const char* files[] = {"src/launcher.cpp", "src/canary_game_main.cpp", "src/covers.cpp", "src/game_patches.cpp",
                         "src/title_switch.cpp", "src/autotest.cpp", "src/native_game_main.cpp",
                         "port/presenter_sidecar.hpp", "port/kinect_state.inc", "include/xbox360ps5/avatar_skeleton.hpp",
                         "include/xbox360ps5/launcher.hpp", "include/xbox360ps5/user_storage.hpp",
                         "platform/ps5/crash_report.cpp"};
  for (const char* file : files) {
    std::istringstream lines(ReadText(file));
    int number = 0;
    for (std::string line; std::getline(lines, line);) {
      ++number;
      if (!Has(line, "/download0/xbox360ps5")) continue;
      const size_t code = line.find_first_not_of(" \t");
      if (code != std::string::npos && line.compare(code, 2, "//") == 0) continue;  // a comment
      bool known = false;
      for (const auto& k : kept) known |= std::string(k.file) == file && Has(line, k.part);
      Expect(known, std::string("(f) ") + file + ":" + std::to_string(number) + " names the internal storage: " + line);
      for (const auto& item : us::kMoved) {
        const std::string named = std::string("/download0/xbox360ps5/") + item.from;
        Expect(!Has(line, named + "\"") && !Has(line, named + "/"),
               std::string("(f) ") + file + ":" + std::to_string(number) + " hardcodes a moved item: " + line);
      }
    }
  }
  // The launcher keeps only the library cache and the disc folder in the internal storage.
  Expect(Has(launcher, "const fs::path kStorage = xbox360ps5::user_storage::kInternalRoot;") &&
             Has(launcher, "const fs::path& UserData() { return xbox360ps5::user_storage::Root(); }"),
         "(f) the launcher's two folders");
  for (const auto& item : us::kMoved)
    Expect(!Has(launcher, std::string("kStorage / \"") + item.from + "\""),
           std::string("(f) the launcher keeps ") + item.from + " in the internal storage");
  for (const char* name : {"settings.txt", "game-settings", "game_paths.txt", "game_paths.fix60", "recent.txt",
                           "resolution-scale"})
    Expect(Has(launcher, std::string("UserData() / \"") + name + "\""), std::string("(f) the launcher's ") + name);
  Expect(Has(launcher, "std::string FsiFile() { return (UserData() / \"fsi.c\").string(); }") &&
             Has(launcher, "std::string FramegenFile() { return (UserData() / \"framegen.conf\").string(); }"),
         "(f) the FSI and frame generation choices");
  Expect(Has(ReadText("src/covers.cpp"), "xbox360ps5::user_storage::Root() / \"covers\""), "(f) the covers");
  Expect(Has(ReadText("src/game_patches.cpp"), "xbox360ps5::user_storage::Root() / \"patch-choices\" / name"),
         "(f) the patch choices");
  Expect(Has(ReadText("src/title_switch.cpp"), "xbox360ps5::user_storage::Root() / \"game-configs\""), "(f) the game configs");
  Expect(Has(ReadText("include/xbox360ps5/avatar_skeleton.hpp"), "inline constexpr UserFolder kFolder{\"avatars\"};"),
         "(f) the avatars");
  Expect(Has(ReadText("port/presenter_sidecar.hpp"),
             "for (const char* candidate : {user_file.c_str(), \"/app0/framegen.conf\","),
         "(f) the presenter reads the user's framegen.conf first");
  Expect(Has(game_main, "return (xbox360ps5::user_storage::Root() / \"profile.txt\").string();"), "(f) the profile");
  // main: chosen after the import from PS5X360 and before the options are read; logged.
  const size_t import = game_main.find("xbox360ps5::official_import::Run(");
  const size_t choose = game_main.find(
      "const auto user_data = xbox360ps5::user_storage::Choose(xbox360ps5::user_storage::kDataRoot, storage);");
  const size_t use = game_main.find("xbox360ps5::user_storage::Use(user_data.root);");
  const size_t load = game_main.find("  global.Load();");
  const size_t once = game_main.find("std::ifstream once(storage / \"launch-once.txt\");");
  Expect(import != std::string::npos && choose != std::string::npos && use != std::string::npos &&
             load != std::string::npos && once != std::string::npos && import < choose && choose < use &&
             use < load && load < once,
         "(f) main chooses the folder after the import and before the options");
  Expect(Has(game_main, "XELOGW(\"User data: {}\", xbox360ps5::user_storage::Summary(user_data));") &&
             Has(game_main, "for (const auto& line : user_data.lines) XELOGW(\"User data: {}\", line);"),
         "(f) main logs the folder and one line per moved item");
  Expect(Has(game_main, "const std::filesystem::path storage = xbox360ps5::user_storage::kInternalRoot;"),
         "(f) main's internal storage is the header's");
}
}  // namespace

int main() {
  PathChecks();
  const fs::path base = fs::temp_directory_path() / "check-user-storage";
  MigrationChecks(base / "migration");
  PartialChecks(base / "partial");
  FallbackChecks(base / "fallback");
  fs::remove_all(base);
  us::Use(us::kDataRoot);
  TreeChecks();
  if (failures) {
    std::printf("check-user-storage: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-user-storage: all passed\n");
  return 0;
}
