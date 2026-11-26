// Host check of fix 31 (Minecraft: Xbox 360 Edition at ~1 FPS in a world):
// the core's wait objects (xenia/base/posix_conditions.h, the code
// threading_posix.cc uses), its critical sections
// (xenia/kernel/util/critical_section.h, the code RtlEnterCriticalSection and
// RtlLeaveCriticalSection use) and the "Waits:" report (xenia/base/wait_stats.h).
//
// Built by tools/source-1.0.2/run-fix-checks.sh in several ways:
//   - fix 31 (targeted wake-ups, host spin) and XBOX360PS5_LEGACY_WAKES (1.0.2),
//     both with the multi-object poll bound raised to 60 s, so that a lost
//     wake-up hangs the check (its watchdog fails it) instead of costing 4 ms;
//   - fix 31 under ThreadSanitizer, and with the real 4 ms bound under
//     AddressSanitizer and UndefinedBehaviorSanitizer.
// Fix 180 (Spider-Man: Web of Shadows at 9-17 FPS, issue #78): the atomic
// pulse (PosixCondition<Event>::PulseAtomic): the threads waiting at the pulse
// are released (one for an auto-reset event, all for a manual-reset one), a
// released thread that runs late still gets it, a thread that starts waiting
// afterwards does not, nothing is left set, no wake-up is lost, and it does
// not sleep; the switch (xbox360ps5/event_pulse.hpp) and the per-thread line
// of the report.
// Exit status 0 when every case passes.

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace xe {
namespace threading {
// threading_posix.cc blocks the suspend signal here; the check has no
// suspension, so this only counts as the real one does.
class SuspensionDeferred {
 public:
  SuspensionDeferred();
};
}  // namespace threading
}  // namespace xe

#include "xenia/base/posix_conditions.h"
#include "xenia/base/wait_stats.h"
#include "xenia/kernel/util/critical_section.h"
#ifndef CHECK_NO_FIX180  // run-fix-checks.sh: a build that leaves fix 180 out
#include "xbox360ps5/event_pulse.hpp"
#endif

xe::threading::SuspensionDeferred::SuspensionDeferred() {
  xe::wait_stats::Add(xe::wait_stats::kSuspendMask);
}

// assert_true's failure path in debug builds (xenia/base/assert.h).
void xe::AssertFailed(const char* expression, const char* file, int line) {
  std::printf("FAIL: assertion %s at %s:%d\n", expression, file, line);
  std::fflush(stdout);
  std::abort();
}

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define CHECK_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define CHECK_SANITIZED 1
#endif
#ifndef CHECK_SANITIZED
#define CHECK_SANITIZED 0
#endif

using namespace std::chrono_literals;
using xe::threading::PosixCondition;
using xe::threading::PosixConditionBase;
using xe::threading::WaitResult;
using Clock = std::chrono::steady_clock;

namespace {

constexpr bool kFix = xe::threading::kTargetedWakes;
constexpr int kScale = CHECK_SANITIZED ? 10 : 1;  // fewer rounds when slow
std::atomic<int> failures{0};
std::atomic<bool> finished{false};

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      ++failures;                                              \
      std::printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
    }                                                          \
  } while (0)

using Event = PosixCondition<xe::threading::Event>;
using Semaphore = PosixCondition<xe::threading::Semaphore>;
using Mutant = PosixCondition<xe::threading::Mutant>;

// An event whose mutex the check can hold, to keep a multi-object wait from
// looking at its objects for a while.
class HeldEvent : public Event {
 public:
  using Event::Event;
  std::mutex& mutex() { return mutex_; }
};

std::pair<WaitResult, size_t> WaitAny(std::vector<PosixConditionBase*> handles,
                                      std::chrono::milliseconds timeout =
                                          std::chrono::milliseconds::max()) {
  return PosixConditionBase::WaitMultiple(std::move(handles), false, timeout);
}

double Ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

