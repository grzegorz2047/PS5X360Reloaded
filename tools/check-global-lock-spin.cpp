// SPDX-License-Identifier: MIT
// Fix 55 (tools/source-1.0.2/fixes/55-gpu-global-lock-spin.patch): the GPU command thread waits for
// the global critical region by spinning on its own core before it sleeps, and shared-memory
// requests for ranges whose pages are all valid are answered without the lock also without
// "Optimized video memory".
//  (a) include/xbox360ps5/global_lock_spin.hpp's Lock on a std::recursive_mutex: a thread that did
//      not named as the spinning one locks plainly and counts nothing; the GPU thread counts each lock, takes
//      a free or its own (recursive) mutex at once, wins a lock released within the spin by
//      spinning, and sleeps (and counts the wait) for one held longer; Line reports the rates
//      and the waiting time from two snapshots, the TSC's rate taken from the same period;
//  (b) the assembled tree: xe_global_mutex::lock (the "Fast locks" option off) goes through it,
//      the command processor's worker names its own thread as the spinning one after taking its core,
//      SharedMemory::RequestRange answers all-valid ranges without the lock whatever
//      gpu_diag::enabled is (and counts them), and the 30-s summary logs the line.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/global_lock_spin.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
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
  return text.str();
}

namespace gl = xbox360ps5::global_lock;

void CheckLock() {
  std::recursive_mutex mutex;
  // Another thread: plain, nothing counted.
  gl::Lock(mutex);
  mutex.unlock();
  Expect(gl::Take().taken == 0, "a thread that did not ask to spin is not counted");

  gl::SpinOnThisThread();
  gl::Lock(mutex);
  gl::Lock(mutex);  // recursive: its own mutex at once
  mutex.unlock();
  mutex.unlock();
  auto now = gl::Take();
  Expect(now.taken == 2 && now.contended == 0, "a free and an own mutex: taken at once");

  // Held for 100 ms: the spin runs out, the thread sleeps, the wait is counted.
  {
    std::atomic<bool> held{false};
    std::thread holder([&] {
      mutex.lock();
      held = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      mutex.unlock();
    });
    while (!held) std::this_thread::yield();
    const auto before = gl::Take();
    gl::Lock(mutex);
    mutex.unlock();
    holder.join();
    const auto after = gl::Take();
    Expect(after.contended - before.contended == 1 && after.slept - before.slept == 1 &&
               after.won_spinning == before.won_spinning && after.wait_ticks > before.wait_ticks,
           "held long: contended, slept, the wait counted");
  }
  // Held for a few microseconds: won while spinning (at least once in 50 tries; the holder may
  // also let go before the first try).
  {
    const auto before = gl::Take();
    for (int attempt = 0; attempt < 50 && gl::Take().won_spinning == before.won_spinning; ++attempt) {
      std::atomic<bool> held{false};
      std::thread holder([&] {
        mutex.lock();
        held = true;
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(3);
        while (std::chrono::steady_clock::now() < until) {
        }
        mutex.unlock();
      });
      while (!held) {
      }
      gl::Lock(mutex);
      mutex.unlock();
      holder.join();
    }
    const auto after = gl::Take();
    std::printf("   short holds: %llu contended, %llu won spinning, %llu slept\n",
                (unsigned long long)(after.contended - before.contended),
                (unsigned long long)(after.won_spinning - before.won_spinning),
                (unsigned long long)(after.slept - before.slept));
    Expect(after.won_spinning > before.won_spinning, "a lock let go within the spin is won spinning");
  }
  gl::SpinOnNoThread();
}

