// SPDX-License-Identifier: MIT
// Fix 148h (tools/source-1.0.2/fixes/148h-install-notice.patch): after fix 148e refused a cut-short
// install package and the game then stopped on its dirty-disc error (Grand Theft Auto V), the
// library explains what to do: the game's sheet, its DLCs tab, Remove on each install package, start
// the game again. Nothing is deleted for the player.
//  (a) install_notice.hpp: Serialize / Parse (a line break in a value cannot split it; missing or
//      equal sizes are no notice); Check on made-up package files (a cut-short STFS one gives the
//      display name and both sizes, a whole one and a file that is no package give none, a package
//      without a display name gives the file's name); NoteRefused keeps each path once, 32 at most;
//      Message fills the six places in order;
//  (b) the core: OpenContent notes a package file that does not mount; the dirty-disc path writes
//      the notice for the first refused package still cut short, before returning to the library;
//  (c) the launcher reads the notice once, builds the message with Tr("DLCs") and Tr("Remove"),
//      removes the file (unlink), shows the window and closes it with X or Circle;
//  (d) every language file translates the title and the message with its six %s.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh) with the language
// folder as argument.
#undef NDEBUG
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/install_notice.hpp"

namespace {
namespace fs = std::filesystem;
namespace in = xbox360ps5::install_notice;

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
void Put32(std::vector<unsigned char>& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v >> 24), b[at + 1] = uint8_t(v >> 16), b[at + 2] = uint8_t(v >> 8), b[at + 3] = uint8_t(v);
}
// A made-up STFS package: header 0xAD0E bytes, 231 read-only blocks (needs 0xB000 + 958464 bytes).
void Package(const fs::path& path, const char* name, uint64_t size, const char* magic = "PIRS") {
  std::vector<unsigned char> head(0x1000, 0);
  std::memcpy(head.data(), magic, 4);
  Put32(head, 0x340, 0xAD0E);
  Put32(head, 0x344, 0x00000002);
  Put32(head, 0x360, 0x545408A7);
  head[0x379 + 2] = 1;
  Put32(head, 0x379 + 0x1C, 231);
  for (int i = 0; name[i]; ++i) head[0x411 + 2 * i + 1] = uint8_t(name[i]);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(head.data()), std::streamsize(head.size()));
  out.close();
  fs::resize_file(path, size);
}

void Notices(const fs::path& scratch) {
  in::Notice n{"Grand Theft Auto V", "gtav - part1", 947867648, 2147262464};
  const auto back = in::Parse(in::Serialize(n));
  Expect(back && back->game == n.game && back->package == n.package && back->have == n.have && back->need == n.need,
         "(a) Serialize and Parse round-trip");
  Expect(in::Parse(in::Serialize({"A\nB", "p\r\nq", 1, 2}))->game == "A B", "(a) a line break in a value is a space");
  Expect(!in::Parse("game=x\npackage=y\nhave=5\n") && !in::Parse(in::Serialize({"x", "y", 5, 5})) &&
             !in::Parse(""),
         "(a) a notice needs both sizes, the second larger");
  Expect(in::FileName("/data/homebrew/PPSA83600") == "/data/homebrew/PPSA83600/install-notice.txt", "(a) the file");

  const uint64_t whole = 0xB000 + 958464;
  Package(scratch / "cut", "gtav - part1", whole - 500000);
  const auto cut = in::Check((scratch / "cut").string(), "Grand Theft Auto V");
  Expect(cut && cut->package == "gtav - part1" && cut->have == whole - 500000 && cut->need == whole &&
             cut->game == "Grand Theft Auto V",
         "(a) a cut-short package gives its name and both sizes");
  Package(scratch / "whole", "gtav - part0", whole);
  Expect(!in::Check((scratch / "whole").string(), "x"), "(a) a whole package gives no notice");
  Package(scratch / "545408A700000002", "", whole - 1);
  const auto unnamed = in::Check((scratch / "545408A700000002").string(), "x");
  Expect(unnamed && unnamed->package == "545408A700000002", "(a) without a display name, the file's name");
  Package(scratch / "notpkg", "x", whole - 1, "XEX2");
  Expect(!in::Check((scratch / "notpkg").string(), "x") && !in::Check((scratch / "missing").string(), "x"),
         "(a) a file that is no package, or no file, gives no notice");

  in::NoteRefused("/a");
  in::NoteRefused("/a");
  in::NoteRefused("/b");
  Expect(in::RefusedPaths() == std::vector<std::string>{"/a", "/b"}, "(a) each refused path once");
  for (int i = 0; i < 40; ++i) in::NoteRefused("/x" + std::to_string(i));
  Expect(in::RefusedPaths().size() == 32, "(a) 32 refused paths at most");

  const std::string line = in::Message("%s|%s|%s|%s|%s|%s", n, "948 MB", "2.1 GB", "DLC", "Usuń");
  Expect(line == "Grand Theft Auto V|gtav - part1|948 MB|2.1 GB|DLC|Usuń", "(a) Message fills the places in order");
}

