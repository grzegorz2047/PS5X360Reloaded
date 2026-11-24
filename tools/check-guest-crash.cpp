// SPDX-License-Identifier: MIT
// Fix 45 (tools/source-1.0.2/fixes/45-guest-crash-notice.patch): a guest crash no longer ends in
// a silent picture at 0 FPS (Minecraft, 584111F7, on the v1.0.3 draft: thread F8000104 read guest
// address 0 at PC 824F849C, Emulator::ExceptionCallback called Pause() and Pause() never returned;
// research/minecraft-online-hang/VERDICT.md).
//  (a) port/guest_crash.hpp: the first crash is recorded (pc, thread, fault address, time) and
//      later ones only counted, also from threads at once; the STALL report's first line names it
//      ("STALL after a guest crash on thread F8000104 at PC 824F849C, 16 s ago; the emulator paused
//      itself ...") and is the line it always was when nothing crashed; the crashed thread's state
//      and the overlay's detail. In the assembled tree: the core declares the weak hook and calls
//      it in ExceptionCallback before it builds the dump and pauses, and leaves its own "Uh-oh!"
//      modal out when the port has the hook; canary_game_main.cpp defines it, logs the headline,
//      names the crashed thread in ReportStall, shows the notice and skips the Vulkan cache's exit
//      checkpoint (the paused GPU worker would hold the way out for 30 s).
//  (b) the overlay's two notices (English keys) are in every language file of
//      tools/source-1.0.2/english/lang (the argument), Polish translated.
//  (c) a model of XmaDecoder's pause handshake: a worker that sleeps on an auto-reset event and
//      checks paused_ after each pass over the contexts (signal pause_fence_, wait on
//      resume_fence_). With the fix, Pause() returns with the worker idle and no context kicked
//      (expected within 100 ms; at most a few of 50 rounds slower, none over 1 s: shared runners),
//      with the worker busy, and at once without a worker loop (xma_inline); the 1.0.2 Pause (no
//      Set) is still waiting after 300 ms in both cases and only a kick (or a signal nobody sends)
//      lets it go: the frozen crash handler. Whether the worker loop runs is decided once in
//      Setup: the port applies its settings again during a game, and a Pause that read the cvar
//      then (fix 45's draft) waited for a loop that never ran, or returned while one still
//      decoded. The tree's XmaDecoder::Setup, WriteRegister, Pause, WorkerThreadMain and Resume
//      are checked to be what the model runs.
//  (d) once Pause() returns, Emulator::Pause suspends every guest thread wherever it is (on the
//      PS5 a signal). A thread stopped while it holds the log's recent_mutex_ would block every
//      thread that logs next, the frame loop and its STALL report too. The model: a thread
//      holding a lock under fix 45's SuspensionDeferral is stopped only once it lets go, and
//      another thread then takes the lock; without it, the lock stays held for as long as the
//      thread is stopped. The deferral nests. In the tree: threading_posix.cc's
//      SuspensionDeferral and logging.cc's uses of it (RememberLine, KeepRecentLines,
//      WriteRecentLines, Logger::AppendLine).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh). With fix 45
// left out (skip_fixes) there is nothing to check.
#undef NDEBUG
#if __has_include("../port/guest_crash.hpp")
#include "../port/guest_crash.hpp"  // the assembled tree's port/ (run from <tree>/tools)

#include <pthread.h>
#include <signal.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace gc = xbox360ps5::guest_crash;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::atomic<bool> finished{false};