// Waits until cond() is true or `limit` passes.
template <typename F>
bool Eventually(F cond, std::chrono::milliseconds limit = 5000ms) {
  const auto until = Clock::now() + limit;
  while (Clock::now() < until) {
    if (cond()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return cond();
}

uint64_t TotalCount(xe::wait_stats::Counter counter) {
  uint64_t sum = 0;
  const size_t used =
      std::min(xe::wait_stats::next_slot.load(), xe::wait_stats::kSlots);
  for (size_t i = 0; i < used; ++i) {
    sum += xe::wait_stats::slots[i].count[counter].load();
  }
  return sum;
}

void SingleWaits() {
  std::printf("single-object waits\n");
  Event manual(true, false), automatic(false, false);
  CHECK(automatic.Wait(20ms) == WaitResult::kTimeout, "unsignaled waits");
  automatic.Signal();
  CHECK(automatic.Wait(0ms) == WaitResult::kSuccess, "signaled");
  CHECK(automatic.Wait(0ms) == WaitResult::kTimeout, "auto-reset consumed");
  manual.Signal();
  CHECK(manual.Wait(0ms) == WaitResult::kSuccess, "manual");
  CHECK(manual.Wait(0ms) == WaitResult::kSuccess, "manual stays set");
  manual.Reset();
  CHECK(manual.Wait(0ms) == WaitResult::kTimeout, "manual reset");

  Semaphore semaphore(0, 2);
  CHECK(!semaphore.Release(3, nullptr), "above the maximum");
  int previous = -1;
  CHECK(semaphore.Release(2, &previous) && previous == 0, "release");
  CHECK(semaphore.Wait(0ms) == WaitResult::kSuccess, "count 2");
  CHECK(semaphore.Wait(0ms) == WaitResult::kSuccess, "count 1");
  CHECK(semaphore.Wait(0ms) == WaitResult::kTimeout, "count 0");

  // A blocked waiter is woken by a set from another thread.
  std::atomic<bool> woke{false};
  std::thread waiter([&] {
    woke = automatic.Wait(std::chrono::milliseconds::max()) ==
           WaitResult::kSuccess;
  });
  std::this_thread::sleep_for(20ms);
  automatic.Signal();
  waiter.join();
  CHECK(woke, "blocked single wait woken");

  Mutant mutant(false);
  CHECK(mutant.Wait(0ms) == WaitResult::kSuccess, "mutant free");
  CHECK(mutant.Wait(0ms) == WaitResult::kSuccess, "mutant recursive");
  std::atomic<int> other{-1};
  std::thread taker([&] { other = int(mutant.Wait(30ms)); });
  taker.join();
  CHECK(other == int(WaitResult::kTimeout), "mutant owned elsewhere");
  CHECK(mutant.Release() && mutant.Release(), "released twice");
  std::thread taker2([&] {
    other = int(mutant.Wait(1000ms));
    mutant.Release();
  });
  taker2.join();
  CHECK(other == int(WaitResult::kSuccess), "mutant free again");
}

void TargetedRegistration() {
  std::printf("multi-object wait: registration and targeted wake-up\n");
  Event a(false, false), b(false, false), c(false, false), unrelated(false, false);
  std::atomic<int> index{-1};
  std::atomic<int> result{-1};
  Clock::time_point signaled_at;
  std::atomic<int64_t> woke_after_us{-1};
  std::thread waiter([&] {
    auto r = WaitAny({&a, &b, &c});
    woke_after_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - signaled_at)
                        .count();
    result = int(r.first);
    index = int(r.second);
  });
  if (kFix) {
    CHECK(Eventually([&] { return a.multi_waiter_count() == 1; }),
          "registered with each object");
    CHECK(b.multi_waiter_count() == 1 && c.multi_waiter_count() == 1,
          "registered with each object");
  } else {
    std::this_thread::sleep_for(50ms);
    CHECK(a.multi_waiter_count() == 0, "1.0.2 keeps no registration");
  }
  CHECK(unrelated.multi_waiter_count() == 0, "not with other objects");
  // Signals of an object the thread does not wait on do not wake it (1.0.2
  // woke it every time).
  // Sleeps ended by a wake-up, not by the bound.
  const auto woken = [] {
    return TotalCount(xe::wait_stats::kMultiSleep) -
           TotalCount(xe::wait_stats::kMultiBound);
  };
  const uint64_t sleeps = woken();
  for (int i = 0; i < 200; ++i) {
    unrelated.Signal();
    unrelated.Wait(0ms);
  }
  std::this_thread::sleep_for(20ms);
  const uint64_t extra = woken() - sleeps;
  if (kFix) {
    CHECK(extra == 0, "woken %llu times by an unrelated object",
          (unsigned long long)extra);
  } else {
    CHECK(extra > 0, "1.0.2 wakes it on every signal (%llu)",
          (unsigned long long)extra);
  }
  std::printf("  wake-ups from 200 unrelated signals: %llu\n",
              (unsigned long long)extra);
  signaled_at = Clock::now();
  b.Signal();
  waiter.join();
  CHECK(result == int(WaitResult::kSuccess) && index == 1, "index %d",
        index.load());
  CHECK(a.multi_waiter_count() == 0 && b.multi_waiter_count() == 0 &&
            c.multi_waiter_count() == 0,
        "unregistered");
  CHECK(b.Wait(0ms) == WaitResult::kTimeout, "auto-reset consumed by it");
  CHECK(woke_after_us >= 0 && woke_after_us < 1000000, "woke after %lld us",
        (long long)woke_after_us);
}

void WaitAll() {
  std::printf("multi-object wait for all\n");
  Event a(false, false), b(false, false);
  std::atomic<bool> done{false};
  std::thread waiter([&] {
    auto r = PosixConditionBase::WaitMultiple({&a, &b}, true,
                                              std::chrono::milliseconds::max());
    CHECK(r.first == WaitResult::kSuccess, "wait all");
    done = true;
  });
  std::this_thread::sleep_for(20ms);
  a.Signal();
  std::this_thread::sleep_for(50ms);
  CHECK(!done, "returned with one of two");
  b.Signal();
  waiter.join();
  CHECK(done, "returned with both");
  CHECK(a.Wait(0ms) == WaitResult::kTimeout && b.Wait(0ms) == WaitResult::kTimeout,
        "both consumed");
}

void Timeouts() {
  std::printf("multi-object timeouts\n");
  Event a(false, false), b(false, false);
  const auto start = Clock::now();
  auto r = WaitAny({&a, &b}, 30ms);
  const double ms = Ms(Clock::now() - start);
  CHECK(r.first == WaitResult::kTimeout, "timed out");
  CHECK(ms >= 29 && ms < 2000, "after %.1f ms", ms);
  CHECK(a.multi_waiter_count() == 0 && b.multi_waiter_count() == 0,
        "unregistered after a timeout");
  // A signal after the timeout is kept for the next waiter.
  a.Signal();
  CHECK(WaitAny({&b, &a}, 0ms).second == 1, "kept");
}

// A signal kept for the threads that waited when it was set must reach a
// later waiter once those leave without it (here: a multi-object wait that
// took another object). 1.0.2 stranded it until the next Set.
void KeptSignalReleased() {
  std::printf("auto-reset signal kept for a waiter that takes another object\n");
  Event first(false, false);
  Event kept(false, false);
  HeldEvent gate(false, false);
  std::atomic<int> taken{-1};
  std::thread multi([&] { taken = int(WaitAny({&first, &kept, &gate}).second); });
  // Until the thread sleeps registered (1.0.2: until it sleeps).
  if (kFix) {
    CHECK(Eventually([&] { return kept.multi_waiter_count() == 1; }), "registered");
  } else {
    std::this_thread::sleep_for(100ms);
  }
  std::atomic<bool> later_woke{false};
  std::thread later;
  {
    // While the multi-object wait cannot look (it cannot lock `gate`): both of
    // its first objects are set, and a later thread starts waiting for `kept`.
    std::lock_guard<std::mutex> hold(gate.mutex());
    first.Signal();
    kept.Signal();  // kept for the multi-object wait, the only waiter now
    later = std::thread([&] {
      later_woke = kept.Wait(std::chrono::milliseconds::max()) ==
                   WaitResult::kSuccess;
    });
    std::this_thread::sleep_for(50ms);  // `later` is blocked by now
    CHECK(!later_woke, "the later waiter took a signal kept for another");
  }
  multi.join();
  CHECK(taken == 0, "the multi-object wait took the first object (%d)",
        taken.load());
  if (kFix) {
    CHECK(Eventually([&] { return later_woke.load(); }, 2000ms),
          "the later waiter did not get the signal");
  } else {
    std::this_thread::sleep_for(200ms);
    CHECK(!later_woke, "1.0.2 strands it");
    kept.Signal();  // 1.0.2 needs another Set
  }
  later.join();
  CHECK(later_woke, "later waiter woke");
}

// Two threads hand a token back and forth through auto-reset events, each
// waiting on its event and an idle one. With the 60 s bound, a lost wake-up
// stops the exchange (the watchdog then fails the check).
void PingPong() {
  const int rounds = 20000 / kScale;
  std::printf("ping-pong through multi-object waits, %d rounds\n", rounds);
  Event ping(false, false), pong(false, false), idle1(false, false),
      idle2(false, false);
  const auto start = Clock::now();
  std::thread other([&] {
    for (int i = 0; i < rounds; ++i) {
      auto r = WaitAny({&idle1, &ping});
      CHECK(r.second == 1, "ping");
      pong.Signal();
    }
  });
  for (int i = 0; i < rounds; ++i) {
    ping.Signal();
    auto r = WaitAny({&pong, &idle2});
    CHECK(r.second == 0, "pong");
  }
  other.join();
  const double ms = Ms(Clock::now() - start);
  std::printf("  %.1f us per round trip\n", ms * 1000 / rounds);
}

// Semaphores released by producers and taken by consumers that each wait on
// all of them in their own order: every unit is taken exactly once, and no
// consumer is left asleep with units available.
void SemaphoreStress() {
  const int kSemaphores = 4, kProducers = 3, kConsumers = 6;
  const int per_producer = 20000 / kScale;
  const int total = kProducers * per_producer;
  std::printf("semaphore stress: %d producers, %d consumers, %d units\n",
              kProducers, kConsumers, total);
  std::vector<std::unique_ptr<Semaphore>> semaphores;
  for (int i = 0; i < kSemaphores; ++i) {
    semaphores.push_back(std::make_unique<Semaphore>(0, 1u << 30));
  }
  // Consumers wait without a timeout (a unit left with every consumer asleep
  // is a lost wake-up), and leave when they take a unit of `stop`.
  Semaphore stop(0, kConsumers);
  std::atomic<int> taken{0};
  std::vector<std::thread> threads;
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&, c] {
      std::mt19937 random(c + 1);
      std::vector<PosixConditionBase*> order;
      for (auto& s : semaphores) order.push_back(s.get());
      order.push_back(&stop);
      while (true) {
        std::shuffle(order.begin(), order.end(), random);
        auto r = WaitAny(order);
        CHECK(r.first == WaitResult::kSuccess, "consumer wait");
        if (order[r.second] == &stop) break;
        taken.fetch_add(1);
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      std::mt19937 random(100 + p);
      for (int i = 0; i < per_producer; ++i) {
        semaphores[random() % kSemaphores]->Release(1, nullptr);
        if (i % 64 == 0) std::this_thread::yield();
      }
    });
  }
  for (auto& t : producers) t.join();
  CHECK(Eventually([&] { return taken.load() == total; }, 60000ms),
        "%d of %d taken", taken.load(), total);
  stop.Release(kConsumers, nullptr);
  for (auto& t : threads) t.join();
  CHECK(taken.load() == total, "%d of %d taken", taken.load(), total);
  for (auto& s : semaphores) {
    CHECK(s->Wait(0ms) == WaitResult::kTimeout, "units left over");
    CHECK(s->multi_waiter_count() == 0, "registration left over");
  }
}

