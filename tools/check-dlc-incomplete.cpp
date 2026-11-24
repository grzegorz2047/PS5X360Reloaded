// SPDX-License-Identifier: MIT
// Fix 148f (tools/source-1.0.2/fixes/148f-dlc-incomplete-mark.patch): the game sheet's DLCs tab marks a
// package whose file is shorter than its STFS header says (an install that stopped, such as Grand
// Theft Auto V's), "incomplete: 1.4 GB of 2.1 GB" in the warning colour, so the player knows which
// file to remove there (fix 109's Triangle).
//  (a) dlc2::Read: a made-up PIRS package (type 00000002, an STFS volume, read-only, 231 blocks: Dance
//      Central 3's nuisp1033 shape) expects its header plus DataNeeded; an SVOD volume expects nothing;
//  (b) FindAll on a made-up drive: the whole file is not cut short, the same file cut short is, with
//      the sizes the row shows;
//  (c) the launcher draws "incomplete: %s of %s" with both sizes in the warning colour for such a row;
//  (d) every language file translates the text, with its two %s.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh) with the language
// folder as argument.
#undef NDEBUG
#include "../port/dlc_manager.hpp"

#include <cstdio>
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
void Put32(std::vector<unsigned char>& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v >> 24), b[at + 1] = uint8_t(v >> 16), b[at + 2] = uint8_t(v >> 8), b[at + 3] = uint8_t(v);
}
// A package file: PIRS, title 545408A7, type 2, header 0xAD0E bytes, the volume given, `size` bytes.
void Package(const fs::path& path, uint32_t volume, uint32_t blocks, uint64_t size) {
  std::vector<unsigned char> head(0x1000, 0);
  head[0] = 'P', head[1] = 'I', head[2] = 'R', head[3] = 'S';
  Put32(head, 0x340, 0xAD0E);
  Put32(head, 0x344, 0x00000002);
  Put32(head, 0x360, 0x545408A7);
  head[0x379 + 2] = 1;  // read-only format
  Put32(head, 0x379 + 0x1C, blocks);
  Put32(head, 0x3A9, volume);
  const char* name = "Install data";
  for (int i = 0; name[i]; ++i) head[0x411 + 2 * i + 1] = uint8_t(name[i]);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(head.data()), std::streamsize(head.size()));
  out.close();
  fs::resize_file(path, size);
}

void Headers(const fs::path& scratch) {
  const uint64_t whole = 0xB000 + 958464;  // 231 blocks need 958464 bytes after the 0xB000 header
  Package(scratch / "stfs", 0, 231, whole);
  const auto h = xbox360ps5::dlc2::Read(scratch / "stfs");
  Expect(h.package && h.type == 2 && h.title == 0x545408A7, "(a) the made-up package reads");
  Expect(h.expected == whole, "(a) an STFS package expects its header plus its blocks");
  Package(scratch / "svod", 1, 231, whole);
  Expect(xbox360ps5::dlc2::Read(scratch / "svod").expected == 0, "(a) an SVOD volume expects nothing");
}

void Rows(const fs::path& scratch) {
  namespace dlc2 = xbox360ps5::dlc2;
  const fs::path root = scratch / "drive";
  const fs::path folder = dlc2::ActiveFolder(root, 0x545408A7);
  fs::create_directories(folder);
  const uint64_t whole = 0xB000 + 958464;
  Package(folder / "545408A700000000", 0, 231, whole);
  Package(folder / "545408A700000001", 0, 231, whole - 300000);
  const auto found = dlc2::FindAll(0x545408A7, {{root, "M.2"}}, {});
  Expect(found.items.size() == 2, "(b) both packages are listed");
  for (const auto& item : found.items) {
    const bool short_one = item.file == "545408A700000001";
    Expect(dlc2::CutShort(item) == short_one, "(b) " + item.file + (short_one ? " is cut short" : " is whole"));
    Expect(item.expected == whole, "(b) " + item.file + " expects the header's size");
  }
}

void Tree(const fs::path& lang) {
  const std::string launcher = Read("src/launcher.cpp");
  const size_t mark = launcher.find("dlc2::CutShort(item)");
  Expect(mark != std::string::npos && launcher.find("Tr(\"incomplete: %s of %s\")", mark) != std::string::npos &&
             launcher.find("dlc2::SizeText(item.expected)", mark) != std::string::npos &&
             launcher.find("cut_short ? kWarning", mark) != std::string::npos,
         "(c) the DLCs tab draws the incomplete row with both sizes in the warning colour");
  for (const char* language : {"pl", "de", "fr", "es", "es-419", "it", "ja", "ko", "nl", "pt", "pt-BR", "ru", "tr",
                               "zh-Hans", "zh-Hant"}) {
    const std::string file = Read(lang / (std::string(language) + ".txt"));
    const size_t at = file.find("\nincomplete:\\s%s\\sof\\s%s = ");
    bool two = false;
    if (at != std::string::npos) {
      const std::string line = file.substr(at + 1, file.find('\n', at + 1) - at - 1);
      const std::string value = line.substr(line.find(" = ") + 3);
      size_t count = 0;
      for (size_t p = value.find("%s"); p != std::string::npos; p = value.find("%s", p + 2)) ++count;
      two = count == 2;
    }
    Expect(two, std::string("(d) ") + language + " translates the text with two %s");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path scratch = fs::temp_directory_path() / "check-dlc-incomplete";
  std::error_code error;
  fs::remove_all(scratch, error);
  fs::create_directories(scratch);
  Headers(scratch);
  Rows(scratch);
  fs::remove_all(scratch, error);
  if (fs::exists("src/launcher.cpp") && argc > 1) Tree(argv[1]);
  else std::printf("note: no assembled tree or language folder here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("dlc incomplete: all checks passed\n");
  return 0;
}