// A file of the tree with its line ends as "\n" (canary_game_main.cpp keeps the drop's CRLF).
std::string ReadTree(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (const char c : text.str()) {
    if (c != '\r') out += c;
  }
  return out;
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

size_t Count(const std::string& text, const std::string& part) {
  size_t count = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
  return count;
}

// The text from `start` to the first "\n}\n" after it (a function's body at namespace level).
std::string Body(const std::string& text, const std::string& start) {
  const auto at = text.find(start);
  if (at == std::string::npos) std::fprintf(stderr, "  not found: %s\n", start.c_str());
  assert(at != std::string::npos && "function not found in the tree");
  const auto end = text.find("\n}\n", at);
  assert(end != std::string::npos);
  return text.substr(at, end + 3 - at);
}

// `parts` appear in `text` in this order.
bool InOrder(const std::string& text, std::initializer_list<const char*> parts) {
  size_t at = 0;
  for (const char* part : parts) {
    const auto found = text.find(part, at);
    if (found == std::string::npos) {
      std::fprintf(stderr, "  missing (in order): %s\n", part);
      return false;
    }
    at = found + std::string(part).size();
  }
  return true;
}

// ---------------------------------------------------------------- (a) the record and the text

void NothingRecorded() {
  gc::Recorder recorder;
  const gc::Crash crash = recorder.Get();
  assert(!crash.recorded && crash.pc == 0 && crash.thread == 0 && crash.count == 0);
  // The headline the frame watch always logged (fmt's "{:08X} '{}' after {:.0f}s, {} ... {}").
  assert(gc::StallHeadline(crash, 50000, 0x584111F7, "Minecraft", 30.4, 707, 1414) ==
         "STALL title 584111F7 'Minecraft' after 30s, 707 refreshed outputs, 1414 submitted swaps; "
         "no output is not proof of deadlock");
  assert(gc::ThreadNote(crash, 0xF8000104, 50000).empty());
  assert(gc::NoticeDetail(crash).empty());
  std::printf("  nothing recorded: the STALL headline as before, no thread note, no notice detail\n");
}

void MinecraftCrash() {
  gc::Recorder recorder;
  // draft:194-198: thread F8000104, PC 824F849C, access violation reading 0x0000001000000000.
  assert(recorder.Record(0x824F849C, 0xF8000104, 0x0000001000000000ull, 120000));
  gc::Crash crash = recorder.Get();
  assert(crash.recorded && crash.pc == 0x824F849C && crash.thread == 0xF8000104);
  assert(crash.fault_address == 0x0000001000000000ull && crash.at_ms == 120000 && crash.count == 1);
  const std::string headline =
      gc::StallHeadline(crash, 136900, 0x584111F7, "Minecraft", 15.0, 707, 707);
  assert(headline ==
         "STALL after a guest crash on thread F8000104 at PC 824F849C, 16 s ago; the emulator paused "
         "itself (title 584111F7 'Minecraft' after 15s, 707 refreshed outputs, 707 submitted swaps)");
  assert(headline.rfind("STALL after a guest crash on thread F8000104 at PC 824F849C, ", 0) == 0);
  assert(!Has(headline, "no output is not proof of deadlock"));
  assert(gc::SecondsAgo(crash, 119000) == 0);  // a clock read before the record: never negative
  assert(gc::ThreadNote(crash, 0xF8000104, 136900) ==
         "crashed at PC 824F849C (access violation at 0000001000000000), 16 s ago");
  assert(gc::ThreadNote(crash, 0xF8000110, 136900).empty());  // the Server thread keeps its wait text
  assert(gc::NoticeDetail(crash) == " (PC 824F849C)");

  // A second crash (another thread faulting later) is counted; the first stays the one named.
  assert(!recorder.Record(0x82000000, 0xF8000110, 0, 130000));
  crash = recorder.Get();
  assert(crash.pc == 0x824F849C && crash.thread == 0xF8000104 && crash.at_ms == 120000 && crash.count == 2);
  assert(gc::StallHeadline(crash, 136900, 0x584111F7, "Minecraft", 15.0, 707, 707) ==
         "STALL after a guest crash on thread F8000104 at PC 824F849C, 16 s ago; the emulator paused "
         "itself (title 584111F7 'Minecraft' after 15s, 707 refreshed outputs, 707 submitted swaps; "
         "2 guest crashes in all)");

  // An illegal instruction: no fault address.
  gc::Recorder other;
  other.Record(0x82100004, 0xF8000020, 0, 1000);
  assert(gc::ThreadNote(other.Get(), 0xF8000020, 3500) == "crashed at PC 82100004, 2 s ago");
  std::printf("  Minecraft's crash: the STALL headline, the crashed thread's state, a later crash counted\n");
}

void ConcurrentCrashes() {
  gc::Recorder recorder;
  std::atomic<int> kept{0};
  std::vector<std::thread> threads;
  for (uint32_t n = 0; n < 8; ++n) {
    threads.emplace_back([&, n] {
      if (recorder.Record(0x82000000 + n * 4, 0xF8000100 + n * 4, n, 1000 + n)) ++kept;
    });
  }
  for (auto& thread : threads) thread.join();
  const gc::Crash crash = recorder.Get();
  assert(kept == 1 && crash.count == 8);
  // The one kept is whole: its pc, thread, fault address and time belong together.
  const uint32_t n = (crash.pc - 0x82000000) / 4;
  assert(n < 8 && crash.thread == 0xF8000100 + n * 4 && crash.fault_address == n &&
         crash.at_ms == int64_t(1000 + n));
  assert(!gc::Global().Get().recorded);  // the process-wide recorder is a separate one
  std::printf("  8 threads crashing at once: one crash kept whole, 8 counted\n");
}

void TreeUsesIt() {
  // The hook's signature, in the core (weak) and in the port.
  const std::string hook =
      "extern \"C\" void Xbox360PS5GuestCrashed(uint32_t guest_pc, uint32_t thread_handle,\n" +
      std::string(39, ' ') + "uint64_t fault_address)";
  const std::string emulator = ReadTree(".deps/xenia-canary/src/xenia/emulator.cc");
  assert(Has(emulator, hook + " __attribute__((weak));"));
  // In ExceptionCallback: the port told as soon as the function and thread are known (before the
  // dump is built and flushed, and before Pause()); the guest function not trusted to be there.
  // The core's modal only without the hook.
  const std::string callback = Body(emulator, "bool Emulator::ExceptionCallback(Exception* ex) {");
  assert(InOrder(callback, {"auto guest_function = code_cache->LookupFunction(ex->pc());",
                            "if (Xbox360PS5GuestCrashed) {",
                            "guest_function ? guest_function->MapMachineCodeToGuestAddress(ex->pc())",
                            "ex->code() == Exception::Code::kAccessViolation ? ex->fault_address()",
                            "Xbox360PS5GuestCrashed(guest_pc,",
                            "current_thread ? current_thread->handle() : 0", "std::string crash_msg;",
                            "XELOGE(\"{}\", crash_msg);", "FlushLog();", "  Pause();",
                            "if (display_window_ && imgui_drawer_ && !Xbox360PS5GuestCrashed) {",
                            "ShowMessageBox(", "current_thread->Suspend(nullptr);"}));
  assert(Count(callback, "Xbox360PS5GuestCrashed(") == 1);  // called once
  assert(Count(callback, "display_window_ && imgui_drawer_") == 1);

  const std::string main = ReadTree("src/canary_game_main.cpp");
  // The include after the resolver's declaration, where fix 33's line above it is not context.
  assert(Has(main, "extern \"C\" void ps5x_resolver_report(char* out, size_t size);  // port/ps5_resolver.cpp\n"
                   "#include \"guest_crash.hpp\""));
  assert(Has(main, hook + " {"));
  assert(Has(main, "xbox360ps5::guest_crash::Global().Record(guest_pc, thread_handle, fault_address,"));
  // The frame watch's first STALL line comes from StallHeadline; the old text only there.
  assert(Has(main, "XELOGW(\"{}\", xbox360ps5::guest_crash::StallHeadline("));
  assert(!Has(main, "no output is not proof of deadlock"));
  // ReportStall names the crashed thread instead of its wait text.
  const std::string report = Body(main, "void ReportStall(xe::Emulator& emulator, int pass) {");
  assert(InOrder(report, {"xbox360ps5::guest_crash::Global().Get()",
                          "xbox360ps5::guest_crash::ThreadNote(crash, thread->handle(), crash_now)",
                          "crashed.empty() ? WaitText(*thread) : crashed"}));
  // The overlay: the notice whenever a crash is recorded, with the PC after it.
  assert(Has(main, "const auto crash = xbox360ps5::guest_crash::Global().Get();"));
  assert(Has(main, "notice += xbox360ps5::guest_crash::NoticeDetail(crash);"));
  // The way out after a crash: no snapshot asked of the paused GPU worker (30 s for nothing).
  const std::string checkpoint = Body(main, "void CheckpointExitCache(");
  assert(InOrder(checkpoint, {"if (xbox360ps5::guest_crash::Global().Get().recorded) {",
                              "\"Vulkan cache: exit checkpoint skipped after a guest crash\"", "return;",
                              "RequestPipelineCacheSnapshot()"}));
  // The recorder needs no guard inside the exception handler.
  const std::string header = ReadTree("port/guest_crash.hpp");
  assert(Has(header, "inline constinit Recorder global_recorder;"));
  std::printf("  the tree: hook declared weak and called before the dump and Pause(), no core modal with "
              "it, defined by the port, used by the STALL report, the overlay and the exit checkpoint\n");
}

// ---------------------------------------------------------------- (b) the language files

// "\s" in a language file is a space, "\n" a line break (rekey.py's escape()).
std::string Unescape(const std::string& text) {
  std::string out;
  for (size_t at = 0; at < text.size(); ++at) {
    if (text[at] == '\\' && at + 1 < text.size()) {
      const char next = text[++at];
      out += next == 's' ? ' ' : next == 'n' ? '\n' : next == 't' ? '\t' : next;
    } else {
      out += text[at];
    }
  }
  return out;
}

void LanguageFiles(const fs::path& folder) {
  const std::string keys[] = {gc::kNoticeTouchpad, gc::kNoticeOptions};
  std::map<std::string, std::map<std::string, std::string>> texts;  // code -> key -> text
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    std::ifstream file(entry.path(), std::ios::binary);
    for (std::string line; std::getline(file, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto split = line.find(" = ");
      if (split == std::string::npos) continue;
      const std::string key = Unescape(line.substr(0, split));
      for (const auto& wanted : keys) {
        if (key == wanted) texts[entry.path().stem().string()][key] = Unescape(line.substr(split + 3));
      }
    }
  }
  const char* codes[] = {"en", "pt-BR", "pt", "es", "es-419", "de", "fr", "it", "ja", "ko", "nl", "pl",
                         "ru", "tr", "zh-Hans", "zh-Hant"};
  for (const char* code : codes) {
    for (const auto& key : keys) {
      const auto& file = texts[code];
      const auto found = file.find(key);
      if (found == file.end() || found->second.empty()) {
        std::fprintf(stderr, "  %s.txt lacks: %s\n", code, key.c_str());
        assert(!"the crash notice is in every language file");
      }
    }
  }
  assert(texts["en"][keys[0]] == keys[0] && texts["en"][keys[1]] == keys[1]);
  assert(texts["pl"][keys[0]] ==
         "Gra uległa awarii i emulator ją wstrzymał. Touchpad: przewodnik, potem Zamknij grę i "
         "wróć do menu");
  assert(texts["pl"][keys[1]] ==
         "Gra uległa awarii i emulator ją wstrzymał. OPTIONS + touchpad: przewodnik, potem Zamknij "
         "grę i wróć do menu");
  // es.txt has no Spanish for the guide's row, which shows in English there: the notice names it
  // as it is on the screen.
  assert(Has(texts["es"][keys[0]], "Close the game and return to the menu"));
  assert(Has(texts["es-419"][keys[0]], "Cerrar el juego y volver al menú"));
  // The overlay asks for exactly these keys.
  const std::string main = ReadTree("src/canary_game_main.cpp");
  assert(Has(main, "Tr(\"" + keys[0] + "\")") && Has(main, "Tr(\"" + keys[1] + "\")"));
  std::printf("  both notices in all %zu language files (Polish translated), asked for by the overlay\n",
              sizeof(codes) / sizeof(codes[0]));
}

// ---------------------------------------------------------------- (c) the XMA pause handshake

// xe::threading's auto-reset event (CreateAutoResetEvent): Set wakes one waiter, or the next Wait.
class AutoResetEvent {
 public:
  void Set() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
    cond_.notify_one();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++waiting_;
    cond_.wait(lock, [this] { return signaled_; });
    --waiting_;
    signaled_ = false;
  }
  int waiting() {
    std::lock_guard<std::mutex> lock(mutex_);
    return waiting_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  bool signaled_ = false;
  int waiting_ = 0;
};

// xe::threading::Fence (base/threading.h) for one waiter: Signal sets it, Wait waits and clears it.
class Fence {
 public:
  void Signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
    cond_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return signaled_; });
    signaled_ = false;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  bool signaled_ = false;
};

