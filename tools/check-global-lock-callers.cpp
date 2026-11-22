// SPDX-License-Identifier: MIT
// Fix 88 (tools/source-1.0.2/fixes/88-global-lock-callers.patch): which code the GPU command thread
// takes the global critical region for. Modern Warfare 2 on the 1.0.5 perf builds with fix 87:
// 102,775 locks a second and 91.8 ms/s waiting, after draws (fix 86) and arming (fix 84) stopped
// taking it per call; the known callers explain only part of it.
//  (a) include/xbox360ps5/global_lock_spin.hpp: one in kCallerSampleEvery of the GPU thread's locks
//      is counted for its caller, with whether it waited and for how long; other threads are not
//      counted; a full table counts the samples it dropped; CallerReporter gives the callers that
//      took it most, scaled back, since its previous line, with the anchor's address;
//  (b) the assembled tree: xe_global_mutex::lock passes its return address, and the summary gives
//      the line after fix 55's.
// Fix 128 (tools/source-1.0.2/fixes/128-global-lock-holds.patch), next to fix 88's table: every
// thread's hold of the region that lasts 1 ms or more, by caller and thread. Kinect Sports
// (4D5308C9) on 1.0.5-work-fix119: the GPU thread waited 643-662 ms/s for the lock, 216 sleeps a
// second of about 3 ms, and fix 88's sample names only where it took the lock.
//  (c) the holder: a short hold is not counted, a long one is, a recursive hold is timed once;
//      the table by caller and thread (count, time, longest) and the line;
//  (d) a real mutex held by four threads, one for 3 ms each time: only that one shows;
//  (e) the assembled tree: the PS5 mutex times the outermost hold in lock, try_lock and unlock in
//      both modes (fix 88's call as it was), and the summary logs the line with the threads' names.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/global_lock_spin.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
namespace gl = xbox360ps5::global_lock;
const void* const kA = reinterpret_cast<const void*>(uintptr_t(0x1000A0));
const void* const kB = reinterpret_cast<const void*>(uintptr_t(0x1000B0));
const void* const kC = reinterpret_cast<const void*>(uintptr_t(0x1000C0));

void CheckCallers() {
  std::recursive_mutex mutex;
  gl::CallerReporter reporter;
  (void)reporter.Line(1, 1);
  // Not the spinning thread: nothing counted.
  for (int n = 0; n < 640; ++n) {
    gl::Lock(mutex, kA);
    mutex.unlock();
  }
  Expect(gl::callers[0].taken == 0 && gl::caller_sample_counter == 0, "other threads are not counted");
  gl::SpinOnThisThread();
  for (int n = 0; n < 6400; ++n) {
    gl::Lock(mutex, kA);
    mutex.unlock();
  }
  for (int n = 0; n < 1280; ++n) {
    gl::Lock(mutex, kB);
    mutex.unlock();
  }
  // A lock held by another thread for 2 ms, sampled: counted as contended, with its wait.
  gl::caller_sample_counter = gl::kCallerSampleEvery - 1;
  std::atomic<bool> holding{false};
  std::thread holder([&mutex, &holding] {
    std::lock_guard<std::recursive_mutex> held(mutex);
    holding = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
    while (std::chrono::steady_clock::now() < until) {
    }  // held for 3 ms without sleeping (a sleep may last far longer on some hosts)
  });
  while (!holding) {
  }
  gl::Lock(mutex, kC);
  mutex.unlock();
  holder.join();
  gl::Lock(mutex, nullptr);  // no caller given: counted in the totals only
  mutex.unlock();
  const uint64_t start = __rdtsc();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const double ticks_per_ms = double(__rdtsc() - start) / 20.0;
  const std::string line = reporter.Line(1, ticks_per_ms, 0x1234);
  std::printf("   %s\n", line.c_str());
  Expect(line.find("Performance: global lock callers (GPU thread, 1 in 64 sampled and scaled): 0x1000a0 6400/s taken, "
                   "0/s contended, 0.0 ms/s waiting; 0x1000b0 1280/s taken") == 0,
         "the callers, most first, scaled back");
  const size_t c = line.find("0x1000c0 64/s taken, 64/s contended, ");
  Expect(c != std::string::npos, "a sampled lock that waited is counted contended");
  if (c != std::string::npos) {
    const double waited = std::atof(line.c_str() + line.find("contended, ", c) + 11) / 64;
    Expect(waited > 0.2 && waited < 30, "with its wait, up to about 3 ms (" + std::to_string(waited) + " ms)");
  }
  Expect(line.find("0 samples without a slot; anchor 0x1234") != std::string::npos, "the dropped count and the anchor");
  Expect(reporter.Line(1, ticks_per_ms, 0x1234) ==
             "Performance: global lock callers (GPU thread, 1 in 64 sampled and scaled): 0 samples without a slot; anchor 0x1234",
         "only since the previous line");
  // A full table drops further callers and counts them.
  for (uint32_t n = 0; n < gl::kCallerSlots + 10; ++n) (void)gl::SlotFor(0x200000 + n * 16);
  Expect(gl::callers_dropped >= 10, "samples without a slot counted (" + std::to_string(gl::callers_dropped) + ")");
  gl::SpinOnNoThread();
}

