// SPDX-License-Identifier: MIT
// Fix 188 (tools/source-1.0.2/fixes/188-title-free-pages.patch, issue #81): the free pages
// MmQueryStatistics reports to the title. UEFA Champions League 2006-2007 (B39D547B) ended as a
// match loaded: "PhysicalHeap::AllocRange unable to alloc physical memory in parent heap
// (requested 196608 bytes, parent free 32/131072 pages)", then the game's DbgBreakPoint. The old
// answer, total - kernel_pages - used in 32 bits, wraps to about four billion pages once fewer than
// kernel_pages (1 MiB, held back in the answer only) are free, so a game that sizes its memory by
// it keeps allocating until the heap is empty.
//  (a) title_memory_stats.hpp AvailablePages: the old answer where nothing wraps, zero instead of
//      a wrap, never more than the physical heap has free; WrappingAvailablePages keeps 1.0.6's;
//  (b) a game that allocates 192 KiB blocks while the answer says at least 64 KiB is free, on a
//      512 MiB heap laid out as 1.0.6's (16 MiB GPU writeback, the reserved last 64 KiB): with the
//      old answer the check passes again once the answer wraps, the game runs the heap empty and a
//      block fails with less than a block free, as in the report; with the new one it stops in
//      time and nothing fails;
//  (c) Note: the session's first query logged once, a corrected answer logged once;
//  (d) the assembled xboxkrnl_memory.cc: the header, the physical heap's free pages, the new
//      answer stored, the old subtraction gone, the two log lines.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#if !__has_include("xbox360ps5/title_memory_stats.hpp")
int main() {
  std::printf("skip check-title-memory-stats (fix 188 left out of this tree)\n");
  return 0;
}
#else
#include "xbox360ps5/title_memory_stats.hpp"