// Which XmaDecoder::Pause runs: 1.0.2's (no Set), fix 45's draft (Set, but the live cvar decides
// whether a worker loop is there) or fix 45's (Set, the worker as Setup started it).
enum class PauseKind { k102, kDraft, kFix };

// XmaDecoder's worker, Pause and Resume (apu/xma_decoder.cc), with the contexts' work reduced to
// a count of kicks.
struct XmaModel {
  const PauseKind kind;
  const bool dedicated_worker;  // dedicated_worker_: cvars::use_dedicated_xma_thread at Setup
  std::atomic<bool> cvar;       // cvars::use_dedicated_xma_thread now (settings applied again)
  std::atomic<bool> worker_running{true}, paused{false};
  std::atomic<int> kicks{0}, passes_with_work{0};
  AutoResetEvent work_event;
  Fence pause_fence, resume_fence;
  std::thread worker;

  XmaModel(PauseKind kind, bool dedicated) : kind(kind), dedicated_worker(dedicated), cvar(dedicated) {
    // XmaDecoder::Setup's XHostThread: WorkerThreadMain only with a dedicated thread.
    if (dedicated_worker) worker = std::thread([this] { WorkerThreadMain(); });
  }
  void WorkerThreadMain() {
    while (worker_running) {
      const bool did_work = kicks.exchange(0) != 0;  // the pass over the contexts
      if (did_work) ++passes_with_work;
      if (paused) {
        pause_fence.Signal();
        resume_fence.Wait();
      }
      if (did_work) continue;
      work_event.Wait();
    }
  }
  void Pause() {
    if (paused) return;
    paused = true;
    if (kind != PauseKind::k102) {
      const bool loop = kind == PauseKind::kFix ? dedicated_worker : cvar.load();
      if (!loop) return;  // no worker loop: nobody would signal the fence
      work_event.Set();   // an idle worker wakes and reaches the paused check
    }
    pause_fence.Wait();
  }
  void Resume() {
    if (!paused) return;
    paused = false;
    resume_fence.Signal();
  }
  void Kick() {  // WriteRegister's context kick: work_event_->SetBoostPriority()
    ++kicks;
    work_event.Set();
  }
  void Shutdown() {
    worker_running = false;
    work_event.Set();
    if (paused) Resume();
    if (worker.joinable()) worker.join();
  }
  void WaitIdle() {  // the worker asleep on work_event_
    const auto until = Clock::now() + 5s;
    while (work_event.waiting() == 0) {
      assert(Clock::now() < until && "the worker never went idle");
      std::this_thread::sleep_for(1ms);
    }
  }
  void WaitPassesAbove(int count) {
    const auto until = Clock::now() + 2s;
    while (passes_with_work <= count) {
      assert(Clock::now() < until && "the worker decodes the kick");
      std::this_thread::sleep_for(1ms);
    }
  }
};

