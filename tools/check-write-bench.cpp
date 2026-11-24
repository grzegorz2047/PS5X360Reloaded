// SPDX-License-Identifier: MIT
// Fix 148i (tools/source-1.0.2/fixes/148i-write-bench.patch): a drive write benchmark the library
// runs once from <data>/write-bench.txt, writing only its own <folder>/write-bench.tmp.
//  (a) write_bench.hpp: Parse (defaults, mib and seconds capped at 2048 and 120, variants known and
//      once each, dir= only under /data/ or /mnt/ without "..", 8 at most); DefaultDirs (<data>
//      first, then the first PS5X360Reloaded folder with saves of each drive); the .ran marker
//      (a finished or twice-tried switch file does not run again until it changes); Describe's line;
//  (b) on this host, for real in a scratch folder: each buffered variant writes what it is asked
//      and removes its file (fadvise skipped where the libc has none, as on the PS5), fsync every
//      32 MiB counted, a time cap of 0 s writes nothing;
//      Launch removes a test file left behind, runs the switch file once, writes the result and
//      the marker, and the next Launch does not run again;
//  (c) the library starts Launch once per start in a thread of its own with the data root;
//  (d) fix 148j: the variants probe (256 MiB at most), mmap (msync every 32 MiB), writev, aio and
//      reopen64 (a new descriptor every 64 MiB), each for real here; pause= (0..600) sleeps before
//      each variant but the first; each line says when its variant started.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/write_bench.hpp"

