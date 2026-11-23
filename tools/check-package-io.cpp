// SPDX-License-Identifier: MIT
// Fix 148c (tools/source-1.0.2/fixes/148c-package-read-runs.patch): files inside a content package
// were read one 4 KiB block at a time, each block a pread of the disc image (GTA V's install from
// the disc copied about 2 MB/s). Records that continue each other are now read as one run, and a
// 30 s line measures an install.
//  (a) package_io.hpp AddPiece: contiguous pieces of one file merge, another file / a gap / an empty
//      piece do not; a model of an STFS package (a hash block after every 170 data blocks): a 1 MiB
//      read at block 0 takes 2 host reads instead of 256, one at block 150 takes 3;
//  (b) Due (the first read starts the period, one report per 30 s, one caller wins), Take (resets),
//      Describe (the numbers in the line), ReaderThread (1 s after a package read);
//  (c) the assembled tree: ReadSync plans the records with AddPiece, reads the runs, fails an
//      incomplete run, counts and reports; host file writes and the reading threads'
//      KeDelayExecutionThread (relative intervals) are counted.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/package_io.hpp"

namespace {
namespace fs = std::filesystem;
namespace pio = xbox360ps5::package_io;

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

// Physical offset of STFS data block n: a hash block before every group of 170 data blocks.
size_t StfsOffset(size_t block) {
  const size_t header = 0xB000;
  return header + (block + block / 170 + 1) * 0x1000;
}
size_t HostReads(size_t first_block, size_t bytes) {
  std::vector<pio::Piece> runs;
  for (size_t n = first_block; bytes; ++n) {
    const size_t take = bytes < 0x1000 ? bytes : 0x1000;
    pio::AddPiece(runs, {0, StfsOffset(n), take});
    bytes -= take;
  }
  return runs.size();
}

void Merging() {
  std::vector<pio::Piece> runs;
  pio::AddPiece(runs, {0, 0x1000, 0x1000});
  pio::AddPiece(runs, {0, 0x2000, 0x1000});
  Expect(runs.size() == 1 && runs[0].offset == 0x1000 && runs[0].length == 0x2000,
         "(a) contiguous pieces of one file merge");
  pio::AddPiece(runs, {1, 0x3000, 0x1000});
  Expect(runs.size() == 2, "(a) another file starts a run");
  pio::AddPiece(runs, {1, 0x5000, 0x1000});
  Expect(runs.size() == 3, "(a) a gap starts a run");
  pio::AddPiece(runs, {1, 0x6000, 0});
  Expect(runs.size() == 3 && runs[2].length == 0x1000, "(a) an empty piece adds nothing");
  pio::AddPiece(runs, {1, 0x6000, 0x800});
  Expect(runs.size() == 3 && runs[2].length == 0x1800, "(a) a partial piece continues its run");
  Expect(HostReads(0, 1 << 20) == 2, "(a) STFS model: 1 MiB from block 0 is 2 host reads (was 256)");
  Expect(HostReads(150, 1 << 20) == 3, "(a) STFS model: 1 MiB from block 150 is 3 host reads");
  Expect(HostReads(0, 4096) == 1, "(a) one block is one host read");
}

void Reporting() {
  pio::Counters c;
  Expect(!pio::Due(c, 1000), "(b) the first read starts the period");
  Expect(!pio::Due(c, 1000 + 29000000000ull), "(b) no report before 30 s");
  Expect(pio::Due(c, 1000 + 30000000000ull), "(b) a report after 30 s");
  Expect(!pio::Due(c, 1000 + 30000000001ull), "(b) only one report per period");
  c.guest_reads = 100;
  c.read_bytes = 100ull << 20;
  c.host_reads = 200;
  c.read_ns = 2000000000ull;
  c.writes = 50;
  c.write_bytes = 100ull << 20;
  c.write_ns = 500000000ull;
  c.delays = 30;
  c.delay_100ns = 300000;
  const pio::Period p = pio::Take(c);
  Expect(p.guest_reads == 100 && p.host_reads == 200 && p.delays == 30 && c.guest_reads == 0 &&
             c.read_bytes == 0 && c.delays == 0,
         "(b) Take returns the period and resets it");
  const std::string line = pio::Describe(p, 30.0);
  Expect(line.find("Package I/O (fix 148c), last 30 s") == 0, "(b) the line's start");
  Expect(line.find("guest reads of package files 100 (100.0 MiB, 1024 KiB each, 3.33 MiB/s)") !=
             std::string::npos,
         "(b) guest reads, size and rate");
  Expect(line.find("host reads 200 (512 KiB each)") != std::string::npos, "(b) host reads");
  Expect(line.find("read time 2000 ms (20000 us per guest read)") != std::string::npos, "(b) read time");
  Expect(line.find("host file writes 50 (100.0 MiB, 2048 KiB each), write time 500 ms (10000 us each)") !=
             std::string::npos,
         "(b) writes");
  Expect(line.find("KeDelayExecutionThread of the reading threads 30 (1.0 ms asked each, 30 ms in all)") !=
             std::string::npos,
         "(b) delays");
  pio::last_package_read_ns = 0;
  Expect(!pio::ReaderThread(5000000000ull), "(b) a thread that read no package is no reader");
  pio::last_package_read_ns = 5000000000ull;
  Expect(pio::ReaderThread(5500000000ull) && !pio::ReaderThread(6000000001ull),
         "(b) a reader for 1 s after its last package read");
}

void Tree() {
  const std::string file = Read(".deps/xenia-canary/src/xenia/vfs/devices/xcontent_container_file.cc");
  const size_t read = file.find("X_STATUS XContentContainerFile::ReadSync(");
  Expect(read != std::string::npos, "(c) ReadSync is there");
  Expect(file.find("pio::AddPiece(runs,", read) != std::string::npos, "(c) the records are planned as runs");
  Expect(file.find("for (const auto& run : runs)", read) != std::string::npos &&
             file.find("Read(std::span<uint8_t>(p, run.length), run.offset, run.file)", read) !=
                 std::string::npos,
         "(c) each run is one Read");
  Expect(file.find("if (num_read != run.length)", read) != std::string::npos,
         "(c) an incomplete run fails the read");
  Expect(file.find("Read(std::span<uint8_t>(p, read_length)", read) == std::string::npos,
         "(c) no read per block any more");
  Expect(file.find("pio::Due(counters", read) != std::string::npos &&
             file.find("pio::Describe(pio::Take(counters)", read) != std::string::npos,
         "(c) the 30 s line");
  const std::string host = Read(".deps/xenia-canary/src/xenia/vfs/devices/host_path_file.cc");
  const size_t write = host.find("X_STATUS HostPathFile::WriteSync(");
  Expect(write != std::string::npos && host.find("counters.writes.fetch_add(1)", write) != std::string::npos &&
             host.find("counters.write_ns.fetch_add(", write) != std::string::npos,
         "(c) host file writes are counted with their time");
  const std::string threading = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc");
  const size_t delay = threading.find("dword_result_t KeDelayExecutionThread_entry(");
  Expect(delay != std::string::npos &&
             threading.find("pio::ReaderThread(now_ns) && int64_t(interval) < 0", delay) != std::string::npos &&
             threading.find("pio::counters.delay_100ns.fetch_add(", delay) != std::string::npos,
         "(c) the reading threads' relative delays are counted");
}

}  // namespace

int main() {
  Merging();
  Reporting();
  if (fs::exists(".deps/xenia-canary")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("package io: all checks passed\n");
  return 0;
}