// Runs Pause() on its own thread (the crashing guest thread in ExceptionCallback).
struct PauseCall {
  const Clock::time_point started = Clock::now();
  std::atomic<bool> returned{false};
  std::atomic<int64_t> took_us{-1};
  std::thread thread;
  explicit PauseCall(XmaModel& model) : thread([this, &model] {
    model.Pause();
    took_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
    returned = true;
  }) {}
  bool ReturnsWithin(std::chrono::milliseconds limit) {
    const auto until = started + limit;
    while (!returned && Clock::now() < until) std::this_thread::sleep_for(1ms);
    return returned;
  }
  ~PauseCall() { thread.join(); }
};

// The handshake's bound is 100 ms; a shared runner can stall a thread longer now and then, which
// says nothing about the fix (1.0.2's Pause does not return at all without a kick). So: every
// round within 1 s, and all but a few within 100 ms.
constexpr auto kExpected = 100ms;
constexpr auto kHardLimit = 1000ms;
constexpr int kSlowRoundsAllowed = 5;

void FixedPauseWithIdleWorker() {
  XmaModel model(PauseKind::kFix, true);
  // Some audio first, as before the crash; then the guest's audio stops (AudioSystem paused).
  model.Kick();
  model.WaitIdle();
  int slow = 0;
  int64_t slowest_us = 0;
  for (int round = 0; round < 50; ++round) {
    const int kicks_before = model.kicks.load();
    {
      PauseCall call(model);
      assert(call.ReturnsWithin(kHardLimit) && "fix 45: Pause() returns with the worker idle, no kick");
      assert(model.kicks == kicks_before);  // nothing kicked the worker
      if (call.took_us > std::chrono::duration_cast<std::chrono::microseconds>(kExpected).count()) ++slow;
      slowest_us = std::max<int64_t>(slowest_us, call.took_us);
    }
    // While paused, a kick is not decoded; after Resume it is.
    const int worked = model.passes_with_work.load();
    model.Kick();
    std::this_thread::sleep_for(round == 0 ? 20ms : 0ms);
    assert(model.passes_with_work == worked);
    model.Resume();
    model.WaitPassesAbove(worked);
    model.WaitIdle();
  }
  model.Shutdown();
  std::printf("  fix 45, worker idle: Pause() returns without a kick (50 pauses, each resumed; %d over "
              "100 ms, slowest %.1f ms)\n",
              slow, slowest_us / 1000.0);
  assert(slow <= kSlowRoundsAllowed && "fix 45: Pause() returns within 100 ms as a rule");
}

