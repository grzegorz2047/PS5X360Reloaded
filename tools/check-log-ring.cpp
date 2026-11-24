// SPDX-License-Identifier: MIT
// Fix 154 (tools/source-1.0.2/fixes/154-log-ring-no-wait.patch): a full log ring never makes a
// thread that logs wait, and the Kinect copy-command-2 warning no longer floods the log.
//
// 1.0.5 at "Resolution scale (test)" 2x (issues #64, #65): every kConvertTo1111 resolve of a
// Kinect title's body tracking logged "Resolve copy command 2 is not supported with resolution
// scaling" (7542 of 9470 lines in Kinect Sports, 40806 of 43143 in Puss in Boots). The core
// logger's writer drained about 20 lines a second, its 32768-block ring filled after 170-330 s,
// and the GPU and UI threads then spun in the disruptor's claim(): 0-2 frames a second.
// Checked:
//  (a) log_ring::Budget: reservations up to the capacity, never past it, given back by Release;
//      a line larger than the whole ring never fits; many threads at once never exceed it.
//  (b) DropNote: a note when the dropped total grew, at most once a second.
//  (c) With the core's own disruptor (multi_threaded_claim_strategy, as logging.cc uses it):
//      the old way, claim() on a full ring does not return while the writer stands still;
//      the fix's way, with a stalled writer, producers on four threads all return at once
//      (each append well under 50 ms), lines that do not fit are counted as dropped, and once
//      the writer runs again it reads every line that went in, whole and in each thread's
//      order, in batches of several lines; appended + dropped = attempted.
//  (d) hang_watch's beat shows the dropped lines next to the appended and written ones.
//  (e) the assembled tree: logging.cc reserves before claim() and drops instead of waiting
//      (only the terminator waits), the writer gives blocks back after publishing them as
//      consumed, reads every published line in one pass and flushes once per pass, and notes
//      dropped lines; the copy-command-2 warning (Vulkan and D3D12) is logged the first 4
//      times and then every 4096th with a count, and still skips the copy at 2x.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if __has_include("xbox360ps5/log_ring.hpp")
#include "third_party/disruptorplus/include/disruptorplus/multi_threaded_claim_strategy.hpp"
#include "third_party/disruptorplus/include/disruptorplus/sequence_barrier.hpp"
#include "third_party/disruptorplus/include/disruptorplus/spin_wait_strategy.hpp"
#include "xbox360ps5/hang_watch.hpp"
#include "xbox360ps5/log_ring.hpp"
#define HAVE_FIX154 1
#endif

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
  std::ostringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}

bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

// The text from `from` to the first `to` after it (empty when either is missing).
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  if (b == std::string::npos) return {};
  return text.substr(a, b - a);
}

bool InOrder(const std::string& text, const std::vector<std::string>& parts) {
  size_t at = 0;
  for (const std::string& part : parts) {
    at = text.find(part, at);
    if (at == std::string::npos) return false;
    at += part.size();
  }
  return true;
}

#ifdef HAVE_FIX154
namespace dp = disruptorplus;
namespace lr = xbox360ps5::log_ring;
namespace hw = xbox360ps5::hang_watch;
using Clock = std::chrono::steady_clock;