namespace {
namespace tm = xbox360ps5::title_memory;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

constexpr uint32_t kTotal = 0x20000;   // 512 MiB in 4 KiB pages
constexpr uint32_t kKernel = 0x100;    // 1 MiB, the answer's kernel share
constexpr uint32_t kWriteback = 4096;  // the 16 MiB GPU writeback page in the 16 MiB heap
constexpr uint32_t kTail = 16;         // the physical heap's reserved last 64 KiB

void Helper() {
  // Plenty free: the same answer as before.
  const uint32_t used = kWriteback + 100000;
  const uint32_t free_pages = kTotal - used - kTail;
  Expect(tm::AvailablePages(kTotal, kKernel, used, free_pages) == free_pages - kKernel,
         "never more than the physical heap has free, less the kernel share");
  Expect(tm::WrappingAvailablePages(kTotal, kKernel, used) - tm::AvailablePages(kTotal, kKernel, used, free_pages) ==
             kTail,
         "the reserved last 64 KiB is the only difference while nothing wraps");

  // The report: 32 pages free in the physical heap.
  const uint32_t used_report = kTotal - 32 - kTail;
  const uint32_t old_answer = tm::WrappingAvailablePages(kTotal, kKernel, used_report);
  Expect(old_answer > 0xFFFF0000u, "1.0.6's answer at 32 free pages wraps to about four billion pages");
  Expect(tm::AvailablePages(kTotal, kKernel, used_report, 32) == 0, "32 free pages: the answer is zero");

  // Edges.
  Expect(tm::AvailablePages(kTotal, kKernel, kTotal - kKernel, kKernel) == 0, "used + kernel == total: zero");
  Expect(tm::AvailablePages(kTotal, kKernel, kTotal - kKernel - 1, kKernel + 1) == 1, "one page over the share");
  Expect(tm::AvailablePages(kTotal, kKernel, kTotal, 0) == 0, "everything used: zero");
  Expect(tm::AvailablePages(kTotal, kKernel, 0xFFFFFFFFu, 0) == 0, "a used count past the total: zero");
  Expect(tm::AvailablePages(kTotal, kKernel, 1000, 500) == 500 - kKernel,
         "the physical heap's free pages win over the heaps' count");
  Expect(tm::AvailablePages(kTotal, 0, 1000, kTotal) == kTotal - 1000, "no kernel share: the heaps' count");
}

// A 512 MiB physical heap as 1.0.6 lays it out, and a game that asks before each 192 KiB block
// whether at least 64 KiB is free (the kind of check GlobalMemoryStatus's dwAvailPhys serves).
struct FakeHeap {
  uint32_t used_by_heaps = kWriteback;  // the three physical heaps' pages
  uint32_t Free() const { return kTotal - kTail - used_by_heaps; }
  bool Alloc(uint32_t pages) {
    if (pages > Free()) return false;
    used_by_heaps += pages;
    return true;
  }
};

struct Run {
  uint32_t blocks = 0, failed = 0, free_at_failure = 0, free_at_end = 0;
};
template <typename Answer>
Run Game(Answer answer) {
  FakeHeap heap;
  Run run;
  // The pools of the report's session first (about 470 MiB in 20 calls), then the match's blocks.
  heap.Alloc(470 * 256);
  constexpr uint32_t kBlock = 196608 / 4096;  // 48 pages, the failed request
  constexpr uint32_t kEnough = 65536 / 4096;  // the game's own check: 64 KiB free
  for (int i = 0; i < 100000; ++i) {
    if (answer(heap) < kEnough) break;  // the game stops (evicts, waits) instead
    if (!heap.Alloc(kBlock)) {
      ++run.failed;
      run.free_at_failure = heap.Free();
      break;  // the game's DbgBreakPoint
    }
    ++run.blocks;
  }
  run.free_at_end = heap.Free();
  return run;
}

void Simulation() {
  const Run old_run = Game([](const FakeHeap& heap) {
    return tm::WrappingAvailablePages(kTotal, kKernel, heap.used_by_heaps);
  });
  Expect(old_run.failed == 1, "1.0.6's answer: the game runs the heap empty and an allocation fails");
  Expect(old_run.free_at_failure < 48,
         "1.0.6's answer: the block fails with less than a block free, as in the report (" +
             std::to_string(old_run.free_at_failure) + " pages)");
  const Run new_run = Game([](const FakeHeap& heap) {
    return tm::AvailablePages(kTotal, kKernel, heap.used_by_heaps, heap.Free());
  });
  Expect(new_run.failed == 0, "the new answer: no allocation fails");
  Expect(new_run.free_at_end >= kKernel, "the new answer: the kernel share stays free");
  Expect(new_run.blocks + 1 >= old_run.blocks - kKernel / 48,
         "the new answer: the game still gets all but the kernel share");
  std::printf("simulation: 1.0.6 %u blocks, failed at %u free pages; fix 188 %u blocks, %u pages left\n",
              old_run.blocks, old_run.free_at_failure, new_run.blocks, new_run.free_at_end);
}

void Logging() {
  tm::State state;
  const auto first = tm::Note(1000, 1000 - kTail, state);
  Expect(first.first && !first.corrected, "the first query is logged; the reserved tail alone is no correction");
  const auto second = tm::Note(1000, 1000, state);
  Expect(!second.first && !second.corrected, "a second query logs nothing");
  const auto wrapped = tm::Note(0xFFFFFF30u, 0, state);
  Expect(!wrapped.first && wrapped.corrected, "a wrapped answer is logged");
  const auto again = tm::Note(0xFFFFFF30u, 0, state);
  Expect(!again.corrected, "a corrected answer is logged once");
}

void Assembled() {
  const std::string source = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc");
  Expect(!source.empty(), "xboxkrnl_memory.cc read from the tree's root");
  Expect(source.find("#include \"xbox360ps5/title_memory_stats.hpp\"") != std::string::npos,
         "xboxkrnl_memory.cc includes title_memory_stats.hpp");
  const size_t at = source.find("dword_result_t xeMmQueryStatistics(");
  const size_t end = source.find("dword_result_t MmQueryStatistics_entry(", at);
  Expect(at != std::string::npos && end != std::string::npos, "xeMmQueryStatistics found");
  if (at == std::string::npos || end == std::string::npos) return;
  const std::string body = source.substr(at, end - at);
  Expect(body.find("GetPhysicalHeap()->unreserved_page_count()") != std::string::npos,
         "the physical heap's free pages are read");
  Expect(body.find("xbox360ps5::title_memory::AvailablePages(") != std::string::npos, "the new answer is computed");
  Expect(body.find("stats_ptr->title.available_pages = available_pages;") != std::string::npos,
         "the new answer is stored");
  Expect(body.find("stats_ptr->total_physical_pages - stats_ptr->kernel_pages - used_pages") == std::string::npos,
         "the old wrapping subtraction is gone");
  Expect(body.find("xbox360ps5::title_memory::Note(") != std::string::npos, "each query is noted");
  Expect(body.find("Memory (fix 188): the game asked how much memory is free") != std::string::npos,
         "the first query is logged");
  Expect(body.find("1.0.6 would have answered") != std::string::npos, "a corrected answer is logged");
}

}  // namespace

int main() {
  Helper();
  Simulation();
  Logging();
  Assembled();
  if (failures) {
    std::printf("check-title-memory-stats: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-title-memory-stats: ok\n");
  return 0;
}
#endif