void FixedPauseWithBusyWorker() {
  // The worker in the middle of a pass when Pause() comes: it still stops after that pass.
  XmaModel model(PauseKind::kFix, true);
  std::atomic<bool> stop{false};
  std::thread kicker([&] {
    while (!stop) {
      model.Kick();
      std::this_thread::sleep_for(200us);
    }
  });
  std::this_thread::sleep_for(20ms);
  int slow = 0;
  for (int round = 0; round < 5; ++round) {
    {
      PauseCall call(model);
      assert(call.ReturnsWithin(kHardLimit));
      if (call.took_us > std::chrono::duration_cast<std::chrono::microseconds>(kExpected).count()) ++slow;
    }
    model.Resume();
    std::this_thread::sleep_for(5ms);
  }
  stop = true;
  kicker.join();
  model.Shutdown();
  std::printf("  fix 45, worker busy: Pause() returns (5 pauses, %d over 100 ms)\n", slow);
  assert(slow <= 1);
}

void FixedPauseWithoutWorker() {
  XmaModel model(PauseKind::kFix, false);  // xma_inline: the XMA thread returns at once
  {
    PauseCall call(model);
    assert(call.ReturnsWithin(kHardLimit) && "fix 45: Pause() does not wait for a worker that does not run");
  }
  model.Resume();
  model.Shutdown();
  std::printf("  fix 45, no worker loop (xma_inline): Pause() returns at once\n");
}

