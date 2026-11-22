// SPDX-License-Identifier: MIT
// Fix 52 (tools/source-1.0.2/fixes/52-library-disc-number.patch): the library keeps the number of
// one disc of a set in its name. Battlefield 3's two .iso files were two tiles with the same
// cover and size; their long names were cut before "(Disc N)", and after the first launch the
// cached title ("Battlefield 3") named both, so Disc 1 (multiplayer) was started for Disc 2
// (the campaign).
//  (a) port/disc_catalog.cpp: DiscOfImage reads the disc number in an image's or an extracted
//      disc folder's name (Disc / Disk / Disco N), 0 for none; LibraryName appends " (Disc N)"
//      unless the name already names that disc, and leaves a single-disc game's name alone;
//  (b) the assembled tree: GameEntry has the disc, the library's scan sets it and the name for
//      every entry that is not an extracted group (after the cached title, so a cached
//      "Battlefield 3" becomes "Battlefield 3 (Disc 1)"), and the details line under the name,
//      which is never cut, says Tr("Disc %u") before the size;
//  (c) every language file (argument: tools/source-1.0.2/english/lang) translates "Disc %u" with
//      one %u.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/disc_catalog.cpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

void CheckNames() {
  namespace md = xbox360ps5::multidisc;
  const std::string bf3 = "/mnt/ext1/xbox360/Battlefield 3 (USA, Europe) (En,Fr,De,Es,It) (Disc 1).iso";
  Expect(md::DiscOfImage(bf3) == 1, "Battlefield 3's Disc 1 image");
  Expect(md::DiscOfImage("/mnt/usb0/games/Battlefield 3 (USA, Europe) (Disc 2).iso") == 2, "Disc 2 image");
  Expect(md::DiscOfImage("/data/games/Mass Effect 2 (Disc 2)") == 2, "an extracted disc's folder");
  Expect(md::DiscOfImage("/data/games/Lost Odyssey Disk 3.iso") == 3, "Disk N");
  Expect(md::DiscOfImage("/data/games/Juego (Disco 2).iso") == 2, "Disco N");
  Expect(md::DiscOfImage("/data/games/Halo 3 (USA).iso") == 0, "a single-disc image: none");
  Expect(md::DiscOfImage("/data/games/Halo 3.iso") == 0, "a title ending in a number is no disc");
  Expect(md::DiscOfImage("/data/games/Discovery 2.iso") == 0, "a word starting with Disc is no disc");

  Expect(md::LibraryName("Battlefield 3", 1) == "Battlefield 3 (Disc 1)", "the cached title gets the disc");
  Expect(md::LibraryName("Battlefield 3", 2) == "Battlefield 3 (Disc 2)", "the other disc differs");
  Expect(md::LibraryName("Battlefield 3 (USA, Europe) (En,Fr,De,Es,It) (Disc 1)", 1) ==
             "Battlefield 3 (USA, Europe) (En,Fr,De,Es,It) (Disc 1)",
         "a file name that names its disc is kept");
  Expect(md::LibraryName("Mass Effect 2 Disc 2", 2) == "Mass Effect 2 Disc 2", "a folder name that names its disc is kept");
  Expect(md::LibraryName("Halo 3", 0) == "Halo 3", "a single-disc game is unchanged");
  Expect(md::LibraryName("", 2) == " (Disc 2)", "an empty name still says the disc");
}

void CheckTree() {
  const std::string header = Read("include/xbox360ps5/launcher.hpp");
  Expect(header.find("unsigned disc = 0;") != std::string::npos, "GameEntry has the disc");
  const std::string launcher = Read("src/launcher.cpp");
  // Fix 89 adds, on the same line, that this game's name came from its first launch.
  const size_t cached = launcher.find("if (std::getline(info, title) && !title.empty()) { game.name = title;");
  const size_t grouped = launcher.find("game.name = fs::path(game.folder).filename().string();", cached);
  const size_t disc = launcher.find(
      "game.disc = multidisc::DiscOfImage(game.kind == \"XEX\" ? game.path.parent_path() : game.path);", cached);
  const size_t name = launcher.find("game.name = multidisc::LibraryName(game.name, game.disc);", cached);
  const size_t cover = launcher.find("// Cover: a picture the user put beside the game", cached);
  Expect(cached != std::string::npos && grouped != std::string::npos && disc != std::string::npos &&
             name != std::string::npos && cover != std::string::npos,
         "the scan's naming is there");
  Expect(grouped < disc && disc < name && name < cover, "the disc is named after the cached title, outside groups");
  Expect(launcher.find("} else {", grouped) < disc, "an extracted group keeps its folder's name");
  const size_t line = launcher.find("std::string line = game.kind;");
  const size_t details = launcher.find("std::snprintf(disc, sizeof(disc), Tr(\"Disc %u\"), game.disc);", line);
  const size_t size = launcher.find("if (!game.size.empty()) line += \"   ·   \" + game.size;", line);
  Expect(line != std::string::npos && details != std::string::npos && size != std::string::npos && details < size,
         "the details line names the disc before the size");
}

void CheckLanguages(const std::string& folder) {
  namespace fs = std::filesystem;
  int files = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++files;
    const std::string text = Read(entry.path().string());
    const size_t at = text.find("\nDisc\\s%u = ");
    Expect(at != std::string::npos, entry.path().filename().string() + " translates \"Disc %u\"");
    if (at == std::string::npos) continue;
    const size_t start = at + std::string("\nDisc\\s%u = ").size();
    const std::string value = text.substr(start, text.find('\n', start) - start);
    size_t count = 0;
    for (size_t p = value.find('%'); p != std::string::npos; p = value.find('%', p + 1)) ++count;
    Expect(count == 1 && value.find("%u") != std::string::npos,
           entry.path().filename().string() + ": one %u in " + value);
  }
  Expect(files == 16, "16 language files (" + std::to_string(files) + ")");
}

}  // namespace

int main(int argc, char** argv) {
  CheckNames();
  CheckTree();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "the language folder is given");
  if (failures) {
    std::printf("check-library-disc: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-library-disc: ok\n");
  return 0;
}