// --- Critical sections -----------------------------------------------------

// The guest structure's fields as relaxed atomics, so that ThreadSanitizer
// sees the same accesses the kernel makes (the owner check reads the owner
// field of a section another thread holds).
struct RelaxedU32 {
  std::atomic<uint32_t> value{0};
  RelaxedU32& operator=(uint32_t v) {
    value.store(v, std::memory_order_relaxed);
    return *this;
  }
  bool operator==(uint32_t v) const {
    return value.load(std::memory_order_relaxed) == v;
  }
};
struct RelaxedI32 {
  std::atomic<int32_t> value{0};
  RelaxedI32& operator=(int32_t v) {
    value.store(v, std::memory_order_relaxed);
    return *this;
  }
  int32_t operator++(int) {
    int32_t v = value.load(std::memory_order_relaxed);
    value.store(v + 1, std::memory_order_relaxed);
    return v;
  }
  int32_t operator--() {
    int32_t v = value.load(std::memory_order_relaxed) - 1;
    value.store(v, std::memory_order_relaxed);
    return v;
  }
};
struct FakeSection {
  int32_t lock_count = -1;
  RelaxedI32 recursion_count;
  RelaxedU32 owning_thread;
  Event event{false, false};  // the section's event (auto-reset)
};

struct SectionCounts {
  std::atomic<uint64_t> entered{0}, spun{0}, blocked{0}, handoffs{0};
};

