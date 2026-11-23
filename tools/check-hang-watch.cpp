// SPDX-License-Identifier: MIT
// Fix 47 (tools/source-1.0.2/fixes/47-hang-watch.patch): a game whose process the system ends
// without a crash report (Ninja Gaiden II, issue #11: no STALL report and no Performance line,
// so the UI loop that writes them stood still, then the app was gone ~30 s later).
//  (a) include/xbox360ps5/hang_watch.hpp: which thread is in which Vulkan call since when (also
//      through gpu_diag::Call), nested calls, the submission a fence wait is for, slots freed by
//      exiting threads (more threads than slots over a game), the ring of recent draws and
//      resolves (oldest first, the newest 32 kept).
//  (b) the Watcher, stepped with a clock of its own: nothing while the UI loop turns and calls
//      return; a fence wait of the "gpu" thread that lasts 3 s gives one report (UI and GPU
//      heartbeats, the queue, each watched thread, the draws of the open submissions, the GPU
//      hang verdict) with a probe and the shaders of the draws in flight to write out; a line
//      every 5 s while it lasts; the report again after 20 s; "HANG over" when it ends. A UI loop
//      stopped outside any call is reported with the other verdict; one measuring on purpose
//      is not; pipeline creation only after 20 s. Every line fits Stage's 200-byte buffer.
//  (c) the assembled tree has the hooks: gpu_diag::Call marks its calls, the completion timeline
//      names the awaited submission and marks the queue submit, the presenter marks its acquire,
//      the command processor names its thread and beats, the Vulkan command processor records
//      draws, resolves and submissions, canary_game_main.cpp runs the watch thread around the
//      game loop only (stopped before the exit's waits) with the UI loop's beats, and
//      crash_report.cpp reports the signals that end a process without a fault (SIGPIPE then
//      ignored).
//  (f) candidate 8 (candidate 4: the process ended ~1 s after the UI thread's acquire had waited
//      293 ms; the 3 s report never came): a GPU call past 200 ms or the UI loop still for 1 s is
//      reported, checked every 50 ms; the work ring holds 128 entries with each draw's targets
//      (RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO) and first texture's fetch constant; a
//      report goes out in one write and one fsync; failed submits, fence waits and acquires are
//      written raw.
//  (d) candidate 4 (candidate 1 left no line at all with Ninja Gaiden II, which closed by itself
//      after loading): the heartbeat line (loops, frames, GPU ring, last submission, the log
//      writer's progress, calls in progress) every second; the watch thread sleeps with usleep (no
//      condition variable); its lines and the raw sink (hang_watch::Raw) go to hang-watch.txt
//      opened at the start, with write() and fsync(), to the kernel log, and to the stream only
//      through NetLogTry (its lock taken only if free); the log counts lines handed to its writer
//      and lines written; the raw sink is null unless set and calls the sink when set; the game's
//      own DbgPrint messages (the first 64), its XamLoaderTerminateTitle / XamLoaderLaunchTitle
//      (thread, caller, r3, r30) and each step of the title-switch restart go through it, and
//      exit() / quick_exit() leave a line.
//  (g) candidate 11 (candidate 8: the last beat, 9 s into play, was healthy and nothing followed):
//      the beat comes every 250 ms and carries the memory the system still has and the guest
//      heaps' reserved bytes; a guest allocation is counted and one of 8 MiB or more, or a failed
//      one, is a raw line; the crash report's lines (signals, terminate, probes, Stage) are
//      mirrored to hang-watch.txt, which stays open after the game loop; every LoadExec of the
//      title, the window's close, a FatalError, HalReturnToFirmware, KeBugCheck, TerminateTitle
//      and the guest's main thread ending leave a line.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../include/xbox360ps5/gpu_diagnostics.hpp"  // includes hang_watch.hpp (fix 47)

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace hw = xbox360ps5::hang_watch;

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
bool Has(const std::vector<std::string>& lines, const std::string& part) {
  for (const auto& line : lines)
    if (line.find(part) != std::string::npos) return true;
  return false;
}
void Show(const char* title, const std::vector<std::string>& lines) {
  std::printf("-- %s\n", title);
  for (const auto& line : lines) std::printf("   %s\n", line.c_str());
}
void ExpectFits(const std::vector<std::string>& lines) {
  // Stage: "[X360] " + text + "\n" in 200 bytes.
  for (const auto& line : lines) Expect(line.size() <= 191, "line fits Stage: " + line);
}

