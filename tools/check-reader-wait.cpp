// SPDX-License-Identifier: MIT
// Fix 157: a CPU read of a resolve copy that fix 142i has queued waits on the reading thread for
// that copy (xenia/gpu/vulkan/reader_wait.h), with the global lock let go, at most
// readback_reader_wait_ms; off by default, "reader-wait on" in readback-deliver.txt.
//  The real header, with threads standing in for the reading thread and the GPU thread:
//  (a) a read whose copy is already in memory (or another range's) does not wait;
//  (b) the wait ends when the GPU thread puts the copy in memory, which it can do only with the
//      reader's lock let go; the reader then sees the copy's bytes;
//  (c) a read on a page with two queued copies waits for both;
//  (d) with no delivery, the wait ends at the timeout (counted, "no look from the GPU thread");
//  (e) the deadlock case: the GPU thread waits for the guest (WAIT_REG_MEM) and only goes on once
//      the reader has read: the timeout breaks it (counted as "while the GPU thread waited for
//      the guest"), or, as in the build, the GPU thread puts the copy in memory from that wait;
//  (f) a lock held twice is still held during the wait: the GPU thread cannot answer, the timeout
//      ends it, and the GPU thread goes on after;
//  (g) the board: full (counted), an old id does not clear a reused slot, the 30 s counts.
//  The assembled tree: the read waits before fix 91 and fix 142i note it, not on the GPU thread,
//  with the global lock let go; both queue paths publish (fix 59's late copy also armed); every
//  copy leaving fix 142i's queue leaves the board; the GPU thread serves a waiting reader at each
//  draw, at WAIT_REG_MEM and in the idle wait, ending a submission that holds one; the settings.
// Runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if __has_include("xenia/gpu/vulkan/reader_wait.h")
#include "xenia/gpu/vulkan/reader_wait.h"
#define HAVE_READER_WAIT 1
#endif

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

#ifdef HAVE_READER_WAIT
namespace rw = xe::gpu::vulkan::reader_wait;
using Clock = std::chrono::steady_clock;

