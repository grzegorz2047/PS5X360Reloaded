// SPDX-License-Identifier: MIT
// Fix 126 (xbox360ps5/sample_ticket.hpp, platform/ps5/crash_report.cpp): MEASURE's sampler asks a
// thread for its instruction with a signal; a thread that answers late no longer costs the round
// 100 ms more. Grand Theft Auto IV on 1.0.5-work-fix120: F8001014 answered 42 of 94 rounds and
// MEASURE took 94 rounds in 10.1 s (about 1,000 elsewhere).
//  (a) the ticket: only the pending request's target answers it; an answered request is read
//      once; one given up on is cancelled and a late answer then claims nothing, nor a later
//      request to another thread; a claimed one is waited for;
//  (b) threads: a "handler" answering after the sampler gave up writes nothing, and the sampler
//      returns after its own wait only; 20,000 requests against a racing handler, no answer lost
//      or taken twice;
//  (c) on Linux, real SIGUSR2 to a thread that blocks the signal for 200 ms: the request returns
//      in its 20 ms, the late handler claims nothing;
//  (d) the assembled tree: the sampler uses the ticket, the second wait is gone, a late answer
//      prints no WHERE line, ProbeThread names its thread.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "xbox360ps5/sample_ticket.hpp"

#if defined(__linux__)
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

using xbox360ps5::sample_ticket::Ticket;
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
using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

void CheckTicket() {
  Ticket ticket;
  uint64_t claimed = 0;
  uint64_t r = ticket.Ask(7);
  Expect(!ticket.Claim(8, claimed), "another thread does not answer the request");
  Expect(ticket.Claim(7, claimed), "the target answers it");
  Expect(!ticket.Claim(7, claimed), "an answer is claimed once");
  ticket.Done(claimed);
  Expect(ticket.Answered(r) && ticket.Finish(r), "answered: the sample is there");

  r = ticket.Ask(7);
  Expect(!ticket.Answered(r) && !ticket.Finish(r), "unanswered: cancelled");
  Expect(!ticket.Claim(7, claimed), "a late answer after the cancel claims nothing");
  const uint64_t next = ticket.Ask(9);
  Expect(next == r + 1, "rounds count up");
  Expect(!ticket.Claim(7, claimed), "a late answer does not take a later request to another thread");
  Expect(ticket.Claim(9, claimed), "the later request's target still answers it");
  ticket.Done(claimed);
  Expect(ticket.Finish(next), "and it is read");
}

void CheckThreads() {
  {
    // A handler that answers after the sampler gave up.
    Ticket ticket;
    int slot = 0;
    const uint64_t r = ticket.Ask(1);
    std::atomic<bool> gave_up{false};
    std::atomic<bool> wrote{false};
    std::thread late([&] {
      while (!gave_up.load()) std::this_thread::yield();
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      uint64_t claimed;
      if (ticket.Claim(1, claimed)) {
        slot = 42;
        wrote = true;
        ticket.Done(claimed);
      }
    });
    const auto t = Clock::now();
    while (!ticket.Answered(r) && MsSince(t) < 20) std::this_thread::sleep_for(std::chrono::microseconds(50));
    const bool taken = ticket.Finish(r);
    const double ms = MsSince(t);
    gave_up = true;
    late.join();
    Expect(!taken && !wrote.load() && slot == 0, "a late answer writes nothing");
    Expect(ms < 80, "the sampler returns after its own wait (" + std::to_string(ms) + " ms)");
  }
  {
    // A racing handler: each request is either answered once or cancelled, never both.
    Ticket ticket;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> handler_answers{0};
    std::atomic<uint64_t> slot{0};
    std::thread handler([&] {
      uint64_t claimed;
      while (!stop.load(std::memory_order_relaxed)) {
        if (ticket.Claim(5, claimed)) {
          slot.store(claimed >> 2, std::memory_order_relaxed);
          handler_answers.fetch_add(1, std::memory_order_relaxed);
          ticket.Done(claimed);
        }
      }
    });
    uint64_t taken = 0, wrong = 0;
    for (int n = 0; n < 20000; ++n) {
      const uint64_t r = ticket.Ask(5);
      for (int spin = 0; spin < (n % 7) * 20 && !ticket.Answered(r); ++spin) std::this_thread::yield();
      if (ticket.Finish(r)) {
        ++taken;
        if (slot.load(std::memory_order_relaxed) != r) ++wrong;
      }
    }
    stop = true;
    handler.join();
    std::printf("racing handler: %llu of 20000 requests answered, %llu cancelled\n",
                (unsigned long long)taken, (unsigned long long)(20000 - taken));
    Expect(taken == handler_answers.load(), "every answer is read once, none lost (" + std::to_string(taken) +
                                                " read, " + std::to_string(handler_answers.load()) + " answered)");
    Expect(wrong == 0, "each answer is read for its own round");
  }
}