void Enter(FakeSection& cs, uint32_t me, SectionCounts& counts) {
  using xe::kernel::critical_section::Entered;
  auto entered = xe::kernel::critical_section::Enter(&cs, me, 0, [&] {
    cs.event.Wait(std::chrono::milliseconds::max());
  });
  if (entered == Entered::kSpun) counts.spun++;
  if (entered == Entered::kBlocked) counts.blocked++;
  if (entered != Entered::kRecursive) counts.entered++;
}
void Leave(FakeSection& cs, SectionCounts& counts) {
  if (xe::kernel::critical_section::Leave(&cs, [&] { cs.event.Signal(); })) {
    counts.handoffs++;
  }
}

void CriticalSections() {
  const int kThreads = 4;
  const int rounds = 100000 / kScale;
  std::printf("critical sections: %d threads x %d entries\n", kThreads, rounds);
  FakeSection cs;
  SectionCounts counts;
  uint64_t inside_counter = 0;  // only changed inside the section
  std::atomic<int> inside{0};
  std::atomic<int> overlap{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      const uint32_t me = 0x80000000u + uint32_t(t) * 0x1000;
      for (int i = 0; i < rounds; ++i) {
        Enter(cs, me, counts);
        if (inside.fetch_add(1) != 0) overlap++;
        ++inside_counter;
        if (i % 7 == 0) {
          // Recursion, as games do.
          Enter(cs, me, counts);
          ++inside_counter;
          Leave(cs, counts);
        }
        if (i % 97 == 0) {
          // A longer hold now and then, so that some threads block.
          std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        inside.fetch_sub(1);
        Leave(cs, counts);
      }
    });
  }
  for (auto& t : threads) t.join();
  const uint64_t expected =
      uint64_t(kThreads) * (uint64_t(rounds) + uint64_t((rounds + 6) / 7));
  CHECK(inside_counter == expected, "counter %llu, expected %llu",
        (unsigned long long)inside_counter, (unsigned long long)expected);
  CHECK(overlap == 0, "%d overlapping entries", overlap.load());
  const int32_t final_count =
      xe::kernel::critical_section::LoadLockCount(&cs.lock_count);
  CHECK(final_count == -1, "lock count %d at the end", final_count);
  CHECK(cs.owning_thread == 0u, "owner left");
  CHECK(counts.blocked == counts.handoffs, "%llu blocked, %llu hand-offs",
        (unsigned long long)counts.blocked.load(),
        (unsigned long long)counts.handoffs.load());
  if (!kFix) {
    CHECK(counts.spun == 0, "1.0.2 has no host spin");
  }
  std::printf("  entries %llu: spun %llu, blocked %llu (hand-offs %llu)\n",
              (unsigned long long)counts.entered.load(),
              (unsigned long long)counts.spun.load(),
              (unsigned long long)counts.blocked.load(),
              (unsigned long long)counts.handoffs.load());
}