void OldPauseHangs() {
  {
    XmaModel model(PauseKind::k102, true);
    model.WaitIdle();
    {
      PauseCall call(model);
      assert(!call.ReturnsWithin(300ms) && "1.0.2's Pause() waits for an idle worker");
      // Only a context kick wakes the worker; after a crash nothing kicks (the guest's audio is
      // paused and its threads are about to be).
      model.Kick();
      assert(call.ReturnsWithin(2300ms));
    }
    model.Resume();
    model.Shutdown();
  }
  {
    XmaModel model(PauseKind::k102, false);
    {
      PauseCall call(model);
      assert(!call.ReturnsWithin(300ms) && "1.0.2's Pause() waits for a worker loop that never runs");
      model.pause_fence.Signal();  // what nothing in 1.0.2 does: let the check end
      assert(call.ReturnsWithin(2300ms));
    }
    model.Resume();
    model.Shutdown();
  }
  std::printf("  1.0.2's Pause() still waiting after 300 ms, idle worker or no worker loop: the frozen crash handler\n");
}

void SettingsAppliedAgain() {
  // Started with xma_inline (no worker loop), then the settings applied again with it off: the
  // cvar says "dedicated" while no loop runs.
  {
    XmaModel model(PauseKind::kDraft, false);
    model.cvar = true;
    {
      PauseCall call(model);
      assert(!call.ReturnsWithin(300ms) && "the draft's Pause() waits for a loop that never started");
      model.pause_fence.Signal();
      assert(call.ReturnsWithin(2300ms));
    }
    model.Resume();
    model.Shutdown();
  }
  {
    XmaModel model(PauseKind::kFix, false);
    model.cvar = true;
    {
      PauseCall call(model);
      assert(call.ReturnsWithin(kHardLimit) && "fix 45: the worker as Setup started it decides");
    }
    model.Resume();
    model.Shutdown();
  }
  // Started with a worker loop, then the cvar turned off: the draft's Pause() returned while the
  // worker still decoded; fix 45's waits until the worker has stopped.
  {
    XmaModel model(PauseKind::kDraft, true);
    model.WaitIdle();
    model.cvar = false;
    {
      PauseCall call(model);
      assert(call.ReturnsWithin(kHardLimit));
    }
    const int worked = model.passes_with_work.load();
    model.Kick();
    model.WaitPassesAbove(worked);  // decoded although "paused"
    model.Resume();
    model.Shutdown();
  }
  {
    XmaModel model(PauseKind::kFix, true);
    model.WaitIdle();
    model.cvar = false;
    {
      PauseCall call(model);
      assert(call.ReturnsWithin(kHardLimit));
    }
    const int worked = model.passes_with_work.load();
    model.Kick();
    std::this_thread::sleep_for(20ms);
    assert(model.passes_with_work == worked && "fix 45: nothing decoded while paused");
    model.Resume();
    model.WaitPassesAbove(worked);
    model.Shutdown();
  }
  std::printf("  settings applied again (the cvar changed after Setup): fix 45 follows the worker as it "
              "started; the draft's Pause() waited for nothing or returned while decoding\n");
}

void TreeXmaDecoder() {
  const std::string xma = ReadTree(".deps/xenia-canary/src/xenia/apu/xma_decoder.cc");
  const std::string xma_h = ReadTree(".deps/xenia-canary/src/xenia/apu/xma_decoder.h");
  assert(Has(xma_h, "bool dedicated_worker_ = true;"));
  // The cvar is read once, in Setup, before the worker thread starts.
  assert(Count(xma, "cvars::use_dedicated_xma_thread") == 1);
  assert(InOrder(xma, {"dedicated_worker_ = cvars::use_dedicated_xma_thread;",
                       "work_event_ = xe::threading::Event::CreateAutoResetEvent(false);", "[this]() {",
                       "if (dedicated_worker_) {", "WorkerThreadMain();", "worker_thread_->Create();"}));
  const std::string write = Body(xma, "void XmaDecoder::WriteRegister(uint32_t addr, uint32_t value) {");
  assert(InOrder(write, {"context.Enable();", "if (!dedicated_worker_) {", "context.Work();",
                         "work_event_->SetBoostPriority();", "if (dedicated_worker_) {", "WaitForWorkDone();"}));
  const std::string worker = Body(xma, "void XmaDecoder::WorkerThreadMain() {");
  assert(InOrder(worker, {"while (worker_running_) {", "contexts_[n]->Work();", "if (paused_) {",
                          "pause_fence_.Signal();", "resume_fence_.Wait();",
                          "if (did_work",  // fix 155b adds "&& (!continuous || progressed)"
                          "xe::threading::Wait(work_event_.get(), false);"}));
  const std::string pause = Body(xma, "void XmaDecoder::Pause() {");
  assert(InOrder(pause, {"if (paused_) {", "paused_ = true;", "if (!dedicated_worker_) {", "return;",
                         "work_event_->Set();", "pause_fence_.Wait();"}));
  // Set() unconditionally, once: between the xma_inline return and it only comments.
  assert(Count(pause, "work_event_->Set();") == 1 && Count(pause, "pause_fence_.Wait();") == 1);
  const auto inline_return = pause.find("return;\n  }\n", pause.find("if (!dedicated_worker_) {"));
  const auto set = pause.find("work_event_->Set();");
  assert(inline_return != std::string::npos && inline_return < set);
  const std::string between = pause.substr(inline_return, set - inline_return);
  assert(!Has(between, "if (") && !Has(between, "else") && !Has(between, "{"));
  const std::string resume = Body(xma, "void XmaDecoder::Resume() {");
  assert(InOrder(resume, {"paused_ = false;", "resume_fence_.Signal();"}));
  // The audio worker's pause, which the fix mirrors, wakes its worker the same way.
  const std::string audio = ReadTree(".deps/xenia-canary/src/xenia/apu/audio_system.cc");
  assert(InOrder(Body(audio, "void AudioSystem::Pause() {"),
                 {"paused_ = true;", "pending_work_event_->Set();", "pause_fence_.Wait();",
                  "xma_decoder_->Pause();"}));
  std::printf("  the tree's XmaDecoder::Setup, WriteRegister, Pause, WorkerThreadMain and Resume are the model's\n");
}