#if defined(__linux__)
Ticket signal_ticket;
std::atomic<int> signal_slot{0};
std::atomic<int> late_claims{0};
void Handler(int) {
  uint64_t claimed;
  if (signal_ticket.Claim((uintptr_t)pthread_self(), claimed)) {
    signal_slot.store(1);
    signal_ticket.Done(claimed);
  } else {
    late_claims.fetch_add(1);
  }
}
void CheckSignals() {
  struct sigaction action {};
  action.sa_handler = Handler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGUSR2, &action, nullptr);
  std::atomic<bool> ready{false}, done{false};
  pthread_t target{};
  std::thread slow([&] {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);  // a thread that takes the signal late
    target = pthread_self();
    ready = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    while (!done.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  while (!ready.load()) std::this_thread::yield();
  const auto t = Clock::now();
  const uint64_t r = signal_ticket.Ask((uintptr_t)target);
  pthread_kill(target, SIGUSR2);
  for (int wait = 0; wait < 400 && !signal_ticket.Answered(r); ++wait) usleep(50);
  const bool taken = signal_ticket.Finish(r);
  const double ms = MsSince(t);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  done = true;
  slow.join();
  Expect(!taken, "a thread blocking the signal does not answer in time");
  Expect(ms < 100, "the request returns after its own wait, not 100 ms more (" + std::to_string(ms) + " ms)");
  Expect(signal_slot.load() == 0 && late_claims.load() == 1, "the late handler ran and claimed nothing");
}
#endif

void CheckTree() {
  const std::string cr = Read("platform/ps5/crash_report.cpp");
  const size_t fn = cr.find("bool SampleThread(void* thread, ThreadSample* out) {");
  const size_t fn_end = cr.find("void ProbeThread(void* thread, const char* name) {", fn);
  const std::string sample = fn != std::string::npos && fn_end != std::string::npos ? cr.substr(fn, fn_end - fn) : "";
  Expect(cr.find("#include \"xbox360ps5/sample_ticket.hpp\"") != std::string::npos, "the sampler includes the ticket");
  Expect(sample.find("sample_ticket_.Ask(") != std::string::npos &&
             sample.find("sample_ticket_.Finish(round)") != std::string::npos &&
             sample.find("*out = sample_slot;") != std::string::npos,
         "SampleThread asks, waits, finishes and copies the slot");
  Expect(sample.find("usleep(1000)") == std::string::npos && sample.find("sample_out") == std::string::npos,
         "the second wait and the pointer handed to the handler are gone");
  Expect(cr.find("if (sample_ticket_.Claim(self, claimed)) {") != std::string::npos &&
             cr.find("sample_ticket_.Done(claimed);") != std::string::npos,
         "the handler answers only its own request");
  Expect(cr.find("if (where_target.load() != self || !where_wanted.exchange(false)) return;") != std::string::npos,
         "a late answer prints no WHERE line");
  const size_t probe = cr.find("void ProbeThread(void* thread, const char* name) {");
  Expect(probe != std::string::npos && cr.find("where_wanted = true;", probe) != std::string::npos &&
             cr.find("where_target = reinterpret_cast<uintptr_t>(thread);", probe) != std::string::npos,
         "ProbeThread names the thread it asks");
}

}  // namespace

int main(int argc, char** argv) {
  CheckTicket();
  CheckThreads();
#if defined(__linux__)
  CheckSignals();
#endif
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-sample-ticket: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-sample-ticket: ok\n");
  return 0;
}
