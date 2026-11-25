// SPDX-License-Identifier: MIT
// Fix 148e (tools/source-1.0.2/fixes/148e-stfs-cut-short.patch): an STFS package whose file is
// shorter than its header says (Grand Theft Auto V's install stopped half-way) is not mounted, so the
// game no longer spins on empty reads past the cut.
//  (a) stfs_size.hpp: BlockToOffset for read-only (one block per hash table) and two-block packages;
//      DataNeeded equals the data size of every real package measured on the console (GTA V's four
//      install packages and nxeart, Dance Central 3's database.xmplr, nuiidentity.bin.be, nuisp1033,
//      nxeart, Battlefield 3's own package), and the stopped install (about 1.44 GB of the 2.15 GB package
//      545408A700000001) is cut short; one byte less than needed is cut short, the exact size is
//      not; no blocks need nothing;
//  (b) the core (stfs_container_device.cc): Read() checks the data size against DataNeeded with the
//      device's blocks per hash table before reading the file table, refuses a cut-short package
//      with a read error and logs it with "fix 148e".
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/stfs_size.hpp"

namespace {
namespace fs = std::filesystem;
namespace ss = xbox360ps5::stfs_size;

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

void Sizes() {
  Expect(ss::BlockToOffset(0, 1) == 0x1000, "(a) read-only: block 0 after one hash block");
  Expect(ss::BlockToOffset(169, 1) == 170 * 0x1000, "(a) read-only: block 169");
  Expect(ss::BlockToOffset(170, 1) == 173 * 0x1000, "(a) read-only: block 170 after the next level-0 table and the level-1 table");
  Expect(ss::BlockToOffset(0, 2) == 0x2000, "(a) two blocks per table: block 0");
  struct Real { const char* name; uint32_t blocks; uint64_t data; };
  // total_block_count from each header, data = file size - header (rounded to 4 KiB), read over FTP.
  const Real real[] = {
      {"GTA V 545408A700000000", 520987, 2146598912ull}, {"GTA V 545408A700000001", 521137, 2147217408ull},
      {"GTA V 545408A700000002", 521027, 2146762752ull}, {"GTA V 545408A700000003", 441107, 1817473024ull},
      {"GTA V nxeart", 394, 1630208}, {"DC3 database.xmplr", 3184, 13123584}, {"DC3 nuiidentity.bin.be", 782, 3227648},
      {"DC3 nuisp1033", 231, 958464}, {"DC3 nxeart", 234, 970752}, {"BF3 4541095000000000", 382551, 1576210432ull}};
  for (const auto& r : real) {
    Expect(ss::DataNeeded(r.blocks, 1) == r.data, std::string("(a) ") + r.name + " needs exactly its data");
    Expect(!ss::CutShort(r.data, r.blocks, 1), std::string("(a) ") + r.name + " is whole");
    Expect(ss::CutShort(r.data - 1, r.blocks, 1), std::string("(a) ") + r.name + " less one byte is cut short");
  }
  Expect(ss::CutShort(1440000000ull - 0xB000, 521137, 1), "(a) the stopped install (1.44 of 2.15 GB) is cut short");
  Expect(ss::DataNeeded(0, 1) == 0 && !ss::CutShort(0, 0, 2), "(a) no blocks need nothing");
}

void Tree() {
  const std::string device =
      Read(".deps/xenia-canary/src/xenia/vfs/devices/xcontent_devices/stfs_container_device.cc");
  const size_t read = device.find("StfsContainerDevice::Result StfsContainerDevice::Read() {");
  const size_t check = device.find("xbox360ps5::stfs_size::CutShort(data_->size(), total,", read);
  const size_t table = device.find("descriptor_->file_table_block_number()", read);
  Expect(read != std::string::npos && check != std::string::npos && check < table,
         "(b) Read() checks the size before the file table");
  Expect(device.find("blocks_per_hash_table_", check) < device.find(";", check) + 200,
         "(b) with the device's blocks per hash table");
  Expect(device.find("return Result::kReadError;", check) < table, "(b) a cut-short package is refused");
  Expect(device.find("(fix 148e)", read) < table, "(b) and logged");
}

}  // namespace

int main() {
  Sizes();
  if (fs::exists(".deps/xenia-canary")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("stfs cut short: all checks passed\n");
  return 0;
}
