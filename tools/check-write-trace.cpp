// SPDX-License-Identifier: MIT
// Fix 148g (tools/source-1.0.2/fixes/148g-write-trace.patch): GTA V's install writes slowed to 0.5 s
// per MiB while the FTP server wrote to the same drive at 15+ MB/s; the cost is measured inside
// PosixFileHandle::Write itself.
//  (a) write_trace.hpp: Sequential (the first write and append-only always, else the offset where the
//      last one ended); ShouldLog (the first 3 writes of a handle, every write over 100 ms, 60 lines a
//      session); Count and TakeSummary (writes, not sequential, slow, pwrite and copy time each, reset);
//      FlagText (access mode, O_APPEND, O_SYNC, O_DIRECT, the value); Describe (file, offset, sizes, flags,
//      sequential or not, pwrite time, the copy's with the bounce switch);
//  (b) the core: Write reads <data>/write-bounce.on once, copies the guest buffer to a thread's host
//      buffer with it, times the copy and the pwrite apart, tracks the next offset per handle, logs with
//      fstat's size and F_GETFL's flags; the 30 s file I/O line (fix 148d) appends the totals.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/write_trace.hpp"

namespace {
namespace fs = std::filesystem;
namespace wt = xbox360ps5::write_trace;

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

void Rules() {
  Expect(wt::Sequential(0, 0, 123456, false), "(a) the first write of a handle is sequential");
  Expect(wt::Sequential(4, 1048576, 1048576, false) && !wt::Sequential(4, 1048576, 0, false),
         "(a) later writes: sequential where the last ended, else not");
  Expect(wt::Sequential(9, 10, 99, true), "(a) an append-only handle always continues");
  wt::Totals t;
  Expect(wt::ShouldLog(t, 1, 1000) && wt::ShouldLog(t, 3, 1000) && !wt::ShouldLog(t, 4, 1000),
         "(a) the first 3 writes of a handle are logged, not the 4th");
  Expect(wt::ShouldLog(t, 500, 150000000), "(a) a write over 100 ms is logged");
  wt::Totals full;
  for (int n = 0; n < 60; ++n) wt::ShouldLog(full, 1, 0);
  Expect(!wt::ShouldLog(full, 1, 500000000), "(a) at most 60 lines a session");
  wt::Totals c;
  wt::Count(c, true, 17000000, 0);
  wt::Count(c, false, 530000000, 2000000);
  const std::string summary = wt::TakeSummary(c, true);
  Expect(summary == "; drive write calls (fix 148g): 2, 1 not sequential, 1 over 100 ms, pwrite 273.5 ms each, "
                    "copy 1.0 ms each",
         "(a) the 30 s totals: " + summary);
  Expect(c.writes == 0 && wt::TakeSummary(c, false).empty(), "(a) the totals reset, none without writes");
  wt::Count(c, true, 1000000, 0);
  Expect(wt::TakeSummary(c, false).find(", bounce off") != std::string::npos, "(a) bounce off is said");
  // Made-up flag values (the real ones come from fcntl.h).
  Expect(wt::FlagText(0x2 | 0x8, 0x0, 0x1, 0x2, 0x8, 0x80, 0x10000) == "O_RDWR|O_APPEND (0xA)", "(a) O_RDWR|O_APPEND");
  Expect(wt::FlagText(0x1 | 0x80 | 0x10000, 0x0, 0x1, 0x2, 0x8, 0x80, 0x10000) == "O_WRONLY|O_SYNC|O_DIRECT (0x10081)",
         "(a) O_WRONLY|O_SYNC|O_DIRECT");
  Expect(wt::FlagText(0x1, 0x0, 0x1, 0x2, 0x8, 0x80, 0) == "O_WRONLY (0x1)", "(a) no O_DIRECT on a system without it");
  const std::string line = wt::Describe("545408A700000001", 2, 1048576, 1048576, 1048576, 2097152,
                                        "O_WRONLY (0x1)", false, 530000000, 1500000, true);
  Expect(line == "Write (fix 148g): 545408A700000001 write 2 at 1048576, 1048576 bytes, wrote 1048576, file now "
                 "2097152 bytes, O_WRONLY (0x1), NOT sequential, pwrite 530.0 ms, copy from guest memory 1.5 ms",
         "(a) the line: " + line);
  Expect(wt::Describe("f", 1, 0, 1, 1, 1, "O_WRONLY (0x1)", true, 0, 0, false).find("(no bounce)") != std::string::npos,
         "(a) without the switch the line says no bounce");
}

void Tree() {
  const std::string posix = Read(".deps/xenia-canary/src/xenia/base/filesystem_posix.cc");
  const size_t write = posix.find("  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,");
  Expect(write != std::string::npos, "(b) PosixFileHandle::Write is there");
  Expect(posix.find("XBOX360PS5_DATA_ROOT \"/write-bounce.on\"", write) != std::string::npos,
         "(b) the bounce switch file");
  Expect(posix.find("std::memcpy(bounce_buffer.data(), buffer, buffer_length);", write) != std::string::npos &&
             posix.find("pwrite(handle_, source, buffer_length, file_offset)", write) != std::string::npos,
         "(b) with the switch the write goes from the host copy");
  Expect(posix.find("wt::Sequential(writes_, next_offset_, file_offset, append_only_)", write) != std::string::npos &&
             posix.find("next_offset_ = uint64_t(file_offset) + uint64_t(out);", write) != std::string::npos,
         "(b) the next offset per handle");
  Expect(posix.find("fstat(handle_, &now_stat)", write) != std::string::npos &&
             posix.find("fcntl(handle_, F_GETFL)", write) != std::string::npos &&
             posix.find("wt::Describe(", write) != std::string::npos,
         "(b) logged with the file's size and open flags");
  const std::string io = Read("include/xbox360ps5/package_io.hpp");
  Expect(io.find("write_trace::TakeSummary(write_trace::totals,") != std::string::npos,
         "(b) the 30 s file I/O line appends the totals");
}

}  // namespace

int main() {
  Rules();
  if (fs::exists(".deps/xenia-canary")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("write trace: all checks passed\n");
  return 0;
}