// A section held for longer than the spin: the second thread blocks and is
// handed the section, as in 1.0.2.
void CriticalSectionLongHold() {
  std::printf("critical section held longer than the spin\n");
  FakeSection cs;
  SectionCounts counts;
  Enter(cs, 1, counts);
  std::atomic<bool> got{false};
  std::thread other([&] {
    Enter(cs, 2, counts);
    got = true;
    Leave(cs, counts);
  });
  std::this_thread::sleep_for(100ms);
  CHECK(!got, "entered a held section");
  const int32_t held_count =
      xe::kernel::critical_section::LoadLockCount(&cs.lock_count);
  CHECK(held_count == 1, "one owner and one waiter: lock count %d", held_count);
  Leave(cs, counts);
  other.join();
  CHECK(got && counts.blocked == 1 && counts.handoffs == 1,
        "handed over (blocked %llu, hand-offs %llu)",
        (unsigned long long)counts.blocked.load(),
        (unsigned long long)counts.handoffs.load());
  CHECK(xe::kernel::critical_section::LoadLockCount(&cs.lock_count) == -1,
        "free at the end");
}

// A section held briefly: with fix 31 the second thread takes it by spinning,
// with no hand-off.
void CriticalSectionShortHold() {
  if (!kFix) return;
  std::printf("critical section held briefly\n");
  FakeSection cs;
  SectionCounts counts;
  std::atomic<bool> holding{false};
  int spun = 0, blocked = 0;
  for (int round = 0; round < 50; ++round) {
    counts.spun = counts.blocked = 0;
    Enter(cs, 1, counts);
    std::thread other([&] {
      holding.wait(false);
      Enter(cs, 2, counts);
      Leave(cs, counts);
    });
    holding = true;
    holding.notify_one();
    // ~2 us: far shorter than the spin, far longer than a CAS.
    const auto until = Clock::now() + 2us;
    while (Clock::now() < until) {
    }
    Leave(cs, counts);
    other.join();
    holding = false;
    spun += int(counts.spun);
    blocked += int(counts.blocked);
  }
  std::printf("  50 rounds: %d taken by spinning, %d blocked\n", spun, blocked);
  CHECK(blocked <= (CHECK_SANITIZED ? 50 : 10), "%d of 50 blocked", blocked);
}

// --- The report ------------------------------------------------------------

void ReportText() {
  std::printf("the Waits: report\n");
  xe::wait_stats::Reporter reporter;
  const auto t0 = Clock::now();
  const uint64_t ticks0 = 1000000000ull;
  CHECK(xe::wait_stats::Report(reporter, 0, t0, ticks0).empty(),
        "first call only starts");
  std::thread worker([] {
    pthread_setname_np(pthread_self(), "Main XThread");
    for (int i = 0; i < 1200; ++i) xe::wait_stats::Add(xe::wait_stats::kCsEnter);
    for (int i = 0; i < 10; ++i) {
      xe::wait_stats::Add(xe::wait_stats::kCsBlocked, 500000);
      xe::wait_stats::Add(xe::wait_stats::kSingleBlocked, 500000);
      xe::wait_stats::CsBlocked(0x40000618, 0, 500000);
    }
    for (int i = 0; i < 24; ++i) {
      xe::wait_stats::Add(xe::wait_stats::kCsHandoff, 30);
      xe::wait_stats::CsHandoff(0x40000618, 30);
    }
    xe::wait_stats::CsBlocked(0x40001000, 1, 10);
  });
  worker.join();
  // A thread named the way the emulator names threads on the PS5 (which
  // cannot read a name back): through RememberName, not pthread_setname_np.
  std::thread named([] {
    xe::wait_stats::RememberName(pthread_self(), "Chunk update");
    xe::wait_stats::Add(xe::wait_stats::kCsBlocked, 100000);
  });
  named.join();
  // 10 s and 10^7 ticks: one tick is a microsecond; 12 frames.
  auto lines =
      xe::wait_stats::Report(reporter, 12, t0 + 10s, ticks0 + 10000000ull);
  for (auto& line : lines) std::printf("  %s\n", line.c_str());
  CHECK(lines.size() == 3, "%zu lines", lines.size());
  if (lines.size() == 3) {
    CHECK(lines[0].find("Waits: 12 frames in 10 s; per frame: 100 "
                        "critical-section entries") == 0,
          "totals");
    CHECK(lines[0].find("1 blocked (463636 us each, 425.0 ms in all), 2 "
                        "hand-offs (30 us to signal)") != std::string::npos,
          "blocked and hand-offs");
    CHECK(lines[1].find("40000618 (spin 0): 1 blocked/frame (500000 us each), "
                        "2 hand-offs") != std::string::npos,
          "section 40000618 first");
    CHECK(lines[1].find("40001000 (spin 256)") != std::string::npos,
          "section 40001000 second");
    CHECK(lines[2].find("'Main XThread' 50% (50%), 'Chunk update' 1% (0%)") !=
              std::string::npos,
          "thread share");
  }
  // Nothing new: no section or thread lines.
  lines = xe::wait_stats::Report(reporter, 12, t0 + 20s, ticks0 + 20000000ull);
  CHECK(lines.size() == 1, "%zu lines when idle", lines.size());
}