uint64_t NowNs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}
void Pause() { std::this_thread::sleep_for(std::chrono::microseconds(50)); }
// Waits for a condition, at most 5 s.
template <typename F>
bool Within5s(F f) {
  const auto until = Clock::now() + std::chrono::seconds(5);
  while (!f()) {
    if (Clock::now() > until) return false;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return true;
}

void CheckNoWait() {
  // (a)
  rw::Board board;
  int unlocks = 0, relocks = 0;
  auto unlock = [&] { ++unlocks; };
  auto relock = [&] { ++relocks; };
  const int slot = rw::Publish(board, 7, 0x1F0E8000, 8192);
  rw::Retire(board, slot, 7);
  rw::Result r = rw::Wait(board, 0x1F0E8000, 16384, NowNs, Pause, unlock, relock);
  Expect(r.outcome == rw::Outcome::kNothingQueued && !unlocks && !relocks && !r.waited_ns,
         "(a) a read whose copy is already in memory does not wait or let the lock go");
  rw::Publish(board, 8, 0x1F0F0000, 8192);
  r = rw::Wait(board, 0x1F0E8000, 16384, NowNs, Pause, unlock, relock);
  Expect(r.outcome == rw::Outcome::kNothingQueued && !unlocks,
         "(a) a read of another range does not wait");
  rw::Count(board, r);
  Expect(board.nothing_queued.load() == 1, "(a) counted as found in memory");
}

void CheckDelivered() {
  // (b) The reader holds the "global lock"; the GPU thread needs it to put the copy in memory.
  rw::Board board;
  board.timeout_ns = 5000000000ull;
  std::recursive_mutex global;
  uint8_t guest[8192] = {};
  const int slot = rw::Publish(board, 11, 0x1F0EC000, 8192);
  std::atomic<bool> gpu_done{false};
  std::thread gpu([&] {
    Within5s([&] { return rw::ReaderWaiting(board); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::lock_guard<std::recursive_mutex> lock(global);  // only with the reader's lock let go
    board.gpu_polls.fetch_add(1);
    std::memset(guest, 0xAB, sizeof(guest));
    rw::Retire(board, slot, 11);
    gpu_done = true;
  });
  global.lock();
  int unlocks = 0, relocks = 0;
  const rw::Result r = rw::Wait(
      board, 0x1F0EC000, 16384, NowNs, Pause, [&] { ++unlocks; global.unlock(); },
      [&] { ++relocks; global.lock(); });
  const bool bytes = guest[0] == 0xAB && guest[8191] == 0xAB;
  global.unlock();
  gpu.join();
  Expect(r.outcome == rw::Outcome::kDelivered && gpu_done && bytes,
         "(b) the wait ends once the GPU thread has put the copy in memory, and the reader sees "
         "its bytes");
  Expect(unlocks == 1 && relocks == 1 && r.gpu_answered && !rw::ReaderWaiting(board),
         "(b) the lock let go once and taken back once; the GPU thread answered");
  Expect(r.waited_ns >= 15000000 && r.waited_ns < 4000000000ull,
         "(b) the reader waited for the delivery, not for the timeout");
  rw::Count(board, r);
  Expect(board.delivered.load() == 1 && board.timeouts.load() == 0 &&
             board.waited_max_ns.load() == r.waited_ns,
         "(b) counted as a wait until the copy was in memory, with its time");
}

void CheckTwoCopies() {
  // (c) Two 8 KiB keys on one 16 KiB page: the read waits for both.
  rw::Board board;
  board.timeout_ns = 5000000000ull;
  const int a = rw::Publish(board, 21, 0x1F0E8000, 8192);
  const int b = rw::Publish(board, 22, 0x1F0EA000, 8192);
  std::atomic<int> stage{0};
  std::thread gpu([&] {
    Within5s([&] { return rw::ReaderWaiting(board); });
    rw::Retire(board, a, 21);
    stage = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stage = 2;
    rw::Retire(board, b, 22);
  });
  const rw::Result r = rw::Wait(board, 0x1F0E8000, 16384, NowNs, Pause, [] {}, [] {});
  const int seen = stage.load();
  gpu.join();
  Expect(r.outcome == rw::Outcome::kDelivered && seen == 2,
         "(c) a read on a page with two queued copies waits for both");
}

void CheckTimeout() {
  // (d)
  rw::Board board;
  board.timeout_ns = 30000000;
  rw::Publish(board, 31, 0x0BFB4000, 65536);
  const rw::Result r = rw::Wait(board, 0x0BFB8000, 16384, NowNs, Pause, [] {}, [] {});
  Expect(r.outcome == rw::Outcome::kTimeout && r.waited_ns >= 30000000 &&
             r.waited_ns < 2000000000ull && !r.gpu_answered,
         "(d) with no delivery the wait ends at the timeout");
  rw::Count(board, r);
  Expect(board.timeouts.load() == 1 && board.timeouts_silent.load() == 1 &&
             board.timeouts_guest_wait.load() == 0 && board.delivered.load() == 0,
         "(d) counted as a timeout with no look at the board from the GPU thread");
}

void CheckDeadlock() {
  // (e) The GPU thread waits for a guest write that the reading thread makes only after its read.
  for (const bool serve_from_wait : {false, true}) {
    rw::Board board;
    board.timeout_ns = 50000000;
    const int slot = rw::Publish(board, 41, 0x1F0E8000, 8192);
    std::atomic<bool> guest_wrote{false}, gpu_done{false};
    std::thread gpu([&] {
      bool delivered = false;
      while (!guest_wrote) {  // WAIT_REG_MEM
        board.gpu_guest_wait_ns.store(NowNs());  // as PrepareForWait / GuestWaitPoll
        if (serve_from_wait && rw::ReaderWaiting(board) && !delivered) {
          board.gpu_polls.fetch_add(1);
          rw::Retire(board, slot, 41);
          delivered = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      if (!delivered) rw::Retire(board, slot, 41);  // the packets after the wait
      gpu_done = true;
    });
    const rw::Result r = rw::Wait(board, 0x1F0E8000, 16384, NowNs, Pause, [] {}, [] {});
    guest_wrote = true;  // the read went on, and the guest thread writes what the GPU waits for
    const bool finished = Within5s([&] { return gpu_done.load(); });
    gpu.join();
    rw::Count(board, r);
    if (!serve_from_wait) {
      Expect(r.outcome == rw::Outcome::kTimeout && r.gpu_guest_wait && finished,
             "(e) the GPU thread waiting for the reader: the timeout breaks it, both go on");
      Expect(board.timeouts_guest_wait.load() == 1,
             "(e) counted as a timeout while the GPU thread waited for the guest");
    } else {
      Expect(r.outcome == rw::Outcome::kDelivered && r.waited_ns < 50000000 && finished,
             "(e) served from the GPU thread's wait for the guest, the read gets its copy");
    }
  }
}

void CheckLockHeldTwice() {
  // (f) The lock held twice: letting it go once keeps it; the GPU thread cannot answer.
  rw::Board board;
  board.timeout_ns = 40000000;
  std::recursive_mutex global;
  const int slot = rw::Publish(board, 51, 0x045BF000, 8192);
  std::atomic<bool> gpu_done{false};
  std::thread gpu([&] {
    Within5s([&] { return rw::ReaderWaiting(board); });
    std::lock_guard<std::recursive_mutex> lock(global);
    board.gpu_polls.fetch_add(1);
    rw::Retire(board, slot, 51);
    gpu_done = true;
  });
  global.lock();
  global.lock();
  const rw::Result r = rw::Wait(board, 0x045BC000, 16384, NowNs, Pause, [&] { global.unlock(); },
                                [&] { global.lock(); });
  const bool done_before = gpu_done.load();
  global.unlock();
  global.unlock();
  const bool finished = Within5s([&] { return gpu_done.load(); });
  gpu.join();
  Expect(r.outcome == rw::Outcome::kTimeout && !r.gpu_answered && !done_before && finished,
         "(f) a lock held twice: the timeout ends the wait, and the GPU thread goes on after");
}

void CheckBoard() {
  // (g)
  rw::Board board;
  int first = -1;
  for (uint32_t i = 0; i < rw::kSlots; ++i) {
    const int slot = rw::Publish(board, 100 + i, 0x10000000 + i * 0x1000, 0x1000);
    if (!i) first = slot;
    Expect(slot >= 0, "(g) a free slot for each copy while there is one");
  }
  Expect(rw::Publish(board, 999, 0x20000000, 0x1000) == -1 && board.not_on_board.load() == 1,
         "(g) the board full: not published, counted");
  rw::Retire(board, first, 100);
  const int reused = rw::Publish(board, 1000, 0x30000000, 0x1000);
  Expect(reused == first, "(g) a retired slot is used again");
  rw::Retire(board, reused, 100);  // an old id
  Expect(board.ids[reused].load() == 1000, "(g) an old id does not clear a reused slot");
  rw::Board counts;
  rw::Result a;
  a.outcome = rw::Outcome::kDelivered;
  a.waited_ns = 2000000;
  rw::Result b;
  b.outcome = rw::Outcome::kTimeout;
  b.waited_ns = 20000000;
  b.gpu_answered = false;
  rw::Count(counts, a);
  rw::Count(counts, b);
  Expect(counts.delivered.load() == 1 && counts.timeouts.load() == 1 &&
             counts.waited_ns.load() == 22000000 && counts.waited_max_ns.load() == 20000000 &&
             counts.timeouts_silent.load() == 1,
         "(g) the 30 s counts: waits, their time and the longest");
}
#endif

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
bool Has(const std::string& text, const std::string& part) {
  const bool found = text.find(part) != std::string::npos;
  if (!found) std::printf("  (not found: %s)\n", part.substr(0, 100).c_str());
  return found;
}
std::string Body(const std::string& text, const std::string& signature) {
  const size_t at = text.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t end = text.find("\n}\n", at);
  return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

void CheckTree() {
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string base = Read(".deps/xenia-canary/src/xenia/gpu/command_processor.cc");
  Expect(!cp.empty() && !base.empty(), "the tree is there (run from the assembled tree's root)");
  // The read.
  const std::string callback = Body(cp, "void VulkanCommandProcessor::ReadbackReadCallback(");
  const size_t wait = callback.find("reader_wait::Wait(");
  Expect(wait != std::string::npos &&
             wait < callback.find("self->readback_read_fires_.fetch_add(1") &&
             wait < callback.find("self->readback_reads_.Push(physical_address, length)") &&
             Has(callback, "if (fix157::board.enabled.load(std::memory_order_relaxed)) {") &&
             Has(callback, "if (fix157::gpu_thread) {") &&
             Has(callback, "[] { xe::global_critical_region::mutex().unlock(); },") &&
             Has(callback, "[] { xe::global_critical_region::mutex().lock(); });") &&
             Has(callback, "reader_wait::Count(fix157::board, result);"),
         "the read waits (lock let go, not on the GPU thread) before fix 91 and 142i note it");
  // The queue.
  Expect(Has(cp, "                                    written_address, readback_length, frame_current_, true});\n"
                 "        fix157::Publish(fix142i::pending.back());  // fix 157") &&
             Has(cp, "            fix157::Publish(fix142i::pending.back());\n"
                     "            memory_->EnablePhysicalMemoryAccessCallbacks(\n"
                     "                written_address, readback_length, false, true);  // fix 157"),
         "both queue paths publish; fix 59's late copy gets a read watch too");
  const std::string deliver = Body(cp, "void VulkanCommandProcessor::Fix142iDeliver() {");
  const size_t keep = deliver.find("fix142i::pending[kept++] = p;");
  const size_t off = deliver.find("~OffTheBoard() { reader_wait::Retire(fix157::board, p.board_slot, p.board_id); }");
  Expect(keep != std::string::npos && off != std::string::npos && keep < off &&
             off < deliver.find("auto it = readback_buffers_.find(p.key);"),
         "every copy leaving fix 142i's queue (in memory or dropped) leaves the board");
  // The GPU thread.
  const std::string draw = Body(cp, "bool VulkanCommandProcessor::IssueDraw(");
  Expect(Has(draw, "if (reader_wait::ReaderWaiting(fix157::board)) Fix157Serve(true);  // fix 157"),
         "a waiting reader served at each draw");
  const std::string serve = Body(cp, "void VulkanCommandProcessor::Fix157Serve(bool may_end_submission) {");
  Expect(Has(serve, "if (now - fix157::last_serve_ns < 100000) return;") &&
             Has(serve, "if (open && CanEndSubmissionImmediately()) {") &&
             serve.find("EndSubmission(false);") < serve.find("Fix142iDeliver();") &&
             Has(serve, "open |= p.board_slot >= 0 && p.submission >= current;"),
         "served at most every 100 us: a submission holding a queued copy ended, the made copies "
         "put in memory");
  const std::string prepare = Body(cp, "void VulkanCommandProcessor::PrepareForWait() {");
  const std::string idle = Body(cp, "void VulkanCommandProcessor::GuestWaitPoll() {");
  Expect(Has(prepare, "CommandProcessor::PrepareForWait();") &&
             Has(prepare, "if (reader_wait::ReaderWaiting(fix157::board)) Fix157Serve(true);") &&
             Has(idle, "if (reader_wait::ReaderWaiting(fix157::board)) Fix157Serve(true);") &&
             Has(base, "        GuestWaitPoll();  // fix 157\n#endif\n        loop_count++;"),
         "served while the GPU thread waits for the guest: WAIT_REG_MEM and each idle round");
  // The settings and the line.
  Expect(Has(cp, "DEFINE_bool(readback_reader_wait, false,") &&
             Has(cp, "DEFINE_uint32(readback_reader_wait_ms, 20,") &&
             Has(cp, "} else if (w == \"reader-wait\" && (value == \"on\" || value == \"off\")) {  // fix 157") &&
             Has(cp, "} else if (w == \"reader-wait-ms\" && !value.empty() &&") &&
             Has(cp, "std::clamp<uint32_t>(uint32_t(std::strtoul(value.c_str(), nullptr, 10)), 1, 1000);") &&
             Has(cp, "XELOGW(\"Reader wait (fix 157): {}, at most {} ms; from {}\"") &&
             Has(cp, "XELOGW(\"Reader wait, last 30 s (fix 157, {}, at most {} ms): {} reads found their copy \""),
         "off by default, 20 ms; reader-wait and reader-wait-ms in readback-deliver.txt; the lines");
}

}  // namespace

int main() {
#ifdef HAVE_READER_WAIT
  CheckNoWait();
  CheckDelivered();
  CheckTwoCopies();
  CheckTimeout();
  CheckDeadlock();
  CheckLockHeldTwice();
  CheckBoard();
  CheckTree();
#else
  std::printf("FAIL: xenia/gpu/vulkan/reader_wait.h is not in the tree (fix 157)\n");
  ++failures;
#endif
  if (failures) {
    std::printf("check-reader-wait: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-reader-wait: ok\n");
  return 0;
}
