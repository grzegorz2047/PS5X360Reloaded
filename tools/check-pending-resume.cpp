// SPDX-License-Identifier: MIT
// Fix 71 (tools/source-1.0.2/fixes/71-pending-resume.patch): a resume that reaches a thread just
// before it suspends itself is kept for that suspension (Ninja Gaiden II's black screen on
// 1.0.4_candidate_26: two job threads suspended by their own NtSuspendThread, never woken).
//  (a) Credit (include/xbox360ps5/pending_resume.hpp): taken once, within 10 ms of the resume;
//  (b) a stress test of XThread's SelfSuspend and Resume as the console runs them (an atomic count
//      raised without the lock, a condition variable under suspend_mutex_, a resume that finds 0
//      doing nothing) with the job pattern that hung: a worker says it is idle and suspends
//      itself, a master that sees it idle resumes it. Without the credit the pair hangs within
//      a second (a resume lands between the two; reproduced, not assumed); with it, the same
//      loop runs for a second without a hang, and a resume long before a suspension still lets
//      the thread sleep;
//  (c) the assembled tree: XThread::Resume notes the credit where it finds a count of 0, and
//      SelfSuspend takes it before raising the count and returns at once, noted in the trace.
// Fix 151 (tools/source-1.0.2/fixes/151-suspend-credit-underflow.patch): the one-byte suspend count
// never wraps (Virtua Tennis 3's replay freeze, issue #61: a job thread slept with a count of 175).
//  (d) Raise and Lower (include/xbox360ps5/suspend_count.hpp) stop at 255 and at 0;
//  (e) the interleaving that wrapped the count, made deterministic: resume A finds 0 (a credit),
//      the worker raises its count, resume B finds 1 and lowers it, the worker takes the credit.
//      Fix 71's code ends at 255 (reproduced); fix 151's at 0, with and without B's clearing of the
//      credit, and a resume C that leaves a new credit after B still does not wrap it;
//  (f) a burst stress test: a master that resumes in bursts of 1-20 (as Virtua Tennis 3's main
//      thread does) a worker that suspends itself between jobs. Fix 71's code sees counts above 1;
//      fix 151's never does and runs for a second without a hang;
//  (g) the trace: a cancelled suspension notes its true count (1->0), a dropped credit (0->0), and a
//      suspension refused at the limit gets a line;
//  (h) the assembled tree: every change to the count goes through Raise and Lower, the credit path
//      lowers through Lower, and a resume that ends the thread's own suspension clears the credit.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/pending_resume.hpp"
#include "xbox360ps5/suspend_count.hpp"
#include "xbox360ps5/suspend_trace.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <random>
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
namespace pr = xbox360ps5::pending_resume;
namespace sc = xbox360ps5::suspend_count;
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void CheckCredit() {
  pr::Credit credit;
  Expect(!credit.Take(1000), "no credit without a resume");
  credit.Note(1000);
  Expect(credit.Take(1000 + pr::kWindowNs), "taken within the window");
  Expect(!credit.Take(1000 + pr::kWindowNs), "taken once");
  credit.Note(5000);
  Expect(!credit.Take(5000 + pr::kWindowNs + 1), "lapsed after the window");
  Expect(!credit.Take(5000 + 10), "a lapsed credit is gone");
  credit.Note(9000);
  credit.Clear();
  Expect(!credit.Take(9000 + 10), "a cleared credit is gone (fix 151)");
}

void CheckCount() {
  std::atomic<uint8_t> count{0};
  Expect(sc::Lower(count) == 0 && count.load() == 0, "Lower stops at 0");
  Expect(sc::Raise(count) == 0 && count.load() == 1, "Raise from 0 to 1");
  Expect(sc::Lower(count) == 1 && count.load() == 0, "Lower from 1 to 0");
  count.store(254);
  Expect(sc::Raise(count) == 254 && count.load() == 255, "Raise from 254 to 255");
  Expect(sc::Raise(count) == 255 && count.load() == 255, "Raise stops at 255");
  Expect(sc::Lower(count) == 255 && count.load() == 254, "Lower from 255");
}