#ifndef CHECK_NO_FIX180
// --- Fix 180: the atomic pulse -----------------------------------------------

// Waits until `count` threads are blocked on `event`.
bool Blocked(Event& event, uint32_t count) {
  return Eventually([&] { return event.Waiting() == count; });
}

void PulseNobodyWaiting() {
  std::printf("fix 180: a pulse with nobody waiting leaves the event reset\n");
  Event manual(true, false), automatic(false, false);
  CHECK(manual.PulseAtomic() == 0 && automatic.PulseAtomic() == 0, "released none");
  CHECK(manual.Wait(0ms) == WaitResult::kTimeout, "manual not set");
  CHECK(automatic.Wait(0ms) == WaitResult::kTimeout, "auto not set");
  manual.Signal();
  CHECK(manual.PulseAtomic() == 0, "released none");
  CHECK(manual.Wait(0ms) == WaitResult::kTimeout, "a pulse resets a set event");
  CHECK(manual.Query().state == 0, "Query: reset");
}

void PulseReleasesWaiters() {
  std::printf("fix 180: auto-reset releases one waiter, manual-reset all\n");
  {
    Event automatic(false, false);
    std::atomic<int> woke{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 2; ++i) {
      waiters.emplace_back([&] {
        if (automatic.Wait(std::chrono::milliseconds::max()) == WaitResult::kSuccess) ++woke;
      });
    }
    CHECK(Blocked(automatic, 2), "two waiters blocked");
    CHECK(automatic.PulseAtomic() == 1, "auto-reset: one released");
    CHECK(Eventually([&] { return woke.load() == 1; }), "one woke (%d)", woke.load());
    std::this_thread::sleep_for(50ms);
    CHECK(woke == 1, "exactly one woke (%d)", woke.load());
    CHECK(automatic.Waiting() == 1, "the other still waits");
    CHECK(automatic.Query().state == 0, "not left set");
    automatic.Signal();  // the other one
    for (auto& t : waiters) t.join();
    CHECK(woke == 2, "both done");
    CHECK(automatic.Wait(0ms) == WaitResult::kTimeout, "nothing left");
  }
  {
    Event manual(true, false);
    std::atomic<int> woke{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 3; ++i) {
      waiters.emplace_back([&] {
        if (manual.Wait(std::chrono::milliseconds::max()) == WaitResult::kSuccess) ++woke;
      });
    }
    CHECK(Blocked(manual, 3), "three waiters blocked");
    CHECK(manual.PulseAtomic() == 3, "manual-reset: all released");
    for (auto& t : waiters) t.join();
    CHECK(woke == 3, "all three woke (%d)", woke.load());
    CHECK(manual.Wait(0ms) == WaitResult::kTimeout, "reset after them");
  }
}

// A released waiter that cannot run for a while (here: the check holds the
// event's mutex for 50 ms after the pulse) still gets the pulse; a thread that
// starts waiting after the pulse does not. 1.0.6 reset the event 10 us after
// setting it, and a waiter that ran later missed it.
void PulseLateWaiter() {
  std::printf("fix 180: a late waiter keeps the pulse, a later one never sees it\n");
  for (bool manual_reset : {false, true}) {
    HeldEvent event(manual_reset, false);
    std::atomic<int> early{-1}, later{-1};
    std::thread first([&] { early = int(event.Wait(std::chrono::milliseconds::max())); });
    CHECK(Blocked(event, 1), "first waiter blocked");
    std::thread second;
    {
      CHECK(event.PulseAtomic() == 1, "one released");
      std::lock_guard<std::mutex> hold(event.mutex());
      // Starts waiting after the pulse; it must get the mutex first.
      second = std::thread([&] { later = int(event.Wait(100ms)); });
      std::this_thread::sleep_for(50ms);
    }
    first.join();
    second.join();
    CHECK(early == int(WaitResult::kSuccess), "%s: the waiter that ran late got it (%d)",
          manual_reset ? "manual" : "auto", early.load());
    CHECK(later == int(WaitResult::kTimeout), "%s: the later waiter saw it (%d)",
          manual_reset ? "manual" : "auto", later.load());
    CHECK(event.Wait(0ms) == WaitResult::kTimeout, "%s: nothing left set",
          manual_reset ? "manual" : "auto");
  }
}