namespace {
namespace fs = std::filesystem;
namespace wb = xbox360ps5::write_bench;

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
std::string Names(const wb::Config& c) {
  std::string names;
  for (const wb::Variant* v : c.variants) names += std::string(names.empty() ? "" : ",") + v->name;
  return names;
}

void Pure() {
  const wb::Config d = wb::Parse("");
  Expect(d.mib == 2048 && d.seconds == 120 && Names(d) == "probe,1m,8m,32m,sync32,direct,dontneed,mmap,writev,aio,reopen64" &&
             d.dirs.empty() && d.pause == 0,
         "(a) defaults: 2048 MiB, 120 s, all variants, the default folders");
  const wb::Config c = wb::Parse(
      "# test\r\nmib = 5000\nseconds=999\nvariants=sync32, 32m,bogus,sync32\ndir=/mnt/ext1/xbox360/PS5X360Reloaded/\n"
      "dir=/tmp/x\ndir=/mnt/ext1/../etc\ndir=/data/homebrew/PPSA83600\ndir=/data/homebrew/PPSA83600\n");
  Expect(c.mib == 2048 && c.seconds == 120, "(a) mib and seconds capped");
  Expect(Names(c) == "sync32,32m", "(a) known variants, each once, in the file's order");
  Expect(c.dirs == std::vector<std::string>{"/mnt/ext1/xbox360/PS5X360Reloaded", "/data/homebrew/PPSA83600"},
         "(a) dir= only under /data/ or /mnt/, without .., each once");
  Expect(wb::Parse("mib=0\nseconds=-5\n").mib == 1 && wb::Parse("seconds=-5").seconds == 1, "(a) at least 1");
  std::string many;
  for (int i = 0; i < 12; ++i) many += "dir=/data/d" + std::to_string(i) + "\n";
  Expect(wb::Parse(many).dirs.size() == 8, "(a) 8 folders at most");

  const std::set<std::string> present{"/mnt/ext1/PS5X360Reloaded/saves", "/mnt/ext1/xbox360/PS5X360Reloaded/saves",
                                      "/mnt/usb0/homebrew/PS5X360Reloaded/saves", "/mnt/usb3/PS5X360Reloaded"};
  const auto dirs = wb::DefaultDirs("/data/homebrew/PPSA83600", [&](const std::string& p) { return present.count(p) > 0; });
  Expect(dirs == std::vector<std::string>{"/data/homebrew/PPSA83600", "/mnt/ext1/PS5X360Reloaded",
                                          "/mnt/usb0/homebrew/PS5X360Reloaded"},
         "(a) <data>, then each drive's first PS5X360Reloaded folder with saves");
  Expect(wb::Targets(c, "/data/x", [](const std::string&) { return true; }) == c.dirs, "(a) dir= replaces them");

  const wb::Ran fresh = wb::ParseRan("");
  Expect(wb::ShouldRun(fresh, "10:20"), "(a) a switch file never run runs");
  Expect(!wb::ShouldRun(wb::ParseRan("10:20 1 1\n"), "10:20") && wb::ShouldRun(wb::ParseRan("10:20 1 1\n"), "11:20"),
         "(a) a finished one does not run again until it changes");
  Expect(wb::ShouldRun(wb::ParseRan("10:20 1 0"), "10:20") && !wb::ShouldRun(wb::ParseRan("10:20 2 0"), "10:20"),
         "(a) an unfinished one is tried twice");
  Expect(wb::SerializeRan({"10:20", 2, true}) == "10:20 2 1\n", "(a) the marker's form");

  wb::Stats s;
  s.bytes = 64 * wb::kMiB, s.writes = 64, s.stalls = 3, s.seconds = 4, s.longest_write_ms = 931.5;
  s.syncs = 2, s.sync_ms = 1840, s.longest_sync_ms = 925, s.close_ms = 0.2, s.remove_ms = 12.5, s.stopped = "size";
  Expect(wb::Describe("/data/x", *wb::FindVariant("sync32"), s) ==
             "Write bench (fix 148i): /data/x sync32 (1 MiB pwrites, fsync every 32 MiB) at +0 s: 64 MiB in 4.0 s, 16.00 "
             "MiB/s, 64 writes, 3 over 100 ms, longest 931.5 ms; 2 fsyncs, 920.0 ms each, longest 925.0 ms; close "
             "0.2 ms, remove 12.5 ms; stopped by size",
         "(a) Describe's line");
}

void Paths(const fs::path& scratch) {
  const wb::Config c = wb::Parse("pause=90\nvariants=reopen64,probe,mmap\n");
  Expect(c.pause == 90 && Names(c) == "reopen64,probe,mmap", "(d) pause= and the variants' order kept");
  Expect(wb::Parse("pause=5000").pause == 600 && wb::Parse("pause=-3").pause == 0, "(d) pause 0..600");
  const std::string dir = scratch.string();
  const wb::Stats probe = wb::Run(dir, *wb::FindVariant("probe"), 2048, 120);
  Expect(probe.bytes == 256 * wb::kMiB && probe.writes == 256 && probe.stopped == "size",
         "(d) the probe stops at 256 MiB whatever mib= says");
  const wb::Stats probe_small = wb::Run(dir, *wb::FindVariant("probe"), 8, 120);
  Expect(probe_small.bytes == 8 * wb::kMiB, "(d) and at mib= when that is less");
  const wb::Stats map = wb::Run(dir, *wb::FindVariant("mmap"), 70, 120);
  Expect(map.bytes == 70 * wb::kMiB && map.writes == 70 && map.syncs == 2 && map.stopped == "size" &&
             std::string(map.sync_name) == "msyncs" && map.note.rfind("ftruncate and mmap ", 0) == 0 &&
             !fs::exists(wb::TestFile(dir)),
         "(d) mmap: 70 MiB copied, msync every 32 MiB, the file removed: " + map.stopped + " " + map.note);
  const wb::Stats vec = wb::Run(dir, *wb::FindVariant("writev"), 40, 120);
  Expect(vec.bytes == 40 * wb::kMiB && vec.writes == 40 && vec.stopped == "size" && !fs::exists(wb::TestFile(dir)),
         "(d) writev: 40 MiB: " + vec.stopped);
  const wb::Stats aio = wb::Run(dir, *wb::FindVariant("aio"), 40, 120);
  if (XBOX360PS5_WRITE_BENCH_AIO)
    Expect(aio.bytes == 40 * wb::kMiB && aio.writes == 40 && aio.stopped == "size" && !fs::exists(wb::TestFile(dir)),
           "(d) aio: 40 MiB, each write waited for: " + aio.stopped);
  else
    Expect(aio.bytes == 0 && aio.stopped == "skipped: no aio_write in these headers", "(d) aio skipped here");
  const wb::Stats re = wb::Run(dir, *wb::FindVariant("reopen64"), 200, 120);
  Expect(re.bytes == 200 * wb::kMiB && re.reopens == 3 && re.stopped == "size" && !fs::exists(wb::TestFile(dir)),
         "(d) reopen64: 200 MiB with 3 new descriptors: " + re.stopped);
  wb::Stats line;
  line.started = 61.4, line.bytes = 200 * wb::kMiB, line.writes = 200, line.seconds = 2, line.reopens = 3,
  line.reopen_ms = 3, line.stopped = "size";
  Expect(wb::Describe("/d", *wb::FindVariant("reopen64"), line).find(
             "reopen64 (1 MiB pwrites, the file opened anew every 64 MiB) at +61 s: 200 MiB in 2.0 s, 100.00 MiB/s, "
             "200 writes, 0 over 100 ms, longest 0.0 ms; 3 reopens, 1.0 ms each; close") != std::string::npos,
         "(d) the line: start time and reopens");

  for (const char* f : {"write-bench.txt", "write-bench.ran", "write-bench-result.txt"}) fs::remove(scratch / f);
  { std::ofstream(wb::SwitchFile(dir)) << "mib=2\nvariants=probe,writev\npause=1\n"; }
  std::vector<std::string> lines;
  const auto t = wb::Clock::now();
  wb::Launch(dir, [&](const std::string& l) { lines.push_back(l); });
  Expect(wb::MsSince(t) >= 1000 && lines.size() == 4 && lines[0].find("pause 1 s between") != std::string::npos &&
             lines[2].find(" writev (") != std::string::npos && lines[2].find(" at +1 s: ") != std::string::npos,
         "(d) pause=1 sleeps a second between the two variants, and the second line says +1 s");
}

void Real(const fs::path& scratch) {
  const std::string dir = scratch.string();
  for (const char* name : {"1m", "8m", "32m", "dontneed"}) {
    const wb::Stats s = wb::Run(dir, *wb::FindVariant(name), 40, 120);
    if (!XBOX360PS5_WRITE_BENCH_FADVISE && std::string(name) == "dontneed") {
      Expect(s.bytes == 0 && s.stopped == "skipped: no posix_fadvise in this libc", "(b) dontneed skipped here");
      continue;
    }
    Expect(s.bytes == 40 * wb::kMiB && s.stopped == "size" && !fs::exists(wb::TestFile(dir)),
           std::string("(b) ") + name + " writes 40 MiB and removes its file: " + s.stopped);
  }
  const wb::Stats e = wb::Run(dir, *wb::FindVariant("8m"), 3, 120);
  Expect(e.bytes == 3 * wb::kMiB && e.writes == 1, "(b) the last write is what is left");
  const wb::Stats sync = wb::Run(dir, *wb::FindVariant("sync32"), 70, 120);
  Expect(sync.bytes == 70 * wb::kMiB && sync.writes == 70 && sync.syncs == 2, "(b) fsync every 32 MiB");
  const wb::Stats none = wb::Run(dir, *wb::FindVariant("1m"), 8, 0);
  Expect(none.bytes == 0 && none.stopped == "time" && !fs::exists(wb::TestFile(dir)), "(b) the time cap");
  const wb::Stats bad = wb::Run((scratch / "missing").string(), *wb::FindVariant("1m"), 1, 1);
  Expect(bad.stopped.rfind("open failed, errno", 0) == 0, "(b) a folder that is not there: " + bad.stopped);

  { std::ofstream(wb::TestFile(dir)) << "left"; }
  std::vector<std::string> lines;
  wb::Launch(dir, [&](const std::string& line) { lines.push_back(line); });
  Expect(!fs::exists(wb::TestFile(dir)) && lines.size() == 1 && lines[0].find("left behind (4 bytes) removed") !=
                                                                    std::string::npos,
         "(b) without the switch file: only the file left behind is removed");
  { std::ofstream(wb::SwitchFile(dir)) << "mib=4\nvariants=1m,sync32\n"; }
  lines.clear();
  wb::Launch(dir, [&](const std::string& line) { lines.push_back(line); });
  const std::string result = Read(wb::ResultFile(dir));
  Expect(lines.size() == 4 && lines.back() == "Write bench (fix 148i): done" &&
             result.find(" 1m (1 MiB pwrites) at +0 s: 4 MiB in ") != std::string::npos &&
             result.find(" sync32 (") != std::string::npos && !fs::exists(wb::TestFile(dir)),
         "(b) the switch file runs each variant in <data>, logs it and writes the result");
  Expect(wb::ParseRan(Read(wb::RanFile(dir))).done && wb::ParseRan(Read(wb::RanFile(dir))).attempts == 1,
         "(b) the marker says it finished");
  lines.clear();
  wb::Launch(dir, [&](const std::string& line) { lines.push_back(line); });
  Expect(lines.size() == 1 && lines[0].find("already ran") != std::string::npos, "(b) the next start does not run again");
}

void Tree() {
  const std::string launcher = Read("src/launcher.cpp");
  const size_t at = launcher.find("if (!g_write_bench_started) {");
  Expect(at != std::string::npos && launcher.find("g_write_bench_started = true;", at) != std::string::npos &&
             launcher.find("xbox360ps5::write_bench::Launch(XBOX360PS5_DATA_ROOT,", at) != std::string::npos &&
             launcher.find(".detach();", at) != std::string::npos,
         "(c) the library starts the benchmark once per start in a thread of its own");
}

}  // namespace

int main() {
  Pure();
  const fs::path scratch = fs::temp_directory_path() / "check-write-bench";
  std::error_code error;
  fs::remove_all(scratch, error);
  fs::create_directories(scratch);
  Real(scratch);
  Paths(scratch);
  fs::remove_all(scratch, error);
  if (fs::exists("src/launcher.cpp")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("write bench: all checks passed\n");
  return 0;
}