// Fix 151's trace: a cancelled suspension shows its true count (1->0), a dropped credit (0->0),
// and a refused suspension at the limit gets a line (the first kRawLines).
void CheckTrace() {
  namespace st = xbox360ps5::suspend_trace;
  st::Ring ring;
  ring.Note(st::kSuspendCancelled, 0, 1, 1000);
  ring.Note(st::kCreditDropped, 0, 0, 1001);
  const std::string line = ring.Line(1002);
  Expect(line ==
             "-1 ms early resume's credit dropped, a resume had ended the suspension (0->0); "
             "-2 ms suspension cancelled by the early resume (1->0)",
         "the trace shows the true counts: " + line);
  const std::string limit = st::CountLimitLine(0xF8000024, "suspension by itself");
  Expect(limit == "Suspend: suspension by itself of F8000024 refused: its suspend count is at its limit, 255 "
                  "(fix 151, #1)",
         "the limit line: " + limit);
  for (uint64_t n = 2; n <= st::kRawLines; ++n) st::CountLimitLine(1, "x");
  Expect(st::CountLimitLine(1, "x").empty(), "then no more limit lines");
}

// XThread's SelfSuspend and Resume (xthread.cc, !XE_PLATFORM_WIN32), for one guest thread.
// fix151 = false is fix 71's code as 1.0.5 has it; true is the code after fix 151.
struct ModelThread {
  bool use_credit = false;
  bool fix151 = true;
  bool clear_on_resume = true;  // fix 151's clearing of the credit (off: only Lower's guard)
  std::atomic<uint8_t> count{0};
  std::mutex suspend_mutex;
  std::condition_variable suspend_cv;
  pr::Credit credit;
  std::atomic<bool> stop{false};
  std::atomic<bool> in_self_suspend{false};
  std::function<void()> after_raise;  // test hook: runs after the count is raised, without the lock

  uint8_t SelfSuspend() {
    uint8_t previous;
    if (fix151) {
      previous = sc::Raise(count);  // raised without the lock, as XThread does
      if (previous == sc::kMax) return previous;
      in_self_suspend.store(true);
    } else {
      previous = count.fetch_add(1);
    }
    if (after_raise) after_raise();
    std::unique_lock<std::mutex> lock(suspend_mutex);
    // Fix 71: taken after raising the count, under the lock Resume reads the count under.
    if (use_credit && credit.Take(NowNs())) {
      if (fix151) {
        sc::Lower(count);
        in_self_suspend.store(false);
      } else {
        count.fetch_sub(1);
      }
      return previous;
    }
    suspend_cv.wait(lock, [&] { return count.load() == 0 || stop.load(); });
    in_self_suspend.store(false);
    return previous;
  }
  uint8_t Resume() {
    std::lock_guard<std::mutex> lock(suspend_mutex);
    uint8_t previous;
    if (fix151) {
      previous = sc::Lower(count);
      if (clear_on_resume && previous == 1 && in_self_suspend.load()) credit.Clear();
    } else {
      previous = count.load();
      while (previous > 0 && !count.compare_exchange_weak(previous, previous - 1)) {
      }
    }
    if (previous == 0 && use_credit) credit.Note(NowNs());  // fix 71
    suspend_cv.notify_all();
    return previous;
  }
};

// Runs a SelfSuspend that must not sleep; returns false (and wakes it) if it slept for 2 s.
bool SelfSuspendReturns(ModelThread& thread) {
  auto done = std::async(std::launch::async, [&] { thread.SelfSuspend(); });
  if (done.wait_for(std::chrono::seconds(2)) == std::future_status::ready) return true;
  thread.stop.store(true);
  {
    std::lock_guard<std::mutex> lock(thread.suspend_mutex);
    thread.suspend_cv.notify_all();
  }
  done.wait();
  return false;
}