void Tree(const fs::path& lang) {
  const std::string content = Read(".deps/xenia-canary/src/xenia/kernel/xam/content_manager.cc");
  const size_t mount = content.find("auto package = OpenAndMountPackage(package_path, root_name, root_path);");
  Expect(mount != std::string::npos &&
             content.find("xbox360ps5::install_notice::NoteRefused(package_path.string());", mount) <
                 content.find("package->set_device_id(", mount),
         "(b) OpenContent notes a package that does not mount");
  const std::string ui = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_ui.cc");
  const size_t dirty = ui.find("BULLYTRACE dirty-disc");
  const size_t check = ui.find("xbox360ps5::install_notice::Check(", dirty);
  const size_t relaunch = ui.find("xbox360ps5::RelaunchInto(std::string(), {});", dirty);
  Expect(dirty != std::string::npos && check < relaunch &&
             ui.find("xbox360ps5::install_notice::Serialize(*notice)", check) < relaunch &&
             ui.find("XBOX360PS5_DATA_ROOT", check) < relaunch,
         "(b) the dirty-disc path writes the notice before returning to the library");
  const std::string launcher = Read("src/launcher.cpp");
  const size_t load = launcher.find("notice_file::FileName(XBOX360PS5_DATA_ROOT)");
  Expect(load != std::string::npos && launcher.find("notice_file::Parse(", load) != std::string::npos &&
             launcher.find("Tr(\"DLCs\"), Tr(\"Remove\")", load) != std::string::npos &&
             launcher.find("::unlink(file.c_str())", load) != std::string::npos,
         "(c) the launcher reads the notice once, names the tab and the action, removes the file");
  Expect(launcher.find("Tr(\"Install data incomplete\")") != std::string::npos &&
             launcher.find("if (key == Key::cross || key == Key::circle) g_install_notice_open = false;") !=
                 std::string::npos,
         "(c) the window, closed with X or Circle");
  const std::string key =
      "%s:\\sits\\sinstall\\sdata\\sis\\sincomplete\\s(%s:\\s%s\\sof\\s%s).\\sIn\\sthe\\slibrary\\spress\\sCross\\son"
      "\\sthe\\sgame,\\sopen\\sthe\\s%s\\stab,\\sselect\\seach\\sof\\sits\\sinstall\\spackages\\sand\\spress\\sTriangle"
      "\\s(%s).\\sThen\\sstart\\sthe\\sgame\\sto\\sinstall\\sit\\sagain.";
  for (const char* language : {"pl", "de", "fr", "es", "es-419", "it", "ja", "ko", "nl", "pt", "pt-BR", "ru", "tr",
                               "zh-Hans", "zh-Hant"}) {
    const std::string file = Read(lang / (std::string(language) + ".txt"));
    Expect(file.find("\nInstall\\sdata\\sincomplete = ") != std::string::npos,
           std::string("(d) ") + language + " translates the title");
    const size_t at = file.find("\n" + key + " = ");
    size_t count = 0;
    if (at != std::string::npos) {
      const std::string line = file.substr(at + 1, file.find('\n', at + 1) - at - 1);
      const std::string value = line.substr(line.find(" = ") + 3);
      for (size_t p = value.find("%s"); p != std::string::npos; p = value.find("%s", p + 2)) ++count;
    }
    Expect(count == 6, std::string("(d) ") + language + " translates the message with six %s");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path scratch = fs::temp_directory_path() / "check-install-notice";
  std::error_code error;
  fs::remove_all(scratch, error);
  fs::create_directories(scratch);
  Notices(scratch);
  fs::remove_all(scratch, error);
  if (fs::exists("src/launcher.cpp") && argc > 1) Tree(argv[1]);
  else std::printf("note: no assembled tree or language folder here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("install notice: all checks passed\n");
  return 0;
}