namespace holds = xbox360ps5::global_lock_holds;
void ResetHolds() {
  for (auto& slot : holds::slots) {
    slot.caller = 0;
    slot.thread = 0;
    slot.holds = 0;
    slot.ticks = 0;
    slot.max_ticks = 0;
  }
  holds::dropped = 0;
  holds::long_holds = 0;
  holds::long_hold_ticks = 0;
}
uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void CheckHolds() {
  ResetHolds();
  holds::SetTicksPerMs(1000.0);  // a "tick" of 1 us below
  holds::Holder holder;
  holds::Acquired(holder, 7, 0x1000, 100);
  holds::Releasing(holder, 600);  // 0.5 ms
  Expect(holds::long_holds.load() == 0, "a hold under 1 ms is not counted");
  holds::Acquired(holder, 7, 0x1000, 1000);
  holds::Acquired(holder, 7, 0x2000, 1500);  // recursive: the same hold
  holds::Releasing(holder, 2000);
  Expect(holds::long_holds.load() == 0, "an inner release does not end the hold");
  holds::Acquired(holder, 7, 0x2000, 2500);
  holds::Releasing(holder, 3000);
  holds::Releasing(holder, 4000);  // the outermost: 3 ms
  Expect(holds::long_holds.load() == 1 && holds::long_hold_ticks.load() == 3000,
         "the outermost hold is timed once, 3 ms");
  holds::Acquired(holder, 9, 0x1000, 10000);
  holds::Releasing(holder, 15000);  // 5 ms, another thread, the same caller
  holds::Acquired(holder, 7, 0x1000, 20000);
  holds::Releasing(holder, 21500);  // 1.5 ms
  uint32_t rows = 0;
  uint64_t seven_holds = 0, seven_max = 0;
  for (const auto& slot : holds::slots) {
    if (!slot.caller.load()) continue;
    ++rows;
    if (slot.thread.load() == 7) {
      Expect(slot.caller.load() == 0x1000, "counted for the outermost caller");
      seven_holds = slot.holds.load();
      seven_max = slot.max_ticks.load();
    }
  }
  Expect(rows == 2 && seven_holds == 2 && seven_max == 3000, "a row per caller and thread, with the longest");

  holds::Reporter reporter;
  const std::string line = reporter.Line(2.0, 1000.0, 0x40, [](uint64_t id) {
    return id == 7 ? std::string("Main XThread (F8000008)") : std::string("XThread0000 (F8000078)");
  });
  std::printf("  %s\n", line.c_str());
  Expect(line.find("Performance: global lock holds of 1 ms or more (every thread): 1.5/s, 4.8 ms/s held;") == 0,
         "the holds line's head: holds and time per second");
  Expect(line.find("0x1000 'XThread0000 (F8000078)' 0.5/s, 2.5 ms/s, longest 5.0 ms") != std::string::npos &&
             line.find("0x1000 'Main XThread (F8000008)' 1.0/s, 2.2 ms/s, longest 3.0 ms") != std::string::npos,
         "each row names its thread, the time and the longest");
  Expect(line.find("0x1000 'XThread0000") < line.find("0x1000 'Main XThread"), "the longest held first");
  Expect(line.find("0 without a slot; anchor 0x40") != std::string::npos, "the holds line's tail");
  const std::string again = reporter.Line(2.0, 1000.0, 0x40, nullptr);
  Expect(again.find("0.0/s, 0.0 ms/s held; by caller and thread: 0 without a slot") != std::string::npos,
         "the next holds line counts only what came since");

  // Four threads on a real mutex, one holding it 3 ms each time.
  ResetHolds();
  holds::SetTicksPerMs(1e6);  // nanoseconds
  std::mutex mutex;
  holds::Holder shared;
  const auto hold = [&](uint64_t self, uintptr_t caller, int us, int times) {
    for (int n = 0; n < times; ++n) {
      mutex.lock();
      holds::Acquired(shared, self, caller, SteadyNs());
      const uint64_t until = SteadyNs() + uint64_t(us) * 1000;
      while (SteadyNs() < until) {
      }
      holds::Releasing(shared, SteadyNs());
      mutex.unlock();
      std::this_thread::yield();
    }
  };
  std::vector<std::thread> threads;
  threads.emplace_back(hold, 1, 0xA0, 3000, 20);
  for (uint64_t t = 2; t <= 4; ++t) threads.emplace_back(hold, t, 0xB0 + t, 20, 2000);
  for (auto& thread : threads) thread.join();
  uint64_t long_rows = 0, long_caller = 0, long_count = 0;
  for (const auto& slot : holds::slots) {
    if (!slot.caller.load() || !slot.holds.load()) continue;
    ++long_rows;
    if (slot.thread.load() == 1) {
      long_caller = slot.caller.load();
      long_count = slot.holds.load();
    }
  }
  Expect(long_caller == 0xA0 && long_count == 20, "the long holder shows with each of its holds");
  Expect(long_rows <= 2, "brief holds stay out (at most one preempted one)");
}

