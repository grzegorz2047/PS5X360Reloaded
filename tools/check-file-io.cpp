// SPDX-License-Identifier: MIT
// Fix 148d (tools/source-1.0.2/fixes/148d-file-io-line.patch): GTA V's slow install phase showed
// no 148c line: it reads no package file, it copies the install disc's package files themselves.
// The 30 s "File I/O (fix 148d)" line counts every guest file read and write by where it goes
// (disc image, drive), their sizes (with the largest) and time, set-length calls, and the
// KeDelayExecutionThread calls of threads doing file I/O with the time asked and really slept.
//  (a) package_io.hpp: Add keeps the largest; FileIoReport: the first call starts the period, a
//      line after 30 s with the numbers, none for a period without file I/O, one caller per period;
//      FileIoThread: 1 s after the thread's last file I/O;
//  (b) the assembled tree: disc image reads, drive reads and writes, set length counted with their
//      time; the delays of file I/O threads measured around KeDelayExecutionThread.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

void Counting() {
  pio::FileIo io;
  Expect(pio::FileIoReport(io, 1000).empty(), "(a) the first call starts the period");
  pio::Add(io.disc_reads, 65536, 2000000);
  pio::Add(io.disc_reads, 32768, 1000000);
  Expect(io.disc_reads.largest == 65536 && io.disc_reads.calls == 2 && io.disc_reads.bytes == 98304,
         "(a) Add counts calls and bytes and keeps the largest");
  for (int n = 0; n < 98; ++n) pio::Add(io.disc_reads, 32768, 1000000);
  for (int n = 0; n < 100; ++n) pio::Add(io.host_writes, 32768, 500000);
  pio::Add(io.set_length, 2147262464, 900000000);
  io.delays = 100;
  io.asked_100ns = 100 * 10000;   // 1 ms asked each
  io.slept_ns = 100 * 15000000;   // 15 ms slept each
  Expect(pio::FileIoReport(io, 1000 + 29000000000ull).empty(), "(a) no line before 30 s");
  const std::string line = pio::FileIoReport(io, 1000 + 30000000000ull);
  Expect(line.find("File I/O (fix 148d), last 30 s: ") == 0, "(a) the line's start");
  Expect(line.find("disc image reads 100 (3.2 MiB, 32 KiB each, largest 64 KiB, 0.11 MiB/s, 1010 us each)") !=
             std::string::npos,
         "(a) disc image reads");
  Expect(line.find("drive reads 0 (0.0 MiB") != std::string::npos, "(a) drive reads");
  Expect(line.find("drive writes 100 (3.1 MiB, 32 KiB each, largest 32 KiB") != std::string::npos &&
             line.find("500 us each") != std::string::npos,
         "(a) drive writes");
  Expect(line.find("set length 1 (900 ms in all)") != std::string::npos, "(a) set length");
  Expect(line.find("KeDelayExecutionThread of threads doing file I/O 100 (1.00 ms asked, 15.00 ms slept each; 1500 ms slept in all)") !=
             std::string::npos,
         "(a) asked and slept");
  Expect(io.disc_reads.calls == 0 && io.delays == 0, "(a) the period is reset");
  Expect(pio::FileIoReport(io, 1000 + 60000000000ull).empty(), "(a) no line for a period without file I/O");
  pio::Add(io.host_reads, 4096, 1000);
  Expect(!pio::FileIoReport(io, 1000 + 90000000000ull).empty() &&
             pio::FileIoReport(io, 1000 + 90000000001ull).empty(),
         "(a) one line per period");
  pio::last_file_io_ns = 0;
  Expect(!pio::FileIoThread(5000000000ull), "(a) no file I/O: not a file I/O thread");
  pio::last_file_io_ns = 5000000000ull;
  Expect(pio::FileIoThread(5900000000ull) && !pio::FileIoThread(6000000001ull),
         "(a) a file I/O thread for 1 s");
}

void Tree() {
  const std::string disc = Read(".deps/xenia-canary/src/xenia/vfs/devices/disc_image_file.cc");
  Expect(disc.find("pio::Add(pio::file_io.disc_reads, real_length,") != std::string::npos &&
             disc.find("pio::FileIoReport(pio::file_io, now_ns)") != std::string::npos,
         "(b) disc image reads are counted and report");
  const std::string host = Read(".deps/xenia-canary/src/xenia/vfs/devices/host_path_file.cc");
  Expect(host.find("pio::Add(pio::file_io.host_reads, *out_bytes_read,") != std::string::npos,
         "(b) drive reads are counted");
  Expect(host.find("pio::Add(pio::file_io.host_writes, written ? *out_bytes_written : 0,") != std::string::npos,
         "(b) drive writes are counted");
  Expect(host.find("pio::Add(pio::file_io.set_length, uint64_t(length),") != std::string::npos,
         "(b) set length is counted");
  const std::string threading = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc");
  const size_t delay = threading.find("dword_result_t KeDelayExecutionThread_entry(");
  Expect(delay != std::string::npos && threading.find("pio::FileIoThread(now_ns)", delay) != std::string::npos &&
             threading.find("pio::file_io.slept_ns.fetch_add(", delay) != std::string::npos &&
             threading.find("pio::file_io.asked_100ns.fetch_add(asked)", delay) != std::string::npos,
         "(b) delays of file I/O threads: asked and slept");
}

}  // namespace

int main() {
  Counting();
  if (fs::exists(".deps/xenia-canary")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("file io: all checks passed\n");
  return 0;
}
