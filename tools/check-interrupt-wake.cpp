// SPDX-License-Identifier: MIT
// Fix 124 (CP wake on interrupt; xenia/gpu/interrupt_wake.h): the GPU command thread's memory
// waits (WAIT_REG_MEM on memory, fix 83's 100 us polls) end right after a guest interrupt callback
// returns. The Sims 3 (454108E2) makes the GPU wait every frame for a word that only its vblank
// callback writes (the flip: SCRATCH_REG1's writeback, cleared at the queued flip's vblank); with
// 1.0.5-work-fix115b and Optimized video memory on, WAIT_REG_MEM slept 531 ms/s in 497 sleeps/s
// over 38 waits/s (research/sims3-boost).
//  (a) the waker: a notify before the wait is not lost, one during the wait ends it early, none
//      leaves it to the slice;
//  (b) a 60 Hz model (a "vblank" thread writes the word and returns from its callback, the
//      command thread waits for it): the time from the write to the wait's end, polling against
//      the wake (printed; the wake's average must stay small);
//  (c) the assembled tree: the setting (default on), the notify after each interrupt callback, the
//      slice ended by the waker or slept as before, the counters and their log line.
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
#include <vector>

#include "xenia/gpu/interrupt_wake.h"

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
using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

void CheckWaker() {
  using xe::gpu::interrupt_wake::Waker;
  {
    Waker waker;
    const uint64_t seen = waker.Seq();
    waker.Notify();  // before the wait: the count already differs
    const auto t = Clock::now();
    Expect(waker.WaitAfter(seen, std::chrono::milliseconds(500)), "a notify before the wait ends it");
    Expect(MsSince(t) < 100, "a notify before the wait ends it at once");
    Expect(waker.Seq() == seen + 1, "one notify, one count");
    Expect(waker.LastNs() != 0, "the notify's time is kept");
  }
  {
    Waker waker;
    const uint64_t seen = waker.Seq();
    const auto t = Clock::now();
    std::thread other([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      waker.Notify();
    });
    const bool woken = waker.WaitAfter(seen, std::chrono::seconds(5));
    const double ms = MsSince(t);
    other.join();
    Expect(woken, "a notify during the wait ends it");
    Expect(ms >= 20 && ms < 2000, "a notify during the wait ends it then, not at the slice (" +
                                      std::to_string(ms) + " ms)");
  }
  {
    Waker waker;
    const auto t = Clock::now();
    Expect(!waker.WaitAfter(waker.Seq(), std::chrono::milliseconds(20)), "no notify: the slice ends the wait");
    Expect(MsSince(t) >= 15, "no notify: the wait lasts the slice");
  }
}