// (e) The interleaving: A early, the worker raises, then `between` (B, or B and C), then the worker
// takes the credit. Returns the count the worker is left with.
int Interleave(bool fix151, bool clear_on_resume, int resumes_between, bool* returned) {
  ModelThread worker;
  worker.use_credit = true;
  worker.fix151 = fix151;
  worker.clear_on_resume = clear_on_resume;
  worker.Resume();  // A: finds 0, leaves a credit
  worker.after_raise = [&] {
    for (int i = 0; i < resumes_between; ++i) worker.Resume();  // B (finds 1), then C (finds 0)
  };
  *returned = SelfSuspendReturns(worker);
  return worker.count.load();
}

void CheckInterleaving() {
  bool returned = false;
  const int old_count = Interleave(false, false, 1, &returned);
  std::printf("   fix 71's code: the worker is left with a count of %d\n", old_count);
  Expect(returned && old_count == 255, "the wrap reproduces with fix 71's code (0 -> 255)");
  const int fixed = Interleave(true, true, 1, &returned);
  Expect(returned && fixed == 0, "fix 151: a resume that ended the suspension clears the credit; count 0");
  const int guarded = Interleave(true, false, 1, &returned);
  Expect(returned && guarded == 0, "fix 151: Lower's guard alone keeps the count at 0");
  const int late_credit = Interleave(true, true, 2, &returned);
  Expect(returned && late_credit == 0, "fix 151: a resume after B leaves a new credit, still count 0");
  // The plain cases keep their meaning: a credit still cancels a suspension it came before.
  ModelThread plain;
  plain.use_credit = true;
  plain.Resume();
  Expect(SelfSuspendReturns(plain) && plain.count.load() == 0, "fix 151: an early resume still ends the suspension");
}

// (f) The burst pattern. The master posts a job, resumes the worker 1-20 times in a row, and does
// so again every 2 ms until the job is done (Virtua Tennis 3's main thread fires such bursts). The
// worker runs the jobs it finds, then suspends itself. Reports the largest count a resume found.
struct BurstOutcome {
  uint64_t rounds = 0;
  int max_count = 0;
  bool hung = false;
};
BurstOutcome RunBursts(bool fix151, double seconds) {
  ModelThread worker;
  worker.use_credit = true;
  worker.fix151 = fix151;
  std::atomic<int> jobs{0};
  std::atomic<uint64_t> rounds{0};
  std::atomic<bool> done{false};
  worker.after_raise = [] {
    for (int spin = 0; spin < 200; ++spin) {
      std::atomic_signal_fence(std::memory_order_seq_cst);  // the cost of the kernel call on the console
    }
  };
  std::thread work([&] {
    while (!done.load()) {
      while (jobs.load() > 0) {
        jobs.fetch_sub(1);
        rounds.fetch_add(1);
      }
      worker.SelfSuspend();
    }
  });
  std::mt19937 random(151);
  BurstOutcome out;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
    jobs.fetch_add(1);
    const auto posted = std::chrono::steady_clock::now();
    auto burst_at = posted - std::chrono::milliseconds(10);
    while (jobs.load() > 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now - posted > std::chrono::milliseconds(250)) {
        out.hung = true;
        break;
      }
      if (now - burst_at >= std::chrono::milliseconds(2)) {
        burst_at = now;
        const int burst = 1 + int(random() % 20);
        for (int i = 0; i < burst; ++i) {
          const int previous = worker.Resume();
          if (previous > out.max_count) out.max_count = previous;
        }
      }
    }
    if (out.hung) break;
  }
  out.rounds = rounds.load();
  done.store(true);
  worker.stop.store(true);
  {
    std::lock_guard<std::mutex> lock(worker.suspend_mutex);
    worker.suspend_cv.notify_all();
  }
  work.join();
  return out;
}