// ---------------------------------------------------------------- (d) suspension and the log lock

// Thread::Suspend on the PS5 is a signal whose handler waits until the thread is resumed.
constexpr int kSuspendSignal = SIGUSR1;
std::atomic<bool> model_suspended{false}, model_resume{false};

void ModelSuspendHandler(int) {
  model_suspended.store(true);
  while (!model_resume.load()) {
    timespec nap{0, 1000000};
    nanosleep(&nap, nullptr);
  }
  model_resume.store(false);
  model_suspended.store(false);
}

// threading_posix.cc's SuspensionDeferral (fix 45): the suspend signal blocked while it lives,
// then put back as it was found.
class ModelDeferral {
 public:
  ModelDeferral() {
    sigset_t signals, previous;
    sigemptyset(&signals);
    sigaddset(&signals, kSuspendSignal);
    pthread_sigmask(SIG_BLOCK, &signals, &previous);
    was_deferred_ = sigismember(&previous, kSuspendSignal) == 1;
  }
  ~ModelDeferral() {
    if (was_deferred_) return;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, kSuspendSignal);
    pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
  }
  ModelDeferral(const ModelDeferral&) = delete;
  ModelDeferral& operator=(const ModelDeferral&) = delete;

 private:
  bool was_deferred_ = false;
};

bool SuspendBlocked() {
  sigset_t now;
  pthread_sigmask(SIG_BLOCK, nullptr, &now);
  return sigismember(&now, kSuspendSignal) == 1;
}

void WaitUntil(const std::atomic<bool>& flag, const char* what) {
  const auto until = Clock::now() + 5s;
  while (!flag) {
    if (Clock::now() >= until) std::fprintf(stderr, "  never: %s\n", what);
    assert(Clock::now() < until);
    std::this_thread::sleep_for(1ms);
  }
}

bool TakesLockWithin(std::mutex& mutex, std::chrono::milliseconds limit) {
  const auto until = Clock::now() + limit;
  do {
    if (mutex.try_lock()) {
      mutex.unlock();
      return true;
    }
    std::this_thread::sleep_for(1ms);
  } while (Clock::now() < until);
  return false;
}

// A guest thread in RememberLine (holding recent_mutex_) when Emulator::Pause suspends it; then
// the frame loop logs. True when the frame loop's log line could not take the lock.
bool LoggerBlocked(bool deferred) {
  std::mutex recent_mutex;  // logging.cc's recent_mutex_
  std::atomic<bool> holding{false}, copied{false};
  std::thread guest([&] {
    std::optional<ModelDeferral> deferral;
    if (deferred) deferral.emplace();
    {
      std::lock_guard<std::mutex> lock(recent_mutex);
      holding = true;
      while (!copied) std::this_thread::sleep_for(1ms);  // the line's copy, stretched
    }
    deferral.reset();  // the end of RememberLine: a pending suspension is taken here
  });
  WaitUntil(holding, "the guest thread holds the lock");
  pthread_kill(guest.native_handle(), kSuspendSignal);  // Emulator::Pause's suspend loop
  if (deferred) {
    std::this_thread::sleep_for(50ms);
    assert(!model_suspended && "deferred: not stopped while it holds the lock");
  } else {
    WaitUntil(model_suspended, "the guest thread stopped holding the lock");
  }
  copied = true;  // the section would end now, if the thread ran
  WaitUntil(model_suspended, "the guest thread suspended");
  // The frame loop's next log line (or its STALL report's recent lines).
  const bool took = TakesLockWithin(recent_mutex, deferred ? 1000ms : 300ms);
  model_resume = true;  // Emulator::Resume (or nothing: then for ever)
  guest.join();
  assert(!model_suspended && !model_resume);
  return !took;
}

