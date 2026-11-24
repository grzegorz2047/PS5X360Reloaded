// SPDX-License-Identifier: MIT
// Fix 155c (assets/patches/545408A7 - Grand Theft Auto V.patch.toml): GTA V's audio freeze in the
// prologue. The mixer counts a stream's decoded bytes (836A96D8, "Available") from the XMA hardware
// context when its stream set is not locked, but takes them (836A9768) from its own copy, which only
// a lock and snapshot refresh; the emulator publishes decoder progress to the context at once, so on
// the hand-over to a prebuffered stream the two disagree and the audio thread spins forever. The
// patch turns Available's "bne" at 836A96FC into "b": it always counts from the game's copy.
//  (a) the patch file: title 545408A7, the default.xex hash both discs boot, enabled, exactly one be32
//      (836A96FC = 48000008) and nothing else;
//  (b) with XBOX360PS5_GTA5_IMAGE naming an unpacked default.xex image (base 82000000; the game is
//      never in the repository), the code shape: the word replaced is 409A0008 (bne cr6, +8), Available
//      starts with lwz r11,4(r3) / the synced-flag test, the hardware path is lwz r11,0x40(r9), and
//      Take starts at 836A9768 (mflr r12);
//  (c) the assembled tree (when run from its root): on the PS5 the emulator reads the package's
//      patches and then the user's from <data>/assets/patches, the folder the library's Patches tab
//      names (before 155c it read only /app0/assets/patches, so user patch files were never loaded).
// Run from the repository root or the assembled tree's root (run-fix-checks.sh passes the repo).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
  return text.str();
}
size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
  return n;
}

void PatchFile(const fs::path& repo) {
  const std::string toml = Read(repo / "assets/patches/545408A7 - Grand Theft Auto V.patch.toml");
  Expect(!toml.empty(), "(a) the patch file is there");
  Expect(toml.find("title_id = \"545408A7\"") != std::string::npos &&
             toml.find("hash = \"C290B43AEEFA5166\"") != std::string::npos,
         "(a) title 545408A7, the executable hash both discs boot");
  Expect(Count(toml, "[[patch]]") == 1 && Count(toml, "is_enabled = true") == 1 &&
             Count(toml, "[[patch.be32]]") == 1 && Count(toml, "address = ") == 1 &&
             toml.find("address = 0x836a96fc") != std::string::npos &&
             toml.find("value = 0x48000008") != std::string::npos,
         "(a) one enabled patch, one be32: 836A96FC = 48000008");
}

uint32_t Word(const std::vector<uint8_t>& image, uint32_t address) {
  const size_t at = address - 0x82000000u;
  return uint32_t(image[at]) << 24 | uint32_t(image[at + 1]) << 16 | uint32_t(image[at + 2]) << 8 | image[at + 3];
}
void Image(const char* path) {
  const std::string bytes = Read(path);
  const std::vector<uint8_t> image(bytes.begin(), bytes.end());
  Expect(image.size() > 0x16AA000, "(b) the image is a whole default.xex memory image");
  if (image.size() <= 0x16AA000) return;
  Expect(Word(image, 0x836A96FC) == 0x409A0008, "(b) the word replaced is bne cr6, +8");
  Expect(Word(image, 0x836A96D8) == 0x81630004 && Word(image, 0x836A96DC) == 0x556A039C &&
             Word(image, 0x836A96F8) == 0x7D2B4B78 && Word(image, 0x836A9700) == 0x81690040 &&
             Word(image, 0x836A9704) == 0x80EB0004,
         "(b) Available: the flags word, the synced-flag test, the copy, the hardware path through +0x40");
  Expect(Word(image, 0x836A9768) == 0x7D8802A6 && Word(image, 0x836A977C) == 0x81630008,
         "(b) Take begins at 836A9768 and reads the set's own entries");
}

void Tree() {
  const std::string patches = Read("src/game_patches.cpp");
  const size_t folders = patches.find("std::vector<fs::path> PatchFolders() {");
  const size_t ps5 = patches.find("#if XE_PLATFORM_PS5", folders);
  Expect(folders != std::string::npos &&
             patches.find("return {\"/app0/assets/patches\", XBOX360PS5_DATA_ROOT \"/assets/patches\"};", ps5) <
                 patches.find("#else", ps5) &&
             patches.find("#include \"xbox360ps5/app_identity.hpp\"") != std::string::npos,
         "(c) the package's patches, then the user's in <data>/assets/patches");
  const std::string identity = Read("include/xbox360ps5/app_identity.hpp");
  const std::string launcher = Read("src/launcher.cpp");
  Expect(identity.find("#define XBOX360PS5_DATA_ROOT \"/data/homebrew/\" XBOX360PS5_TITLE_ID") != std::string::npos &&
             identity.find("#define XBOX360PS5_TITLE_ID \"PPSA83600\"") != std::string::npos &&
             launcher.find("in /data/homebrew/PPSA83600/assets/patches/.") != std::string::npos,
         "(c) the folder the Patches tab names is the one read");
}

}  // namespace

int main(int argc, char** argv) {
  fs::path repo = argc > 1 ? fs::path(argv[1]) : fs::current_path();
  PatchFile(repo);
  if (fs::exists("src/game_patches.cpp")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (const char* image = std::getenv("XBOX360PS5_GTA5_IMAGE")) Image(image);
  else std::printf("note: XBOX360PS5_GTA5_IMAGE not set, the game's code shape is not checked here\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("gta5 audio patch: all checks passed\n");
  return 0;
}