void CheckLine() {
  gl::Snapshot a, b;
  a.tsc = 1000;
  b.tsc = 1000 + 2'000'000'000ull * 2;  // 2 s at 2 GHz
  b.taken = 600000;
  b.contended = 20000;
  b.won_spinning = 18000;
  b.slept = 2000;
  b.wait_ticks = 2'000'000'000ull / 10;  // 100 ms at 2 GHz
  b.lock_free_requests = 400000;
  const std::string line = gl::Line(a, b, 2.0);
  std::printf("   %s\n", line.c_str());
  Expect(line == "Performance: global lock (GPU thread) 300000/s taken, 10000/s contended: 9000/s won spinning, "
                 "1000/s slept, 50.0 ms/s waiting; 200000/s shared-memory requests answered without it",
         "the summary line");
}

void CheckTree() {
  const std::string mutex_text = Read(".deps/xenia-canary/src/xenia/base/mutex.cc");
  // mutex.cc defines xe_global_mutex once a platform; the PS5 one is the one that spins.
  const size_t spin = mutex_text.find("xbox360ps5::global_lock::Lock(blocking_");  // fix 88 adds the caller
  const size_t lock = spin == std::string::npos ? std::string::npos : mutex_text.rfind("void xe_global_mutex::lock() {", spin);
  const size_t fast_off = lock == std::string::npos ? std::string::npos
                                                    : mutex_text.find("if (!ps5_spin_mutexes.load(std::memory_order_relaxed)) {", lock);
  const size_t next = lock == std::string::npos ? std::string::npos : mutex_text.find("void xe_global_mutex::lock_slow() {", lock);
  Expect(spin != std::string::npos && lock != std::string::npos && fast_off < spin && next != std::string::npos && spin < next,
         "the PS5 xe_global_mutex::lock waits through global_lock::Lock when Fast locks is off");
  size_t uses = 0;
  for (size_t at = mutex_text.find("global_lock::Lock("); at != std::string::npos; at = mutex_text.find("global_lock::Lock(", at + 1))
    ++uses;
  Expect(uses == 1, "only the global mutex spins (not the other mutexes)");

  const std::string processor = Read(".deps/xenia-canary/src/xenia/gpu/command_processor.cc");
  const size_t worker = processor.find("void CommandProcessor::WorkerThreadMain() {");
  const size_t place = processor.find("xbox360ps5::PlaceCallingThread(true);", worker);
  const size_t here = processor.find("xbox360ps5::global_lock::SpinOnThisThread();", worker);
  const size_t setup = processor.find("if (!SetupContext()) {", worker);
  Expect(worker != std::string::npos && place < here && here < setup && setup != std::string::npos,
         "the GPU worker spins on its own thread, after taking its core");

  const std::string shared = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.cc");
  const size_t request = shared.find("bool SharedMemory::RequestRange(");
  const size_t fast = shared.find("if (xbox360ps5::gpu_diag::lock_free_valid.load(std::memory_order_relaxed) &&", request);
  const size_t counted = shared.find("xbox360ps5::global_lock::Add(xbox360ps5::global_lock::stats.lock_free_requests);", fast);
  const size_t locked = shared.find("auto global_lock = global_critical_region_.Acquire();", request);
  Expect(request != std::string::npos && fast != std::string::npos && counted != std::string::npos && fast < locked &&
             counted < locked,
         "RequestRange answers all-valid ranges before the lock, whatever memory_boost is");
  Expect(shared.find("if (use_request_cache &&\n      xbox360ps5::gpu_diag::lock_free_valid") == std::string::npos &&
             shared.find("if (use_request_cache &&\r\n      xbox360ps5::gpu_diag::lock_free_valid") == std::string::npos,
         "the fast path no longer needs gpu_diag::enabled");

  const std::string main_text = Read("src/canary_game_main.cpp");
  const size_t rendered = main_text.find("XELOGW(\"Performance: rendered output");
  const size_t line = main_text.find("XELOGW(\"{}\", xbox360ps5::global_lock::Line(", rendered);
  Expect(rendered != std::string::npos && line != std::string::npos, "the summary logs the lock line");
}

}  // namespace

int main() {
  CheckLock();
  CheckLine();
  CheckTree();
  if (failures) {
    std::printf("check-global-lock-spin: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-global-lock-spin: ok\n");
  return 0;
}