void CheckBudget() {
  lr::Budget budget(64);
  Expect(budget.capacity() == 64 && budget.used() == 0, "budget: starts empty");
  Expect(budget.TryReserve(40), "budget: 40 of 64 fit");
  Expect(budget.TryReserve(24), "budget: the last 24 fit exactly");
  Expect(!budget.TryReserve(1), "budget: nothing more fits in a full ring");
  Expect(budget.used() == 64, "budget: a failed reservation takes nothing");
  budget.Release(24);
  Expect(!budget.TryReserve(25), "budget: 25 do not fit in 24 free");
  Expect(budget.TryReserve(24), "budget: 24 fit again after the writer gave them back");
  budget.Release(64);
  Expect(budget.used() == 0, "budget: all given back");
  Expect(!budget.TryReserve(65), "budget: a line larger than the ring never fits");
  Expect(budget.used() == 0, "budget: and takes nothing");

  // Many threads at once never hold more than the capacity.
  lr::Budget shared(1000);
  std::atomic<bool> over{false};
  std::atomic<uint64_t> held_total{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int n = 0; n < 20000; ++n) {
        const size_t count = 1 + size_t((n * 7 + t) % 9);
        if (shared.TryReserve(count)) {
          if (shared.used() > shared.capacity()) over = true;
          held_total += count;
          shared.Release(count);
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  Expect(!over, "budget: four threads never hold more than the capacity");
  Expect(shared.used() == 0 && held_total > 0, "budget: four threads give everything back");
}

void CheckDropNote() {
  lr::DropNote note;
  Expect(!note.Due(0, 100), "note: nothing dropped, no note");
  Expect(note.Due(3, 100) && note.noted == 3, "note: the first drop is noted at once");
  Expect(!note.Due(9, 600), "note: not again within a second");
  Expect(note.Due(9, 1100) && note.noted == 9, "note: the grown total a second later");
  Expect(!note.Due(9, 5000), "note: no note while the total stands still");
}

// A ring of kBlocks blocks of 16 bytes, filled the way logging.cc fills its ring: a claim of
// a line's blocks, its blocks written, then publish; one writer reads published blocks, then
// publishes them as consumed (and with the fix gives them back to the budget).
constexpr size_t kBlocks = 256;
struct Block {
  uint32_t thread = 0;
  uint32_t serial = 0;
  uint32_t count = 0;  // blocks of the line, in its first block
  uint32_t part = 0;   // 0 for the first block
};

struct Ring {
  dp::spin_wait_strategy wait;
  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim{kBlocks, wait};
  dp::sequence_barrier<dp::spin_wait_strategy> consumed{wait};
  lr::Budget budget{kBlocks};
  Block blocks[kBlocks];
  Ring() { claim.add_claim_barrier(consumed); }
};

// The fix's AppendLine: false when the line was dropped.
bool Append(Ring& ring, uint32_t thread, uint32_t serial, size_t count) {
  if (!ring.budget.TryReserve(count)) return false;
  const dp::sequence_range range = ring.claim.claim(count);
  for (size_t n = 0; n < count; ++n) {
    Block& block = ring.blocks[(range.first() + n) % kBlocks];
    block = {thread, serial, uint32_t(count), uint32_t(n)};
  }
  ring.claim.publish(range);
  return true;
}

struct Reader {
  dp::sequence_t next = 0;
  dp::sequence_t last = dp::sequence_t(-1);
  std::vector<uint32_t> last_serial = std::vector<uint32_t>(8, 0);
  uint64_t lines = 0, passes = 0, most_in_a_pass = 0;
  bool broken = false;

  // One pass of the fix's writer: every line published so far (at least `desired` blocks).
  void Pass(Ring& ring) {
    size_t desired = 1;
    while (true) {
      const dp::sequence_t published = ring.claim.wait_until_published(next + dp::sequence_t(desired) - 1, last);
      const size_t ready = size_t(dp::difference(published, next)) + 1;
      size_t read = 0, in_pass = 0;
      while (read < ready) {
        const Block& head = ring.blocks[(next + read) % kBlocks];
        if (head.part != 0 || head.count == 0) { broken = true; return; }
        if (read + head.count > ready) break;  // the rest of this line is not published yet
        for (uint32_t n = 0; n < head.count; ++n) {
          const Block& block = ring.blocks[(next + read + n) % kBlocks];
          if (block.thread != head.thread || block.serial != head.serial || block.part != n) broken = true;
        }
        if (head.thread < last_serial.size()) {
          if (head.serial <= last_serial[head.thread]) broken = true;  // each thread's lines in order
          last_serial[head.thread] = head.serial;
        }
        read += head.count;
        ++in_pass;
      }
      if (!read) { desired = ring.blocks[next % kBlocks].count; continue; }
      next += dp::sequence_t(read);
      last = next - 1;
      ring.consumed.publish(last);
      ring.budget.Release(read);
      lines += in_pass;
      ++passes;
      if (in_pass > most_in_a_pass) most_in_a_pass = in_pass;
      return;
    }
  }
};

void CheckOldClaimWaits() {
  // 1.0.5: claim() with the ring full and the writer standing still does not return.
  dp::spin_wait_strategy wait;
  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim(16, wait);
  dp::sequence_barrier<dp::spin_wait_strategy> consumed(wait);
  claim.add_claim_barrier(consumed);
  claim.publish(claim.claim(16));
  std::atomic<bool> returned{false};
  std::thread logger([&] {
    claim.publish(claim.claim(1));
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  Expect(!returned, "old way: claim() on a full ring waits for the writer (the 1.0.5 stall)");
  consumed.publish(0);  // the writer frees one block: the logger goes on
  logger.join();
  Expect(returned, "old way: claim() returns once the writer frees a block");
}

void CheckNoWait() {
  // Four threads log for 400 ms; the writer stands still for the first 150 ms (the ring fills),
  // then reads in passes, as logging.cc's writer does.
  Ring ring;
  constexpr int kThreads = 4;
  std::atomic<uint64_t> attempted{0}, appended{0}, dropped{0}, dropped_while_stalled{0};
  std::atomic<int64_t> slowest_us{0};
  std::atomic<bool> writer_runs{false}, producing{true}, stop{false};
  Reader reader;
  std::thread writer([&] {
    while (!stop) {
      if (!writer_runs || ring.budget.used() == 0) { std::this_thread::yield(); continue; }
      reader.Pass(ring);
    }
  });
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (uint32_t serial = 1; producing; ++serial) {
        const auto begin = Clock::now();
        ++attempted;
        if (Append(ring, uint32_t(t), serial, 1 + (serial % 3))) {
          ++appended;
        } else {
          ++dropped;
          if (!writer_runs) ++dropped_while_stalled;
        }
        const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
        int64_t seen = slowest_us.load();
        while (us > seen && !slowest_us.compare_exchange_weak(seen, us)) {}
        if (!(serial % 64)) std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  writer_runs = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  producing = false;
  for (auto& thread : threads) thread.join();
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (ring.budget.used() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  stop = true;
  writer.join();
  std::printf("no-wait: %llu attempted, %llu appended, %llu dropped (%llu with the writer stalled), slowest append "
              "%lld us, %llu passes, up to %llu lines a pass\n",
              (unsigned long long)attempted.load(), (unsigned long long)appended.load(),
              (unsigned long long)dropped.load(), (unsigned long long)dropped_while_stalled.load(),
              (long long)slowest_us.load(), (unsigned long long)reader.passes,
              (unsigned long long)reader.most_in_a_pass);
  Expect(appended + dropped == attempted, "no-wait: every line either went in or was counted as dropped");
  Expect(dropped_while_stalled > 0, "no-wait: with the writer stalled, lines that did not fit were dropped");
  Expect(appended > kBlocks, "no-wait: once the writer runs again, lines go in again");
  Expect(slowest_us < 50000, "no-wait: no append waited for the stalled writer (under 50 ms each)");
  Expect(!reader.broken, "no-wait: every line read whole, each thread's lines in order");
  Expect(reader.lines == appended, "no-wait: the writer read every line that went in");
  Expect(ring.budget.used() == 0, "no-wait: every block given back");
  Expect(reader.passes > 1 && reader.most_in_a_pass > 1, "no-wait: the writer reads several lines in one pass");
}

void CheckBeat() {
  hw::BeatInput in{};
  in.now_ms = 3000;
  in.start_ms = 1000;
  hw::log_appended = 120;
  hw::log_written = 95;
  hw::log_dropped = 7;
  const std::string line = hw::HeartbeatLine(in);
  Expect(Has(line, "log 120/95 dropped 7 calls:"), "beat: appended/written and the dropped lines: " + line);
  hw::log_appended = 0;
  hw::log_written = 0;
  hw::log_dropped = 0;
}
#endif

void CheckTree() {
  const std::string logging = Read(".deps/xenia-canary/src/xenia/base/logging.cc");
  const std::string append = Between(logging, "  void AppendLine(uint32_t thread_id", "\n  }\n");
  Expect(InOrder(append, {"budget_.TryReserve(count)", "log_dropped.fetch_add(", "return;",
                          "claim_strategy_.claim(count)", "claim_strategy_.publish(range)"}),
         "logging.cc: AppendLine reserves first and drops a line that does not fit, then claims");
  Expect(InOrder(append, {"if (!terminate)", "while (!budget_.TryReserve(count))"}),
         "logging.cc: only the terminator waits for room");
  Expect(Has(logging, "xbox360ps5::log_ring::Budget budget_{kBlockCount};"), "logging.cc: the budget is the whole ring");
  const std::string writer = Between(logging, "  void WriteThread() {", "\n public:");
  Expect(InOrder(writer, {"claim_strategy_.wait_until_published(", "kBatchBlocks", "consumed_.publish(last_sequence);",
                          "budget_.Release(read_count);", "FlushAllSinks();"}),
         "logging.cc: the writer takes every published line in one pass, gives blocks back after consuming them, "
         "and flushes once a pass");
  Expect(Has(writer, "drop_note.Due(") && Has(writer, "lines dropped"), "logging.cc: the writer notes dropped lines");
  Expect(Has(logging, "#include \"xbox360ps5/log_ring.hpp\""), "logging.cc: includes the fix's header");

  for (const char* path : {".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc",
                           ".deps/xenia-canary/src/xenia/gpu/d3d12/d3d12_render_target_cache.cc"}) {
    const std::string text = Read(path);
    const std::string site = Between(text, "xenos::CopyCommand::kConvertTo1111) {", "resolve_info.copy_dest_extent_length = 0;");
    Expect(InOrder(site, {"fetch_add(1", "count < 4 || !(count & 0xFFF)",
                          "\"Resolve copy command 2 is not supported with resolution scaling ({} \"", "\"so far)\"", "count + 1"}),
           std::string(path) + ": the copy-command-2 warning the first 4 times, then every 4096th, with a count");
    Expect(!site.empty(), std::string(path) + ": the copy is still skipped at 2x (unchanged)");
  }
}

}  // namespace

int main() {
#ifdef HAVE_FIX154
  CheckBudget();
  CheckDropNote();
  CheckOldClaimWaits();
  CheckNoWait();
  CheckBeat();
  CheckTree();
#else
  std::printf("skip: xbox360ps5/log_ring.hpp is not in this tree (fix 154 left out)\n");
#endif
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-log-ring: all passed\n");
  return 0;
}
