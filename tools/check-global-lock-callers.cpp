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

void CheckTree() {
  const std::string mutex = Read(".deps/xenia-canary/src/xenia/base/mutex.cc");
  Expect(mutex.find("xbox360ps5::global_lock::Lock(blocking_, __builtin_return_address(0));  // fix 88: by caller") !=
             std::string::npos,
         "xe_global_mutex::lock passes the code that asked for the lock");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t line = main_code.find("XELOGW(\"{}\", xbox360ps5::global_lock::Line(lock_before, lock_now, lock_seconds));");
  const size_t callers = main_code.find("XELOGW(\"{}\", lock_callers.Line(lock_seconds,", line);
  Expect(line != std::string::npos && callers != std::string::npos, "the summary gives the line after fix 55's");
}

}  // namespace

int main() {
  CheckCallers();
  CheckTree();
  if (failures) {
    std::printf("check-global-lock-callers: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-global-lock-callers: ok\n");
  return 0;
}