void CheckBursts() {
  BurstOutcome old;
  for (int attempt = 0; attempt < 5 && old.max_count <= 1; ++attempt) old = RunBursts(false, 1.0);
  std::printf("   bursts, fix 71's code: largest count a resume found %d, %llu rounds%s\n", old.max_count,
              (unsigned long long)old.rounds, old.hung ? ", HUNG" : "");
  Expect(old.max_count > 1, "the count rises above 1 under bursts with fix 71's code (the wrap, reproduced)");
  const BurstOutcome fixed = RunBursts(true, 1.0);
  std::printf("   bursts, fix 151: largest count a resume found %d, %llu rounds%s\n", fixed.max_count,
              (unsigned long long)fixed.rounds, fixed.hung ? ", HUNG" : "");
  Expect(!fixed.hung && fixed.max_count <= 1 && fixed.rounds > 1000,
         "fix 151: the count never rises above 1 and the bursts run for a second without a hang");
}

// The job pattern: the worker sets idle and suspends itself (some guest code between the two);
// the master waits for idle, clears it and resumes the worker. Returns the rounds done and
// whether it hung (no round for 250 ms) within `seconds`.
struct Outcome {
  uint64_t rounds = 0;
  bool hung = false;
  double seconds = 0;
};
Outcome RunJobs(bool use_credit, double seconds) {
  ModelThread worker;
  worker.use_credit = use_credit;
  std::atomic<int> idle{0};
  std::atomic<uint64_t> rounds{0};
  std::atomic<bool> done{false};
  std::thread work([&] {
    while (!done.load()) {
      idle.store(1);
      for (int spin = 0; spin < 50; ++spin) {
        std::atomic_signal_fence(std::memory_order_seq_cst);  // the guest's code between saying so and NtSuspendThread
      }
      worker.SelfSuspend();
      rounds.fetch_add(1);
    }
  });
  const auto start = std::chrono::steady_clock::now();
  Outcome out;
  auto last_progress = start;
  uint64_t last_rounds = 0;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - start).count() >= seconds) break;
    if (idle.exchange(0)) worker.Resume();
    const uint64_t r = rounds.load();
    if (r != last_rounds) {
      last_rounds = r;
      last_progress = now;
    } else if (now - last_progress > std::chrono::milliseconds(250)) {
      out.hung = true;
      break;
    }
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  out.rounds = rounds.load();
  done.store(true);
  worker.stop.store(true);
  {
    std::lock_guard<std::mutex> lock(worker.suspend_mutex);
    worker.suspend_cv.notify_all();
  }
  work.join();
  return out;
}