// A thread that stays inside a watched call until released.
class Blocked {
 public:
  Blocked(const char* role, const char* call, uint64_t awaited, bool through_gpu_diag = false) {
    thread_ = std::thread([=, this] {
      if (role) hw::NameThread(role);
      if (awaited) hw::SetAwaitedSubmission(awaited);
      const auto wait = [this] {
        std::unique_lock<std::mutex> lock(mutex_);
        inside_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
        return 0;
      };
      if (through_gpu_diag) {
        xbox360ps5::gpu_diag::Call(xbox360ps5::gpu_diag::Kind::fence, wait);
      } else {
        hw::InCall in_call(call);
        hw::InCall nested("submit");  // nested: the outer call is what is reported
        wait();
      }
    });
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return inside_; });
  }
  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  ~Blocked() { Release(); }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool inside_ = false, released_ = false;
  std::thread thread_;
};

const hw::SlotView* Find(const std::vector<hw::SlotView>& slots, const char* role) {
  for (const auto& slot : slots)
    if (slot.role && std::string(slot.role) == role) return &slot;
  return nullptr;
}

void CheckSlots() {
  {
    Blocked gpu("gpu", "fence", 7, true);
    const auto busy = hw::BusySlots();
    const auto* slot = Find(busy, "gpu");
    Expect(slot && slot->call && std::string(slot->call) == "fence", "gpu_diag::Call marks a fence wait");
    Expect(slot && slot->detail == 8, "the awaited submission is kept (+1)");
    gpu.Release();
    const auto after = hw::BusySlots();
    Expect(!Find(after, "gpu"), "an exited thread frees its slot");
  }
  {
    Blocked nested("ui", "acquire", 0);
    const auto* slot = Find(hw::BusySlots(), "ui");
    Expect(slot && slot->call && std::string(slot->call) == "acquire", "a nested call keeps the outer call's name");
  }
  // More threads over a game than slots: each exits and frees its slot.
  for (int round = 0; round < 3 * int(hw::kSlots); ++round) {
    std::thread([] { hw::InCall call("pipeline"); }).join();
  }
  {
    Blocked late("late", "fence", 0);
    Expect(Find(hw::BusySlots(), "late") != nullptr, "slots are still free after many threads came and went");
  }
}

void CheckWork() {
  int vs_object = 0, ps_object = 0;
  for (uint32_t n = 0; n < 140; ++n) {
    if (n % 5 == 3)  // the newest (n = 39) is a draw
      hw::RecordResolve(100 + n / 10, 0x1F778000 + n, 5447680, 32);
    else
      hw::RecordDraw(100 + n / 10, 0x1000 + n, &vs_object, 0x2000 + n, &ps_object, 4, 3 * n, n % 2 ? hw::kIndexed : 0);
  }
  const auto recent = hw::RecentWork();
  Expect(recent.size() == hw::kWork && hw::kWork == 128, "the ring keeps the newest 128");
  bool ordered = true;
  for (size_t i = 1; i < recent.size(); ++i) ordered = ordered && recent[i - 1].serial < recent[i].serial;
  Expect(ordered, "recent work is oldest first");
  Expect(!recent.empty() && recent.back().vs == 0x1000 + 139 && recent.back().submission == 113, "the newest draw is last");
  Expect(!recent.empty() && recent.front().serial == hw::work_serial.load() - hw::kWork + 1, "the oldest kept is 128 back");
}

