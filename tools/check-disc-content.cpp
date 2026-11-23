// SPDX-License-Identifier: MIT
// Fix 148 (tools/source-1.0.2/fixes/148-disc-content-packages.patch): content packages on the game
// disc. Grand Theft Auto V's install disc keeps its data as four packages in
// content\0000000000000000\545408A7\00000002\ and opens them on the optical drive (device 2);
// they were looked for only in the host content folder, every open failed and the game showed its
// dirty-disc error.
//  (a) disc_content.hpp: the guest paths of a package on the disc (the player's xuid first when
//      there is one, then 0), GTA V's exact path, file names that cannot leave the folder refused,
//      and which dispositions may open a disc package (open-existing and open-always only);
//  (b) the core (xam_content.cc): XamContentCreate on the optical drive, when the host folder has no
//      such package, looks for it on the disc through the file system, counts it as existing for
//      open-existing and open-always, opens it with ContentManager::OpenContentFromGuestFile and logs
//      "Content (fix 148)" with the path and the result; the host content folder stays the fallback.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/disc_content.hpp"

namespace {
namespace fs = std::filesystem;
namespace dc = xbox360ps5::disc_content;

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

void Paths() {
  const auto gta = dc::GuestPackagePaths(0, 0x545408A7, 0x00000002, "545408A700000000");
  Expect(gta.size() == 1 &&
             gta[0] == "game:\\content\\0000000000000000\\545408A7\\00000002\\545408A700000000",
         "(a) GTA V's first package on the disc");
  const auto player = dc::GuestPackagePaths(0xE000012345678901ull, 0x545408A7, 2, "545408A700000003");
  Expect(player.size() == 2 &&
             player[0] == "game:\\content\\E000012345678901\\545408A7\\00000002\\545408A700000003" &&
             player[1] == "game:\\content\\0000000000000000\\545408A7\\00000002\\545408A700000003",
         "(a) the player's xuid first, then 0");
  Expect(dc::GuestPackagePaths(~0ull, 0x545408A7, 2, "x").size() == 1,
         "(a) xuid -1 (any user) is not a folder: only 0");
  Expect(dc::GuestPackagePaths(0, 0x545408A7, 2, "").empty(), "(a) an empty name: no path");
  Expect(dc::GuestPackagePaths(0, 0x545408A7, 2, "..").empty(), "(a) '..': no path");
  Expect(dc::GuestPackagePaths(0, 0x545408A7, 2, "a\\b").empty(), "(a) a backslash: no path");
  Expect(dc::GuestPackagePaths(0, 0x545408A7, 2, "a/b").empty(), "(a) a slash: no path");
  Expect(dc::GuestPackagePaths(0, 0x545408A7, 2, "game:x").empty(), "(a) a drive letter: no path");
  Expect(dc::GuestPackagePaths(0, 0x545408A7, 2, std::string(43, 'a')).empty(),
         "(a) longer than an XCONTENT file name: no path");
  Expect(dc::kOpticalDrive == 2, "(a) the optical drive is device 2");
  Expect(!dc::OpensExisting(1) && !dc::OpensExisting(2) && dc::OpensExisting(3) &&
             dc::OpensExisting(4) && !dc::OpensExisting(5) && dc::OpensExisting(0x13),
         "(a) only open-existing and open-always use a disc package (low 4 bits)");
}

void Tree() {
  const std::string xam = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_content.cc");
  Expect(xam.find("xbox360ps5/disc_content.hpp") != std::string::npos,
         "(b) xam_content.cc includes disc_content.hpp");
  const size_t create = xam.find("dword_result_t xeXamContentCreate(");
  Expect(create != std::string::npos, "(b) xeXamContentCreate is there");
  const size_t disc = xam.find("disc_content::GuestPackagePaths(", create);
  const size_t exists = xam.find("ContentExists(", create);
  Expect(disc != std::string::npos && xam.find("disc_content::kOpticalDrive", create) < disc,
         "(b) the disc is searched for the optical drive");
  Expect(xam.find("ResolvePath(", disc) != std::string::npos,
         "(b) the disc path is resolved through the file system");
  Expect(xam.find("disc_content::OpensExisting(", create) != std::string::npos,
         "(b) only open-existing and open-always take the disc package");
  Expect(xam.find("OpenContentFromGuestFile(", create) != std::string::npos,
         "(b) a disc package opens through OpenContentFromGuestFile");
  Expect(xam.find("Content (fix 148)", create) != std::string::npos,
         "(b) the disc open is logged with fix 148");
  Expect(exists != std::string::npos && xam.find("ContentExists(", disc) != std::string::npos,
         "(b) the host content folder still answers (fallback)");
}

}  // namespace

int main() {
  Paths();
  if (fs::exists(".deps/xenia-canary")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("disc content: all checks passed\n");
  return 0;
}