void CheckStress() {
  // Without the credit: reproduced within a few tries of up to a second each.
  Outcome without;
  for (int attempt = 0; attempt < 5 && !without.hung; ++attempt) without = RunJobs(false, 1.0);
  std::printf("   without the credit: %s after %llu rounds in %.3f s\n", without.hung ? "HUNG" : "no hang",
              (unsigned long long)without.rounds, without.seconds);
  Expect(without.hung, "the lost wake-up reproduces without the credit");
  const Outcome with = RunJobs(true, 1.0);
  std::printf("   with the credit: %s, %llu rounds in %.3f s\n", with.hung ? "HUNG" : "no hang",
              (unsigned long long)with.rounds, with.seconds);
  Expect(!with.hung && with.rounds > 1000, "with the credit the same loop runs for a second");

  // A resume long before a suspension leaves no credit: the thread still sleeps until resumed.
  ModelThread late;
  late.use_credit = true;
  late.Resume();  // count 0: a credit
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::atomic<bool> woke{false};
  std::thread sleeper([&] {
    late.SelfSuspend();
    woke.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Expect(!woke.load(), "a lapsed credit does not wake a later suspension");
  late.Resume();
  sleeper.join();
  Expect(woke.load(), "the resume after it does");
}

void CheckTree() {
  const std::string k = ".deps/xenia-canary/src/xenia/kernel/";
  const std::string code = Read(k + "xthread.cc");
  const size_t resume = code.find("X_STATUS XThread::Resume(uint32_t* out_suspend_count) {");
  const size_t zero = code.find("suspend_trace_.Note(xbox360ps5::suspend_trace::kResumeFoundRunning, resumer, 0, now);", resume);
  const size_t note = code.find("pending_resume_.Note(", zero);
  const size_t self = code.find("uint32_t XThread::SelfSuspend() {");
  Expect(resume != std::string::npos && zero != std::string::npos && note != std::string::npos && note < self,
         "a resume that finds a count of 0 leaves the credit");
  const size_t raise = code.find("const uint32_t previous = xbox360ps5::suspend_count::Raise(suspend_count);", self);
  const size_t flag = code.find("in_self_suspend_.store(true, std::memory_order_relaxed);", raise);
  const size_t lock = code.find("std::unique_lock<std::mutex> lock(suspend_mutex_);", raise);
  const size_t take = code.find("if (pending_resume_.Take(", lock);
  const size_t undo = code.find("const uint8_t before = xbox360ps5::suspend_count::Lower(suspend_count);", take);
  const size_t cancelled = code.find("suspend_trace_.Note(before ? xbox360ps5::suspend_trace::kSuspendCancelled", undo);
  const size_t early_return = code.find("return previous;", cancelled);
  const size_t wait = code.find("suspend_cv_.wait(lock,", lock);
  const size_t unflag = code.find("in_self_suspend_.store(false, std::memory_order_relaxed);", wait);
  Expect(self != std::string::npos && raise != std::string::npos && flag < lock && lock != std::string::npos &&
             take != std::string::npos && undo != std::string::npos && cancelled != std::string::npos &&
             early_return < wait && unflag != std::string::npos,
         "SelfSuspend raises through Raise, takes the credit under the lock Resume reads the count under, "
         "lowers through Lower and returns at once, noted with the true count");
  const size_t self_end = code.find("#endif  // !XE_PLATFORM_WIN32", self);
  const std::string self_body = code.substr(self, self_end - self);
  Expect(self_body.find("suspend_count.fetch_add(") == std::string::npos &&
             self_body.find("suspend_count.fetch_sub(") == std::string::npos,
         "fix 151: SelfSuspend changes the count only through Raise and Lower");
  const size_t lower = code.find("const uint8_t previous = xbox360ps5::suspend_count::Lower(suspend_count);", resume);
  const size_t clear =
      code.find("if (previous == 1 && in_self_suspend_.load(std::memory_order_relaxed)) pending_resume_.Clear();", lower);
  Expect(lower != std::string::npos && clear != std::string::npos && clear < self,
         "fix 151: Resume lowers through Lower and clears the credit when it ends the thread's own suspension");
  const size_t suspend = code.find("X_STATUS XThread::Suspend(uint32_t* out_suspend_count) {");
  const size_t remote = code.find("const uint8_t previous_suspend_count = xbox360ps5::suspend_count::Raise(", suspend);
  const size_t refused =
      code.find("return X_STATUS(xbox360ps5::suspend_count::kStatusSuspendCountExceeded);", remote);
  Expect(suspend != std::string::npos && remote != std::string::npos && refused != std::string::npos && remote < self,
         "fix 151: a suspension by another thread raises through Raise and is refused at the limit");
  const std::string header = Read(k + "xthread.h");
  Expect(header.find("xbox360ps5::pending_resume::Credit pending_resume_;") != std::string::npos,
         "each thread has its credit");
  Expect(header.find("std::atomic<bool> in_self_suspend_{false};") != std::string::npos &&
             header.find("#include \"xbox360ps5/suspend_count.hpp\"  // fix 151") != std::string::npos,
         "fix 151: each thread knows when its count holds its own suspension");
}

}  // namespace

int main() {
  CheckCredit();
  CheckCount();
  CheckTrace();
  CheckInterleaving();
  CheckBursts();
  CheckStress();
  CheckTree();
  if (failures) {
    std::printf("check-pending-resume: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-pending-resume: ok\n");
  return 0;
}