void PulseMultiObject() {
  std::printf("fix 180: a multi-object waiter gets the pulse; one that takes another object leaves it reset\n");
  {
    Event idle(false, false), pulsed(false, false);
    std::atomic<int> index{-1};
    std::thread waiter([&] { index = int(WaitAny({&idle, &pulsed}).second); });
    CHECK(Blocked(pulsed, 1), "registered");
    CHECK(pulsed.PulseAtomic() == 1, "one released");
    waiter.join();
    CHECK(index == 1, "took the pulsed event (%d)", index.load());
    CHECK(pulsed.Wait(0ms) == WaitResult::kTimeout, "nothing left set");
  }
  {
    Event first(false, false), pulsed(true, false);
    HeldEvent gate(false, false);
    std::atomic<int> index{-1};
    std::thread waiter([&] { index = int(WaitAny({&first, &pulsed, &gate}).second); });
    CHECK(Blocked(pulsed, 1), "registered");
    {
      std::lock_guard<std::mutex> hold(gate.mutex());
      first.Signal();
      CHECK(pulsed.PulseAtomic() == 1, "one released");
    }
    waiter.join();
    CHECK(index == 0, "took the first object (%d)", index.load());
    CHECK(pulsed.Waiting() == 0, "nobody waits");
    CHECK(pulsed.Wait(0ms) == WaitResult::kTimeout, "the pulse is not left set");
  }
}

// Each round the producer pulses once the consumer is blocked; the consumer
// must be released exactly once a round. With the 60 s bound, a lost wake-up
// stops the rounds (the watchdog then fails the check).
void PulseNoLostWakeups() {
  const int rounds = 4000 / kScale;
  std::printf("fix 180: %d pulse rounds, each waiter released once\n", rounds);
  for (bool multi : {false, true}) {
    Event event(false, false), idle(false, false);
    std::atomic<int> got{0};
    std::thread consumer([&] {
      for (int i = 0; i < rounds; ++i) {
        const bool ok = multi ? WaitAny({&idle, &event}).second == 1
                              : event.Wait(std::chrono::milliseconds::max()) ==
                                    WaitResult::kSuccess;
        if (ok) ++got;
      }
    });
    int released = 0;
    for (int i = 0; i < rounds; ++i) {
      while (event.Waiting() != 1 || got.load() != i) std::this_thread::yield();
      released += int(event.PulseAtomic());
    }
    consumer.join();
    CHECK(got == rounds && released == rounds, "%s: %d woke, %d released of %d",
          multi ? "multi" : "single", got.load(), released, rounds);
    CHECK(event.Wait(0ms) == WaitResult::kTimeout, "nothing left");
  }
}

void PulseDoesNotSleep() {
  std::printf("fix 180: a pulse does not sleep\n");
  Event event(false, false);
  const int n = 2000;
  const auto start = Clock::now();
  for (int i = 0; i < n; ++i) event.PulseAtomic();
  const double idle_us = Ms(Clock::now() - start) * 1000.0 / n;
  std::atomic<bool> stop{false};
  std::thread waiter([&] {
    while (!stop) event.Wait(5ms);
  });
  CHECK(Blocked(event, 1), "waiter blocked");
  double busy_us = 0;
  int pulses = 0;
  while (pulses < 200) {
    if (event.Waiting() == 1) {
      const auto one = Clock::now();
      event.PulseAtomic();
      busy_us += Ms(Clock::now() - one) * 1000.0;
      ++pulses;
    }
    std::this_thread::yield();
  }
  stop = true;
  event.Signal();
  waiter.join();
  busy_us /= pulses;
  const double limit = CHECK_SANITIZED ? 500.0 : 50.0;
  std::printf("  %.2f us a pulse with nobody waiting, %.2f us with a waiter\n", idle_us, busy_us);
  CHECK(idle_us < limit, "%.1f us a pulse (1.0.6 yielded and slept 10 us)", idle_us);
  CHECK(busy_us < limit * 4, "%.1f us a pulse with a waiter", busy_us);
}

void PulseSwitch() {
  std::printf("fix 180: the switch (event-pulse.txt, the setting)\n");
  namespace ep = xbox360ps5::event_pulse;
  CHECK(ep::ParseSwitch("atomic") == ep::kAtomic, "atomic");
  CHECK(ep::ParseSwitch("\xEF\xBB\xBF  Atomic\r\n") == ep::kAtomic, "BOM, blanks, case");
  CHECK(ep::ParseSwitch("old\n") == ep::kOld, "old");
  CHECK(ep::ParseSwitch("") == -1 && ep::ParseSwitch("atomically") == -1 &&
            ep::ParseSwitch("fast") == -1,
        "other words");
  char path[] = "/tmp/check-event-pulse-XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0, "temporary file");
  if (fd < 0) return;
  ::close(fd);
  ::unlink(path);
  ep::path_override.store(path);
  auto r = ep::Refresh(-1, true);
  CHECK(r.changed && r.mode == ep::kOld && r.source == ep::kDefault, "no file: old (default)");
  CHECK(!ep::Atomic(), "old");
  r = ep::Refresh(ep::kAtomic, true);
  CHECK(r.changed && r.mode == ep::kAtomic && r.source == ep::kSetting, "the setting: atomic");
  std::ofstream(path) << "old\n";
  r = ep::Refresh(ep::kAtomic, true);
  CHECK(r.changed && r.mode == ep::kOld && r.source == ep::kFile, "the file overrides the setting");
  std::ofstream(path) << "atomic\n";
  r = ep::Refresh(-1, false);  // within kCheckTicks: not read again
  CHECK(!r.changed && r.mode == ep::kOld, "not re-read at once");
  r = ep::Refresh(-1, true);
  CHECK(r.changed && r.mode == ep::kAtomic && r.source == ep::kFile && ep::Atomic(), "file: atomic");
  r = ep::Refresh(-1, true);
  CHECK(!r.changed, "unchanged");
  std::ofstream(path) << "something\n";
  r = ep::Refresh(-1, true);
  CHECK(r.changed && r.mode == ep::kOld && r.source == ep::kDefault, "unknown word: the default");
  ::unlink(path);
  ep::path_override.store(nullptr);
}

