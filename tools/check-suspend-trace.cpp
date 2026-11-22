// SPDX-License-Identifier: MIT
// Fix 69 (tools/source-1.0.2/fixes/69-suspend-trace.patch): a diagnostic for lost wake-ups of
// threads that suspend themselves (Ninja Gaiden II on 1.0.4_candidate_26: two job threads
// suspended by their own NtSuspendThread, the main thread spinning on a flag only they clear).
//  (a) include/xbox360ps5/suspend_trace.hpp: a thread's ring keeps its last 16 changes, newest
//      first, with who, the count before and after, and how long ago; an early resume (count 0)
//      is written at once for the first 20, then at most one line every 30 s with the count since;
//  (b) the assembled tree: XThread::Resume notes every resume and reports one that finds a count
//      of 0 (with whether the target was entering its own suspension), Suspend notes the thread
//      that suspends it, SelfSuspend marks its entry and notes the suspension and the wake-up;
//      the STALL report prints the history of every thread that has one.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/suspend_trace.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
namespace st = xbox360ps5::suspend_trace;

void CheckRing() {
  st::Ring ring;
  Expect(ring.Line(1000) == "none" && ring.count() == 0, "an empty ring");
  ring.Note(st::kSuspendedItself, 0, 0, 1000);
  ring.Note(st::kResumedBy, 0xF8000008, 1, 1500);
  ring.Note(st::kWokeItself, 0, 0, 1501);
  ring.Note(st::kResumeFoundRunning, 0xF8000008, 0, 1600);
  ring.Note(st::kSuspendedItself, 0, 0, 1610);
  const std::string line = ring.Line(2000);
  std::printf("   %s\n", line.c_str());
  Expect(line ==
             "-390 ms suspended itself (0->1); -400 ms EARLY resume (count 0) by F8000008 (0->0); -499 ms woke; "
             "-500 ms resumed by F8000008 (1->0); -1000 ms suspended itself (0->1)",
         "newest first, who, the counts, how long ago");
  for (int n = 0; n < 40; ++n) ring.Note(st::kSuspendedBy, 0xF8000010, uint8_t(n % 3), 3000 + n);
  const std::string full = ring.Line(3100);
  size_t items = 1;
  for (size_t at = full.find("; "); at != std::string::npos; at = full.find("; ", at + 1)) ++items;
  Expect(items == st::Ring::kEvents && full.rfind("-61 ms suspended by F8000010", 0) == 0,
         "the last 16 kept, the newest first");
}

void CheckEarly() {
  std::string lines;
  int written = 0;
  for (uint64_t n = 0; n < 25; ++n) {
    const std::string line = st::EarlyResumeLine(0xF8000008, 0xF8000164, n == 3, 10000 + n);
    if (!line.empty()) {
      ++written;
      if (n == 0 || n == 3) lines += line + "\n";
    }
  }
  std::printf("%s", lines.c_str());
  Expect(written == 20, "the first 20 written at once");
  Expect(lines.find("Suspend: EARLY resume #1: F8000008 resumed F8000164 while its suspend count was 0 (it was running)") !=
                 std::string::npos &&
             lines.find("#4: F8000008 resumed F8000164 while its suspend count was 0 (it was entering its own suspension)") !=
                 std::string::npos,
         "resumer, target and the target's state");
  Expect(st::EarlyResumeLine(1, 2, false, 20000).empty(), "then none within 30 s");
  const std::string later = st::EarlyResumeLine(1, 2, false, 40020);
  std::printf("   %s\n", later.c_str());
  Expect(later == "Suspend: 7 more early resumes (count 0) in 30 s, 27 in all", "then one line per 30 s");
  Expect(st::early_resumes.load() == 27, "all counted");
}

void CheckTree() {
  const std::string k = ".deps/xenia-canary/src/xenia/kernel/";
  const std::string code = Read(k + "xthread.cc");
  const size_t resume = code.find("X_STATUS XThread::Resume(uint32_t* out_suspend_count) {");
  const size_t found = code.find("if (previous == 0) {", resume);
  const size_t early = code.find("xbox360ps5::suspend_trace::EarlyResumeLine(", found);
  const size_t entering = code.find("entering_self_suspend_.load(std::memory_order_relaxed), now);", early);
  const size_t resumed = code.find("suspend_trace_.Note(xbox360ps5::suspend_trace::kResumedBy, resumer, previous, now);", found);
  const size_t wake = code.find("thread_->Resume(&unused_host_suspend_count);", resume);
  Expect(resume != std::string::npos && found != std::string::npos && early != std::string::npos &&
             entering != std::string::npos && resumed != std::string::npos && resumed < wake,
         "every resume noted; one that finds no suspension reported with the target's state");
  const size_t suspend = code.find("X_STATUS XThread::Suspend(uint32_t* out_suspend_count) {");
  Expect(code.find("suspend_trace_.Note(xbox360ps5::suspend_trace::kSuspendedBy,", suspend) != std::string::npos,
         "a suspension by another thread noted");
  const size_t self = code.find("uint32_t XThread::SelfSuspend() {");
  const size_t mark = code.find("entering_self_suspend_.store(true, std::memory_order_relaxed);", self);
  const size_t add = code.find("const uint32_t previous = suspend_count.fetch_add(1);", self);
  const size_t unmark = code.find("entering_self_suspend_.store(false, std::memory_order_relaxed);", add);
  const size_t noted = code.find("suspend_trace_.Note(xbox360ps5::suspend_trace::kSuspendedItself,", add);
  const size_t wait = code.find("suspend_cv_.wait(lock,", self);
  const size_t woke = code.find("suspend_trace_.Note(xbox360ps5::suspend_trace::kWokeItself,", wait);
  Expect(self != std::string::npos && mark < add && add < unmark && unmark < noted && noted < wait &&
             woke != std::string::npos,
         "SelfSuspend marks its entry around the count and notes the suspension and the wake-up");
  Expect(Read(k + "xthread.h").find("xbox360ps5::suspend_trace::Ring suspend_trace_;") != std::string::npos,
         "each thread has its ring");
  const std::string main_code = Read("src/canary_game_main.cpp");
  Expect(main_code.find("XELOGW(\"STALL thread {:08X} suspend history (newest first): {}\", thread->handle(),") !=
             std::string::npos,
         "the STALL report prints the history");
}

}  // namespace

int main() {
  CheckRing();
  CheckEarly();
  CheckTree();
  if (failures) {
    std::printf("check-suspend-trace: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-suspend-trace: ok\n");
  return 0;
}
