// SPDX-License-Identifier: MIT
// Fix 60 (tools/source-1.0.2/fixes/60-library-folders.patch, issue #12): games copied to the
// title's own assets/roms are found in an image install too, and the empty library names folders
// the library scans. With a .ffpkg/.ffpfsc install /app0 is the image; the empty-library screen said
// to copy games to the title's data folder's assets/roms, which no game folder covered.
//  (a) include/xbox360ps5/game_paths.hpp: the default game folders hold the title's own
//      assets/roms (XBOX360PS5_DATA_ROOT, PPSA83600) right after /app0/assets/roms, never the
//      official app's folder; AddTitleRoms gives a saved list that folder once, after
//      /app0/assets/roms (or first), keeping every entry and its order; DirectoryIdentity (stat) is
//      the same for one directory under two names, different for two directories, and none for a
//      file or a missing path;
//  (b) the assembled launcher: a list saved before gains the folder once (a marker file), both
//      root loops scan one directory once, and both empty-library screens add the line naming
//      /data/xbox360/ and the drives' xbox360 folders.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/game_paths.hpp"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
size_t Count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
  return n;
}
namespace x = xbox360ps5;

void CheckDefaults() {
  Expect(std::string(x::kTitleRoms) == "/data/homebrew/PPSA83600/assets/roms", "the title's own folder");
  const auto defaults = x::DefaultGamePaths();
  Expect(defaults.size() == 7 && defaults[0] == "/app0/assets/roms" && defaults[1] == x::kTitleRoms &&
             defaults[2] == "/data/xbox360",
         "the default folders: /app0's, the title's, /data/xbox360, the drives'");
  for (const auto& path : defaults) Expect(path.find("PPSA50011") == std::string::npos, "never the official app's: " + path);

  std::vector<std::string> saved = {"/mnt/usb0/Games", "/app0/assets/roms", "/data/xbox360"};
  Expect(x::AddTitleRoms(saved), "a saved list without it gains it");
  Expect((saved == std::vector<std::string>{"/mnt/usb0/Games", "/app0/assets/roms", x::kTitleRoms, "/data/xbox360"}),
         "right after /app0/assets/roms, every entry kept in its order");
  Expect(!x::AddTitleRoms(saved) && saved.size() == 4, "once");
  std::vector<std::string> trailing = {"/mnt/ext1/xbox360", std::string(x::kTitleRoms) + "/"};
  Expect(!x::AddTitleRoms(trailing), "already there (written with a slash)");
  std::vector<std::string> other = {"/mnt/ext1/xbox360"};
  Expect(x::AddTitleRoms(other) && other.front() == x::kTitleRoms && other.size() == 2, "first when /app0's is not listed");
}

void CheckIdentity() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("check-library-folders-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "roms" / "Game");
  fs::create_directories(root / "other");
  std::ofstream(root / "file.iso") << "x";
  const auto a = x::DirectoryIdentity(root / "roms");
  const auto b = x::DirectoryIdentity(root / "other" / ".." / "roms");
  const auto c = x::DirectoryIdentity(root / "other");
  Expect(a && b && *a == *b, "one directory under two names: the same identity");
  Expect(c && *a != *c, "two directories: different");
  Expect(!x::DirectoryIdentity(root / "file.iso") && !x::DirectoryIdentity(root / "missing"), "a file or nothing: none");
  fs::remove_all(root);
}

void CheckTree() {
  const std::string launcher = Read("src/launcher.cpp");
  const size_t load = launcher.find("void Settings::Load() {");
  // Fix 165: the user's data folder (UserData()); before 165 the internal storage (kStorage).
  const std::string folder = launcher.find("UserData() / \"game_paths.fix60\"") != std::string::npos ? "UserData()" : "kStorage";
  const size_t marker = launcher.find("const fs::path marker = " + folder + " / \"game_paths.fix60\";", load);
  const size_t add = launcher.find(
      "if (AddTitleRoms(game_paths)) WriteGamePaths(" + folder + " / \"game_paths.txt\", game_paths);", marker);
  Expect(load != std::string::npos && marker != std::string::npos && add != std::string::npos,
         "a saved list gains the folder once");
  Expect(Count(launcher, "directory && !seen_directories.insert(*directory).second") == 2,
         "both root loops scan one directory once");
  Expect(Count(launcher, "Tr(\"or in /data/xbox360/, or in an xbox360 folder at the top of a USB or M.2 drive.\")") == 2,
         "both empty-library screens name the other scanned folders");
  Expect(Count(launcher, "XBOX360PS5_DATA_ROOT \"/assets/roms/\"") == 2, "and the title's folder, now scanned");
}

}  // namespace

int main() {
  CheckDefaults();
  CheckIdentity();
  CheckTree();
  if (failures) {
    std::printf("check-library-folders: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-library-folders: ok\n");
  return 0;
}