void CheckTree() {
  const std::string mutex = Read(".deps/xenia-canary/src/xenia/base/mutex.cc");
  Expect(mutex.find("xbox360ps5::global_lock::Lock(blocking_, __builtin_return_address(0));  // fix 88: by caller") !=
             std::string::npos,
         "xe_global_mutex::lock passes the code that asked for the lock");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t line = main_code.find("XELOGW(\"{}\", xbox360ps5::global_lock::Line(lock_before, lock_now, lock_seconds));");
  const size_t callers = main_code.find("XELOGW(\"{}\", lock_callers.Line(lock_seconds,", line);
  Expect(line != std::string::npos && callers != std::string::npos, "the summary gives the line after fix 55's");

  // Fix 128.
  const std::string mutex_h = Read(".deps/xenia-canary/src/xenia/base/mutex.h");
  Expect(mutex_h.find("#include \"xbox360ps5/global_lock_spin.hpp\"  // fix 128: the holds") != std::string::npos &&
             mutex_h.find("xbox360ps5::global_lock_holds::Holder holds_;") != std::string::npos,
         "the PS5 global mutex keeps its current hold");
  const size_t ps5 = mutex.find("std::atomic<bool> ps5_spin_mutexes{false};");
  const size_t lock = mutex.find("void xe_global_mutex::lock() {", ps5);
  const size_t lock_slow = mutex.find("void xe_global_mutex::lock_slow() {", lock);
  const size_t unlock = mutex.find("void xe_global_mutex::unlock() {", ps5);
  const size_t try_lock = mutex.find("bool xe_global_mutex::try_lock() {", ps5);
  const size_t try_end = mutex.find("void xe_fast_mutex::lock() {", try_lock);
  const auto count = [&](size_t from, size_t to, const std::string& what) {
    size_t n = 0;
    for (size_t at = mutex.find(what, from); at != std::string::npos && at < to; at = mutex.find(what, at + 1)) ++n;
    return n;
  };
  Expect(ps5 != std::string::npos && lock != std::string::npos && lock_slow != std::string::npos &&
             count(lock, lock_slow, "global_lock_holds::Acquired(") == 4,
         "lock() times the hold each way it takes the lock, in both modes");
  Expect(unlock != std::string::npos &&
             mutex.find("xbox360ps5::global_lock_holds::Releasing(holds_, __rdtsc());", unlock) <
                 mutex.find("blocking_.unlock();", unlock),
         "unlock() ends the hold before letting go");
  Expect(try_lock != std::string::npos && try_end != std::string::npos &&
             count(try_lock, try_end, "global_lock_holds::Acquired(") == 3,
         "try_lock() times each way it wins the lock");
  Expect(main_code.find("static xbox360ps5::global_lock_holds::Reporter lock_holds;") > callers &&
             main_code.find("thread->thread_name();") != std::string::npos,
         "the summary logs the holders after fix 88's line, with their threads' names");
}

}  // namespace

int main() {
  CheckCallers();
  CheckHolds();  // fix 128
  CheckTree();
  if (failures) {
    std::printf("check-global-lock-callers: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-global-lock-callers: ok\n");
  return 0;
}