void SuspendedWhileLogging() {
  struct sigaction action {};
  action.sa_handler = ModelSuspendHandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  assert(sigaction(kSuspendSignal, &action, nullptr) == 0);

  assert(LoggerBlocked(false) && "1.0.2: a thread stopped in RememberLine keeps the log's lock");
  assert(!LoggerBlocked(true) && "fix 45: stopped only once it let go of the lock");

  // It nests: an inner deferral that ends leaves the outer one (a state_mutex_ section, or
  // Thread::DeferSuspension) in force, and the last one restores what was there.
  std::thread([] {
    assert(!SuspendBlocked());
    {
      ModelDeferral outer;
      assert(SuspendBlocked());
      {
        ModelDeferral inner;
        assert(SuspendBlocked());
      }
      assert(SuspendBlocked() && "an inner deferral leaves the outer one in force");
    }
    assert(!SuspendBlocked());
  }).join();
  std::printf("  a thread suspended while it holds the log's lock: 1.0.2 blocks the next logger; with the "
              "deferral it stops only after the lock is free, and deferrals nest\n");
}

void TreeSuspensionDeferral() {
  const std::string threading = ReadTree(".deps/xenia-canary/src/xenia/base/threading.h");
  assert(Has(threading, "class SuspensionDeferral {"));
  const std::string posix = ReadTree(".deps/xenia-canary/src/xenia/base/threading_posix.cc");
  assert(InOrder(Body(posix, "SuspensionDeferral::SuspensionDeferral() {"),
                 {"sigaddset(&signals, GetSystemSignal(SignalType::kThreadSuspend));",
                  "pthread_sigmask(SIG_BLOCK, &signals, &previous);", "was_deferred_ =",
                  "sigismember(&previous, GetSystemSignal(SignalType::kThreadSuspend)) == 1;"}));
  assert(InOrder(Body(posix, "SuspensionDeferral::~SuspensionDeferral() {"),
                 {"if (was_deferred_) {", "return;", "pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);"}));
  const std::string log = ReadTree(".deps/xenia-canary/src/xenia/base/logging.cc");
  // Every hold of recent_mutex_ is deferred, and the ring's claim-to-publish too.
  // Fix 93e adds a fourth, TakeRecentLines, deferred the same way.
  std::vector<const char*> holders = {"void RememberLine(", "void logging::KeepRecentLines(bool keep) {",
                                      "void logging::WriteRecentLines() {"};
  // (Fix 93i gives it a parameter.)
  if (log.find("std::string logging::TakeRecentLines(") != std::string::npos)
    holders.push_back("std::string logging::TakeRecentLines(");
  assert(Count(log, "std::lock_guard<std::mutex> lock(recent_mutex_);") == holders.size());
  for (const char* function : holders) {
    assert(InOrder(Body(log, function),
                   {"xe::threading::SuspensionDeferral deferral;", "std::lock_guard<std::mutex> lock(recent_mutex_);"}));
  }
  const auto append = log.find("  void AppendLine(uint32_t thread_id, const char prefix_char,");
  assert(append != std::string::npos);
  assert(InOrder(log.substr(append), {"xe::threading::SuspensionDeferral deferral;",
                                      "claim_strategy_.claim(count);", "claim_strategy_.publish(range);"}));
  std::printf("  the tree: SuspensionDeferral restores the mask it found; the log defers suspension around "
              "recent_mutex_ and the ring's claim and publish\n");
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2 && "usage: check-guest-crash <english/lang folder>");
  std::thread([] {
    const auto until = Clock::now() + 120s;
    while (Clock::now() < until) {
      if (finished) return;
      std::this_thread::sleep_for(100ms);
    }
    std::printf("FAIL: still running after 120 s (a Pause() that never returned?)\n");
    std::fflush(stdout);
    std::_Exit(2);
  }).detach();
  std::printf("check-guest-crash\n");
  NothingRecorded();
  MinecraftCrash();
  ConcurrentCrashes();
  TreeUsesIt();
  LanguageFiles(argv[1]);
  FixedPauseWithIdleWorker();
  FixedPauseWithBusyWorker();
  FixedPauseWithoutWorker();
  OldPauseHangs();
  SettingsAppliedAgain();
  TreeXmaDecoder();
  SuspendedWhileLogging();
  TreeSuspensionDeferral();
  finished = true;
  std::printf("check-guest-crash: all passed\n");
  return 0;
}
#else
#include <cstdio>

int main() {
  std::printf("check-guest-crash: fix 45 left out (no port/guest_crash.hpp), not checked\n");
  return 0;
}
#endif