void PulseReportLine() {
  std::printf("fix 180: the per-thread line of the report\n");
  xe::wait_stats::Reporter reporter;
  const auto t0 = Clock::now();
  const uint64_t ticks0 = 5000000000ull;
  xe::wait_stats::Report(reporter, 0, t0, ticks0);
  std::thread pulser([] {
    xe::wait_stats::RememberName(pthread_self(), "Game Master (F8000008)");
    xe::wait_stats::SetGuest(0xF8000008);
    for (int i = 0; i < 100; ++i) xe::wait_stats::AddPulse(30, 1);
    xe::wait_stats::AddPulse(500, 2);
    for (int i = 0; i < 40; ++i) xe::wait_stats::Add(xe::wait_stats::kDelayZero);
    for (int i = 0; i < 20; ++i) xe::wait_stats::Add(xe::wait_stats::kPoll);
    for (int i = 0; i < 10; ++i) xe::wait_stats::Add(xe::wait_stats::kPollMiss);
  });
  pulser.join();
  std::thread spinner([] {
    xe::wait_stats::RememberName(pthread_self(), "JQ worker 0 (CPU 2) (F8000014)");
    xe::wait_stats::SetGuest(0xF8000014);
    for (int i = 0; i < 3000; ++i) xe::wait_stats::Add(xe::wait_stats::kDelayShort);
    for (int i = 0; i < 10; ++i) xe::wait_stats::Add(xe::wait_stats::kDelayLong);
  });
  spinner.join();
  // 10 s and 10^7 ticks: one tick is a microsecond.
  auto lines = xe::wait_stats::Report(reporter, 100, t0 + 10s, ticks0 + 10000000ull);
  std::string line;
  for (auto& l : lines) {
    if (l.find("Waits: kernel calls by thread (fix 180") == 0) line = l;
  }
  std::printf("  %s\n", line.c_str());
  CHECK(!line.empty(), "the line is there");
  CHECK(line.find(" F8000008 'Game Master (F8000008)' pulses 10/s (1.01 blocked waiters each, "
                  "35 us each, longest 500 us), delays 4/s (0: 4, under 1 ms: 0, 1 ms or more: "
                  "0), polls 2/s (1 found nothing);") != std::string::npos,
        "the pulser first");
  CHECK(line.find("; F8000014 'JQ worker 0 (CPU 2) (F8' pulses 0/s (0.00 blocked waiters each, "
                  "0.0 us each, longest 0.0 us), delays 301/s (0: 0, under 1 ms: 300, 1 ms or "
                  "more: 1)") != std::string::npos,
        "the spinner second, with its guest handle");
  lines = xe::wait_stats::Report(reporter, 100, t0 + 20s, ticks0 + 20000000ull);
  for (auto& l : lines) {
    CHECK(l.find("kernel calls by thread") == std::string::npos, "no line when idle");
  }
}
#endif  // CHECK_NO_FIX180

}  // namespace

int main() {
  std::printf("check-wait-wakeups: %s, multi-object bound %lld ms%s\n",
              kFix ? "fix 31" : "1.0.2 (XBOX360PS5_LEGACY_WAKES)",
              (long long)xe::threading::kMultiWaitBound.count(),
              CHECK_SANITIZED ? ", sanitized" : "");
  std::thread([] {
    const auto limit = CHECK_SANITIZED ? 600s : 240s;
    const auto until = Clock::now() + limit;
    while (Clock::now() < until) {
      if (finished) return;
      std::this_thread::sleep_for(100ms);
    }
    std::printf("FAIL: still running after %lld s (a lost wake-up?)\n",
                (long long)std::chrono::duration_cast<std::chrono::seconds>(limit).count());
    std::fflush(stdout);
    std::_Exit(2);
  }).detach();

  ReportText();
  SingleWaits();
  TargetedRegistration();
  WaitAll();
  Timeouts();
  KeptSignalReleased();
  PingPong();
  SemaphoreStress();
  CriticalSections();
  CriticalSectionLongHold();
  CriticalSectionShortHold();
#ifndef CHECK_NO_FIX180
  PulseSwitch();
  PulseReportLine();
  PulseNobodyWaiting();
  PulseReleasesWaiters();
  PulseLateWaiter();
  PulseMultiObject();
  PulseNoLostWakeups();
  PulseDoesNotSleep();
#endif

  finished = true;
  if (failures) {
    std::printf("check-wait-wakeups: %d failure(s)\n", failures.load());
    return 1;
  }
  std::printf("check-wait-wakeups: all passed\n");
  return 0;
}