void CheckWatcher() {
  namespace gd = xbox360ps5::gpu_diag;
  hw::Watcher watcher;
  const uint64_t t0 = hw::NowMs();
  hw::ui_loop.Mark(hw::kUiInput);
  Expect(watcher.Observe(t0).lines.empty(), "first look: nothing");
  hw::ui_loop.Mark(hw::kUiPaint);
  Expect(watcher.Observe(t0 + 1000).lines.empty(), "a turning UI loop: nothing");
  // The draws of submission 7 (open: 6 is complete), then the GPU thread waits for 7 and the
  // UI loop stands in Paint.
  int vs_object = 0, ps_object = 0, other = 0;
  hw::RecordDraw(6, 0xAAAA, &other, 0, nullptr, 4, 3, 0);
  hw::RecordResolve(7, 0x1F778000, 5447680, 32);
  hw::DrawState state;
  state.surface = 0x00000500;
  state.color = 0x00030000;
  state.depth = 0x00140000;
  state.textures = 0x5;
  state.tex_d0 = 0x01800002;
  state.tex_d1 = 0x1F778000;
  hw::RecordDraw(7, 0x0123456789ABCDEFull, &vs_object, 0xFEDCBA9876543210ull, &ps_object, 8, 1200, hw::kIndexed, state);
  hw::Submitted(7, 6);
  hw::ui_loop.Mark(hw::kUiPaint);
  {
    Blocked gpu("gpu", nullptr, 7, true);
    Blocked ui("ui", "fence", 0);
    const uint64_t t1 = hw::NowMs();
    Expect(watcher.Observe(t1 + 100).lines.empty(), "100 ms in: nothing yet");
    auto step = watcher.Observe(t1 + 300);  // candidate 8: a GPU call past 200 ms
    Show("first report", step.lines);
    ExpectFits(step.lines);
    Expect(step.probe, "the first report asks where the threads are");
    Expect(Has(step.lines, "HANG report: UI loop unchanged for"), "headline");
    Expect(Has(step.lines, "(last paint)"), "the UI loop's last phase");
    Expect(Has(step.lines, "last GPU submission 7"), "the queue");
    Expect(Has(step.lines, "HANG gpu ") && Has(step.lines, "in fence for") && Has(step.lines, "for submission 7"),
           "the gpu thread's fence wait and its submission");
    Expect(Has(step.lines, "surface 00000500 color 00030000 depth 00140000 tex 5 (01800002 1F778000)"),
           "the draw's targets and first texture");
    Expect(Has(step.lines, "sub 7 (open) draw vs 0123456789ABCDEF ps FEDCBA9876543210 prim 8 vertices 1200 indexed"),
           "the draw in flight");
    Expect(Has(step.lines, "sub 7 (open) resolve to 1F778000, 5447680 bytes, format 32"), "the resolve in flight");
    Expect(Has(step.lines, "HANG verdict: the GPU does not finish a submission (gpu thread's wait for submission 7"),
           "the GPU hang verdict");
    bool vs_dumped = false, ps_dumped = false;
    for (const void* object : step.dump) {
      vs_dumped = vs_dumped || object == &vs_object;
      ps_dumped = ps_dumped || object == &ps_object;
    }
    Expect(vs_dumped && ps_dumped, "the shaders of the open submission are written out");
    Expect(watcher.reports() == 1 && watcher.hung(), "one report");
    Expect(watcher.Observe(t1 + 1300).lines.empty(), "no line between the 5 s marks");
    auto still = watcher.Observe(t1 + 5400);
    Show("still", still.lines);
    ExpectFits(still.lines);
    Expect(Has(still.lines, "HANG still +5 s") && Has(still.lines, "gpu:fence"), "a line every 5 s");
    auto again = watcher.Observe(t1 + 20400);
    Expect(Has(again.lines, "HANG report again") && again.probe && again.dump.empty(), "the report again after 20 s, no dump");
    gpu.Release();
    ui.Release();
  }
  hw::ui_loop.Mark(hw::kUiInput);
  auto over = watcher.Observe(hw::NowMs() + 26000);
  Show("over", over.lines);
  Expect(Has(over.lines, "HANG over after"), "the end is said");
  Expect(!watcher.hung(), "watching again");

  // A UI loop stopped outside any watched call (a lock, a log mutex): the other verdict.
  hw::Watcher stopped;
  hw::ui_loop.Mark(hw::kUiSample);
  const uint64_t t2 = hw::NowMs();
  stopped.Observe(t2);
  Expect(stopped.Observe(t2 + 900).lines.empty(), "0.9 s: nothing");
  auto step = stopped.Observe(t2 + 1100);  // candidate 8: the UI loop still for 1 s
  Show("UI loop stopped", step.lines);
  Expect(Has(step.lines, "HANG verdict: the UI loop is stopped outside a GPU wait"), "UI stop verdict");

  // Measuring on purpose: no report.
  hw::Watcher measuring;
  hw::ui_loop.Mark(hw::kUiMeasure);
  const uint64_t t3 = hw::NowMs();
  measuring.Observe(t3);
  Expect(measuring.Observe(t3 + 6000).lines.empty(), "a measurement is not a hang");
  hw::ui_loop.Mark(hw::kUiInput);

  // Pipeline creation: 5 s is no hang, 21 s is.
  hw::Watcher pipeline;
  {
    Blocked compiler("compiler", "pipeline", 0);
    const uint64_t t4 = hw::NowMs();
    pipeline.Observe(t4);
    for (uint64_t at = 1000; at <= 5000; at += 1000) {
      hw::ui_loop.Mark(hw::kUiInput);
      Expect(pipeline.Observe(t4 + at).lines.empty(), "a slow pipeline is no hang");
    }
    hw::ui_loop.Mark(hw::kUiInput);
    auto late = pipeline.Observe(t4 + 21000);
    Expect(Has(late.lines, "HANG report") && Has(late.lines, "in pipeline for"), "a pipeline stuck for 20 s is reported");
    compiler.Release();
  }
  (void)gd::names;
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
void ExpectIn(const char* path, const std::vector<std::string>& parts) {
  const std::string text = Read(path);
  Expect(!text.empty(), std::string("read ") + path);
  for (const auto& part : parts) Expect(text.find(part) != std::string::npos, std::string(path) + " has: " + part);
}

void CheckHeartbeat() {
  hw::ui_loop.Mark(hw::kUiPaint);
  hw::gpu_thread.Mark(hw::kGpuExecute);
  hw::Submitted(42, 41);
  hw::log_appended = 120;
  hw::log_written = 95;
  Blocked gpu("gpu", "fence", 42);
  hw::BeatInput in;
  in.start_ms = hw::NowMs() - 12500;
  in.now_ms = hw::NowMs();
  in.frames = 700;
  in.swaps = 705;
  in.ring_read = 0x104B;
  in.ring_write = 0x1057;
  const std::string line = hw::HeartbeatLine(in);
  std::printf("   %s\n", line.c_str());
  Expect(line.rfind("HANG beat +12.50 s: ui ", 0) == 0, "beat: time since the start");
  Expect(line.find("(paint) gpu ") != std::string::npos && line.find("(executing the ring)") != std::string::npos,
         "beat: both loops and their phase");
  Expect(line.find("frames 700 swaps 705 ring 104B/1057 sub 42 (") != std::string::npos, "beat: frames, ring, submission");
  Expect(line.find("log 120/95") != std::string::npos, "beat: the log writer's progress");
  Expect(line.find(" gpu:fence:") != std::string::npos, "beat: the call in progress");
  gpu.Release();
  Expect(hw::HeartbeatLine(in).find("calls: none") != std::string::npos, "beat: no call in progress");
  // Candidate 11: memory.
  Expect(line.find(" mem: direct free -1 MiB, flexible free -1 MiB;") != std::string::npos, "beat: memory not asked");
  in.direct_free = int64_t(1500) << 20;
  in.flexible_free = int64_t(310) << 20;
  in.guest_4k = uint64_t(120) << 20;
  in.guest_64k = uint64_t(260) << 20;
  in.guest_physical = uint64_t(400) << 20;
  const std::string with_memory = hw::HeartbeatLine(in);
  std::printf("   %s\n", with_memory.c_str());
  Expect(with_memory.find(" mem: direct free 1500 MiB, flexible free 310 MiB; guest heaps 4K 120 64K 260 phys 400 MiB, "
                          "allocated v ") != std::string::npos,
         "beat: the system's and the guest's memory");
  Expect(with_memory.size() < 380, "beat: fits the raw writer's 380 bytes");
}

void CheckGuestAllocations() {
  static std::vector<std::string> raw;
  hw::raw_sink.store(+[](const char* text, size_t size) { raw.emplace_back(text, size); });
  Expect(hw::GuestAllocation("NtAllocateVirtualMemory", false, 0x40000000, 0x10000, 0x1000, true).empty() && raw.empty(),
         "a small allocation: counted, no line");
  Expect(hw::guest_memory.virtual_calls == 1 && hw::guest_memory.virtual_bytes == 0x10000, "the virtual total");
  const std::string big = hw::GuestAllocation("MmAllocatePhysicalMemoryEx", true, 0xA0000000, 96u << 20, 0x20000000, true);
  std::printf("   %s\n", big.c_str());
  Expect(raw.size() == 1 && raw.back() == big, "an allocation of 8 MiB or more: a raw line");
  Expect(big.find("GUEST MmAllocatePhysicalMemoryEx allocated: A0000000, 100663296 bytes (96 MiB), type 20000000; "
                  "so far virtual 0 MiB in 1 calls, physical 96 MiB in 1 calls, 0 failed") != std::string::npos,
         "the big allocation's line");
  hw::GuestAllocation("NtAllocateVirtualMemory", false, 0, 0x2000, 0x1000, false);
  Expect(raw.size() == 2 && raw.back().find("GUEST NtAllocateVirtualMemory FAILED: 00000000, 8192 bytes") == 0 &&
             raw.back().find(", 1 failed") != std::string::npos,
         "a failed allocation, even a small one: a raw line");
  Expect(hw::guest_memory.failures == 1 && hw::guest_memory.virtual_calls == 1, "a failure is not added to the total");
  hw::BeatInput in;
  Expect(hw::HeartbeatLine(in).find("allocated v 0 p 96 MiB, 1 failed") != std::string::npos, "the beat has the totals");
  hw::raw_sink.store(nullptr);
}

// Candidate 21: the guest's kernel calls in the beat (shim_utils.h notes every export call).
void CheckGuestCalls() {
  const uint64_t before = hw::guest_calls.load();
  const char* const names[] = {"NtCreateFile", "NtReadFile", "KeWaitForSingleObject", "XamLoaderGetLaunchData",
                               "NtClose", "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "KeSetEvent",
                               "NtQueryFullAttributesFile", "XexGetModuleHandle"};
  for (const char* name : names) hw::NoteGuestCall(name);
  const std::string line = hw::GuestLine(9.75, before, 0.25);
  std::printf("   %s\n", line.c_str());
  Expect(line.rfind("HANG guest +9.75 s: ", 0) == 0, "guest line: time");
  Expect(line.find(" kernel calls (40/s), last: XexGetModuleHandle, NtQueryFullAttributesFile, KeSetEvent, "
                   "RtlLeaveCriticalSection, RtlEnterCriticalSection, NtClose, XamLoaderGetLaunchData, "
                   "KeWaitForSingleObject") != std::string::npos,
         "guest line: the rate and the last 8 calls, newest first");
  Expect(line.find("NtCreateFile") == std::string::npos, "guest line: older calls drop out");
  const std::string shim = Read(".deps/xenia-canary/src/xenia/kernel/util/shim_utils.h");
  Expect(shim.find("xbox360ps5::hang_watch::NoteGuestCall(export_entry->name);") != std::string::npos,
         "every export's trampoline notes its call");
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(main_text.find("Emit(hw::GuestLine(double(now - start) / 1000.0, guest_calls_before_, 0.25));") !=
             std::string::npos,
         "the beat writes the guest line");
}

void CheckRawSink() {
  static std::string got;
  hw::raw_sink.store(nullptr);
  hw::Raw("nobody listens");
  Expect(got.empty(), "no sink: nothing");
  hw::raw_sink.store(+[](const char* text, size_t size) { got.assign(text, size); });
  hw::Raw("GUEST DbgPrint: [XAPI RETURN VALUE] 1");
  Expect(got == "GUEST DbgPrint: [XAPI RETURN VALUE] 1", "the sink gets the line");
  hw::raw_sink.store(nullptr);
}

void CheckTree() {
  const char* core = ".deps/xenia-canary/src/xenia/";
  ExpectIn("include/xbox360ps5/gpu_diagnostics.hpp",
           {"#include \"xbox360ps5/hang_watch.hpp\"", "hang_watch::InCall in_call(names[static_cast<unsigned>(kind)]);"});
  ExpectIn((std::string(core) + "ui/vulkan/vulkan_gpu_completion_timeline.cc").c_str(),
           {"xbox360ps5::hang_watch::SetAwaitedSubmission(awaited_submission);",
            "xbox360ps5::hang_watch::InCall in_call(\"submit\");"});
  ExpectIn((std::string(core) + "ui/vulkan/vulkan_presenter.cc").c_str(),
           {"xbox360ps5::hang_watch::InCall in_call(\"acquire\");"});
  ExpectIn((std::string(core) + "gpu/command_processor.cc").c_str(),
           {"xbox360ps5::hang_watch::NameThread(\"gpu\");", "gpu_thread.Mark(xbox360ps5::hang_watch::kGpuIdle);",
            "gpu_thread.Mark(xbox360ps5::hang_watch::kGpuExecute);"});
  ExpectIn((std::string(core) + "gpu/vulkan/vulkan_command_processor.cc").c_str(),
           {"xbox360ps5::hang_watch::RecordDraw(", "xbox360ps5::hang_watch::RecordResolve(",
            "xbox360ps5::hang_watch::Submitted(submission_index,"});
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(main_text.find("class HangWatchThread") != std::string::npos, "the watch thread");
  const size_t start = main_text.find("auto hang_watch_thread = std::make_unique<HangWatchThread>(");
  const size_t loop = main_text.find("while (!context.HasQuitFromUIThread()) {", start);
  const size_t reset = main_text.find("hang_watch_thread.reset();", start);
  const size_t cancel = main_text.find("xe::ui::controller_keyboard::Cancel();", start);
  Expect(start != std::string::npos && loop != std::string::npos && loop > start, "the watch starts before the game loop");
  Expect(reset != std::string::npos && cancel != std::string::npos && reset < cancel,
         "the watch stops when the loop ends, before the exit's waits");
  {
    // The mark, then the tick and the paint (fix 146f laps the loop's parts in between).
    const size_t mark = main_text.find("ui_loop.Mark(xbox360ps5::hang_watch::kUiPaint);");
    const size_t tick = main_text.find("context.Tick();", mark);
    const size_t paint = main_text.find("window.Paint();", tick);
    const size_t sample = main_text.find("kUiSample);", paint);
    Expect(mark != std::string::npos && tick != std::string::npos && paint != std::string::npos &&
               sample != std::string::npos && tick - mark < 400 && paint - tick < 400 && sample - paint < 200,
           "the UI loop marks its Paint");
  }
  Expect(main_text.find("ui_loop.Mark(xbox360ps5::hang_watch::kUiMeasure);") != std::string::npos, "a measurement is marked");
  // Candidate 4.
  const size_t klass = main_text.find("class HangWatchThread {");
  const size_t klass_end = main_text.find("std::thread thread_;", klass);
  Expect(klass != std::string::npos && klass_end != std::string::npos, "the watch class");
  const std::string watch = main_text.substr(klass, klass_end - klass);
  // Candidate 20: a file a run (hang-watch-<title>-<UTC time>.txt, appended), the newest 12 kept.
  for (const char* part : {"\"hang-watch.txt\"", "O_WRONLY | O_CREAT | O_APPEND", "::usleep(50000);",
                           "KeepNewestHangWatchFiles(folder_, 11);",
                           "\"hang-watch-%08X-%04d%02d%02d-%02d%02d%02d-UTC.txt\"",
                           "fd_ = ::open((folder_ / file_name_).c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);",
                           "HangWatchRawBatch(step.lines);", "next_observe = now + 50;",
                           "hw::HeartbeatLine(in)", "next_beat = now + 250;", "hang_watch_fd.store(fd_);",
                           "xbox360ps5::hang_watch::raw_sink.store(&HangWatchRaw, std::memory_order_release);",
                           "xbox360ps5::SetCrashReportMirror(fd_);", "ReadMemory(in, memory_);",
                           "if (fd_ < 0) {", "HANG watch stopped: the game loop ended",
                           "std::atexit(HangWatchAtExit);", "std::at_quick_exit(HangWatchAtQuickExit);",
                           "void Emit(const std::string& text) { HangWatchRaw(text.data(), text.size()); }"})
    Expect(watch.find(part) != std::string::npos, std::string("the watch has: ") + part);
  const size_t raw = main_text.find("void HangWatchRaw(const char* text, size_t size) {");
  Expect(raw != std::string::npos && raw < klass, "the raw writer before the watch");
  const std::string raw_body = main_text.substr(raw, main_text.find("\n}", raw) - raw);
  for (const char* part : {"(void)!::write(fd, line, bytes);", "::fsync(fd);", "sceKernelDebugOutText(0, line);",
                           "xbox360ps5::NetLogTry(line, bytes);"})
    Expect(raw_body.find(part) != std::string::npos, std::string("the raw writer has: ") + part);
  for (const char* part : {"XELOG", "Stage(", "NetLog(", "std::fopen", "lock"})
    Expect(raw_body.find(part) == std::string::npos, std::string("the raw writer never uses: ") + part);
  // Fix 93b captures the XAM module too (which notice to show): the capture list's start only.
  const size_t hook = main_text.find("xe::kernel::xam::XamModule::restart_hook = [exit_cache");
  Expect(hook != std::string::npos, "the restart hook");
  const std::string hook_body = main_text.substr(hook, main_text.find("};", hook) - hook);
  const size_t step1 = hook_body.find("hang_watch::Raw(\"Title switch: the game ends or relaunches the title");
  const size_t step2 = hook_body.find("CheckpointExitCache(exit_cache);");
  const size_t step3 = hook_body.find("Title switch: checkpoint done after");
  const size_t step4 = hook_body.find("sceSystemServiceLoadExec(");
  Expect(step1 < step2 && step2 < step3 && step3 < step4 && step4 != std::string::npos,
         "the restart's steps logged raw, around the checkpoint and before the restart");
  // Candidate 8.
  ExpectIn(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc",
           {"state.surface = regs.values[XE_GPU_REG_RB_SURFACE_INFO];", "state.color = regs.values[XE_GPU_REG_RB_COLOR_INFO];",
            "state.depth = regs.values[XE_GPU_REG_RB_DEPTH_INFO];", "regs.GetTextureFetch(xe::tzcnt(used_texture_mask));"});
  ExpectIn(".deps/xenia-canary/src/xenia/ui/vulkan/vulkan_gpu_completion_timeline.cc",
           {"\"GPU queue submit failed: {}\"", "\"GPU fence wait for submission {} failed: {}\""});
  ExpectIn(".deps/xenia-canary/src/xenia/ui/vulkan/vulkan_presenter.cc", {"\"GPU swapchain acquire failed: {}\""});
  {
    const size_t batch = main_text.find("void HangWatchRawBatch(const std::vector<std::string>& lines) {");
    Expect(batch != std::string::npos && batch < klass, "the batch writer before the watch");
    const std::string body = main_text.substr(batch, main_text.find("\n}", batch) - batch);
    Expect(body.find("(void)!::write(fd, text.data(), text.size());") != std::string::npos &&
               body.find("::fsync(fd);") != std::string::npos && body.find("NetLogTry(text.data(), text.size());") != std::string::npos &&
               body.find("lock") == std::string::npos && body.find("XELOG") == std::string::npos,
           "a report in one write and one fsync, no lock");
  }
  ExpectIn(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_strings.cc",
           {"\"GUEST DbgPrint: \" + std::string(string_util::rtrim(data.str()));",
            "if (ps5_dbgprints.fetch_add(1, std::memory_order_relaxed) < 64) {", "xbox360ps5::hang_watch::Raw(text);"});
  const std::string xam = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_info.cc");
  Expect(xam.find("Ps5TitleEnd(\"XamLoaderTerminateTitle\", std::string(), 0);") != std::string::npos &&
             xam.find("Ps5TitleEnd(\"XamLoaderTerminateTitle\"") > xam.find("void XamLoaderTerminateTitle_entry() {"),
         "XamLoaderTerminateTitle logged first");
  Expect(xam.find("Ps5TitleEnd(\"XamLoaderLaunchTitle\",") > xam.find("void XamLoaderLaunchTitle_entry("),
         "XamLoaderLaunchTitle logged first");
  for (const char* part : {"condition_variable", "Stage(", "XELOG", "std::fopen", "::sleep(", "wait_for(",
                           "raw_sink.store(nullptr"})
    Expect(watch.find(part) == std::string::npos, std::string("the watch never uses: ") + part);
  ExpectIn(".deps/xenia-canary/src/xenia/base/logging.cc",
           {"xbox360ps5::hang_watch::log_written.fetch_add(", "xbox360ps5::hang_watch::log_appended.fetch_add("});
  ExpectIn("platform/ps5/net_log.cpp",
           {"bool NetLogTry(const char* text, size_t size) {", "std::unique_lock<std::mutex> lock(guard, std::try_to_lock);"});
  ExpectIn("include/xbox360ps5/crash_report.hpp", {"bool NetLogTry(const char* text, size_t size);"});
  ExpectIn("platform/ps5/crash_report.cpp",
           {"constexpr int kEndingSignals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGXCPU, SIGXFSZ, SIGPIPE};",
            "for (int signal : kEndingSignals) sigaction(signal, &ending, nullptr);",
            "if (number == SIGPIPE) return;", "raise(number);"});
  // Candidate 11.
  {
    // hang-watch.txt stays open after the game loop (the shader dumps close their own files).
    const size_t destructor = watch.find("~HangWatchThread() {");
    const std::string body = destructor == std::string::npos ? std::string()
                                                              : watch.substr(destructor, watch.find("\n  }", destructor) - destructor);
    Expect(!body.empty() && body.find("close(") == std::string::npos && body.find("hang_watch_fd.store(-1)") == std::string::npos,
           "the watch's file stays open when the watch stops");
  }
  ExpectIn("platform/ps5/crash_report.cpp",
           {"std::atomic<int> mirror_file{-1};", "if (fd >= 0) { (void)!write(fd, text, size); fsync(fd); }",
            "Mirror(line, used);", "Mirror(text, bytes);", "Mirror(text, size);", "Mirror(buffer, size);",
            "void SetCrashReportMirror(int fd) { mirror_file.store(fd); }"});
  ExpectIn("include/xbox360ps5/crash_report.hpp", {"void SetCrashReportMirror(int fd);"});
  {
    const std::string define = "#define sceSystemServiceLoadExec(path, argv) LoadExecLogged(path, argv, __LINE__)";
    const size_t at = main_text.find(define);
    Expect(at != std::string::npos, "LoadExec is logged");
    const size_t logged = main_text.find("int LoadExecLogged(const char* path, const char** argv, int line) {");
    Expect(logged != std::string::npos && logged < at &&
               main_text.find("return (sceSystemServiceLoadExec)(path, argv);", logged) < at,
           "the logger calls the system's LoadExec itself");
    size_t uses = 0, early = 0;
    for (size_t n = main_text.find("sceSystemServiceLoadExec("); n != std::string::npos;
         n = main_text.find("sceSystemServiceLoadExec(", n + 1)) {
      if (main_text.compare(n - 4, 4, "int ") == 0 || main_text.compare(n - 8, 8, "#define ") == 0) continue;
      ++uses;
      if (n < at) ++early;
    }
    Expect(uses >= 6 && early == 0, "every LoadExec of the title comes after the logger");
  }
  Expect(main_text.find("xbox360ps5::Stage(\"PROCESS window close requested: the game loop ends\");") != std::string::npos,
         "the window's close is logged");
  Expect(main_text.find("in.direct_free = int64_t(direct_free);") != std::string::npos &&
             main_text.find("in.guest_physical = reserved(memory->GetPhysicalHeap());") != std::string::npos,
         "the beat reads the memory");
  ExpectIn(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc",
           {"static dword_result_t NtAllocateVirtualMemory_impl(", "GuestAllocation(\n        \"NtAllocateVirtualMemory\", false,",
            "GuestAllocation(\"MmAllocatePhysicalMemoryEx\", true,", "GuestAllocation(\"MmAllocatePhysicalMemory\", true,"});
  ExpectIn(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_hal.cc",
           {"xbox360ps5::hang_watch::Raw(\"GUEST HalReturnToFirmware: \" + exitMessage);"});
  ExpectIn(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_debug.cc",
           {"xbox360ps5::hang_watch::Raw(\"GUEST KeBugCheck: \" + msg);"});
  ExpectIn(".deps/xenia-canary/src/xenia/kernel/kernel_state.cc",
           {"xbox360ps5::hang_watch::Raw(\"Kernel: TerminateTitle\");", "if (thread->main_thread()) {",
            "\"GUEST main thread {:08X} ended\""});
  ExpectIn(".deps/xenia-canary/src/xenia/base/logging.cc",
           {"xbox360ps5::hang_watch::Raw(\"PROCESS FatalError: \" + std::string(str));"});
}

}  // namespace

int main() {
  CheckSlots();
  CheckWork();
  CheckHeartbeat();
  CheckRawSink();
  CheckGuestCalls();
  CheckGuestAllocations();
  CheckWatcher();
  CheckTree();
  if (failures) {
    std::printf("check-hang-watch: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-hang-watch: ok\n");
  return 0;
}