// The 60 Hz model: returns the average and the largest time in ms from the callback's write to the
// command thread seeing the word, over `frames` flips.
struct Lateness {
  double average_ms, max_ms;
};
Lateness Model(bool wake, int frames) {
  xe::gpu::interrupt_wake::Waker waker;
  std::atomic<uint32_t> word{1};
  std::atomic<int64_t> written_ns{0};
  std::atomic<bool> done{false};
  std::thread vblank([&] {
    for (int f = 0; f < frames; ++f) {
      std::this_thread::sleep_for(std::chrono::microseconds(16667));
      written_ns.store(int64_t(xe::gpu::interrupt_wake::SteadyNs()), std::memory_order_relaxed);
      word.store(0, std::memory_order_release);  // the callback clears the flip word
      waker.Notify();                             // and returns
      while (word.load(std::memory_order_acquire) == 0 && !done.load()) std::this_thread::yield();
    }
  });
  double sum = 0, max = 0;
  for (int f = 0; f < frames; ++f) {
    uint64_t seen = waker.Seq();
    while (word.load(std::memory_order_acquire) != 0) {
      if (wake) {
        waker.WaitAfter(seen, std::chrono::microseconds(100));
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      seen = waker.Seq();
    }
    const double ms = double(int64_t(xe::gpu::interrupt_wake::SteadyNs()) - written_ns.load()) / 1e6;
    sum += ms;
    if (ms > max) max = ms;
    word.store(1, std::memory_order_release);  // the next frame's flip is queued
  }
  done = true;
  vblank.join();
  return {sum / frames, max};
}

void CheckModel() {
  const Lateness poll = Model(false, 60), wake = Model(true, 60);
  std::printf("60 Hz flip model, 60 frames: polling every 100 us %.3f ms late on average (max %.3f), "
              "woken by the callback %.3f ms (max %.3f)\n",
              poll.average_ms, poll.max_ms, wake.average_ms, wake.max_ms);
  Expect(wake.average_ms < 5.0, "woken by the callback, the wait ends soon after the write");
}

void CheckTree() {
  const std::string gpu = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string flags = Read(gpu + "gpu_flags.cc"), flags_h = Read(gpu + "gpu_flags.h");
  Expect(flags.find("DEFINE_bool(gpu_wake_on_interrupt, true,") != std::string::npos &&
             flags_h.find("DECLARE_bool(gpu_wake_on_interrupt);") != std::string::npos,
         "the setting, on by default");
  const std::string cp_h = Read(gpu + "command_processor.h");
  Expect(cp_h.find("#include \"xenia/gpu/interrupt_wake.h\"") != std::string::npos &&
             cp_h.find("void NotifyInterruptDispatched() { interrupt_waker_.Notify(); }") != std::string::npos &&
             cp_h.find("interrupt_wake::Waker interrupt_waker_;") != std::string::npos,
         "the command processor owns the waker");
  const std::string gs = Read(gpu + "graphics_system.cc");
  const size_t dpc = gs.find("kernel_state()->EmulateCPInterruptDPC(interrupt_callback_,");
  const size_t notify = gs.find("command_processor_->NotifyInterruptDispatched();");
  Expect(dpc != std::string::npos && notify != std::string::npos && notify > dpc && notify - dpc < 400,
         "the notify follows each interrupt callback");
  const std::string pm4 = Read(gpu + "pm4_command_processor_implement.h");
  const size_t fn = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_WAIT_REG_MEM(");
  const size_t fn_end = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_REG_RMW(", fn);
  const std::string body = fn != std::string::npos && fn_end != std::string::npos ? pm4.substr(fn, fn_end - fn) : "";
  const size_t seq_read = body.find("wake_seen = interrupt_waker_.Seq();");
  const size_t word_read = body.find("const uint32_t now_value = xenos::GpuSwap(");
  Expect(body.find("interrupt_waker_.WaitAfter(wake_seen,") != std::string::npos &&
             body.find("std::chrono::microseconds(100))) {") != std::string::npos,
         "the 100 us slice ends early on an interrupt");
  Expect(body.find("xe::threading::Sleep(std::chrono::microseconds(100));") != std::string::npos,
         "with the setting off, the slice is slept as before");
  Expect(seq_read != std::string::npos && word_read != std::string::npos && seq_read < word_read,
         "the interrupt count is read before the word (a callback between them is not missed)");
  Expect(body.find("xbox360ps5::frame_cost::NoteInterruptWake(") != std::string::npos, "the waits are counted");
  const std::string fc = Read("include/xbox360ps5/frame_cost.hpp");
  Expect(fc.find("class InterruptWakeReporter") != std::string::npos &&
             fc.find("Performance interrupt wake (GPU thread, fix 124, %s)") != std::string::npos,
         "the counters' line");
  const std::string game_main = Read("src/canary_game_main.cpp");
  Expect(game_main.find("frame_cost_interrupt_wake.Line(double(summary_samples))") != std::string::npos,
         "the line is logged with the frame time lines");
}

}  // namespace

int main(int argc, char** argv) {
  CheckWaker();
  CheckModel();
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-interrupt-wake: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-interrupt-wake: ok\n");
  return 0;
}
