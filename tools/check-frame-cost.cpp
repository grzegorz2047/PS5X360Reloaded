// SPDX-License-Identifier: MIT
// Fix 49 (tools/source-1.0.2/fixes/49-frame-cost.patch): where a frame's time goes, for Call of
// Duty (MW2, Black Ops: about 20 FPS) and Battlefield 3 (25-30 FPS), which -O3, ThinLTO and the
// JIT backports did not speed up.
//  (a) include/xbox360ps5/frame_cost.hpp: the GPU's busy time as the union of submissions'
//      timestamp intervals (overlaps counted once, a wrapped interval skipped, the valid bits
//      masked, the tick period applied); the GPU thread's fence waits by cause (scopes nest and
//      restore), other threads' apart; pipeline waits timed on the GPU thread only.
//  (b) the summary's "Frame time:" line: each figure per second, the waits taken out of the
//      executing time, and the reading (GPU-bound, synchronisation, shader compilation, the
//      command thread's own work, the guest's CPU time, frames in flight) for made-up seconds.
//  (c) the assembled tree has the hooks: the two Vulkan functions in the device table, the fence
//      wait timed in the completion timeline, the GPU thread marked and its executing and idle
//      time, the timestamp pool made, written at a submission's start and end and read once it
//      completed, the causes around the waits (frames in flight, memexport readback, resolve
//      readback, queries), the pipeline waits, and the line in the 30-second summary.
//  (d) 1.0.4_candidate_6 (candidate 3: the GPU thread busy 800-900 ms/s on its own work, the GPU
//      ~150 ms/s): exclusive sections timed with the time-stamp counter (a nested section pauses
//      the outer one; only the GPU thread counts), the detail line converting ticks to ms with the
//      steady clock, the rest of executing; the hooks: draws, resolves, submissions, the memexport
//      readback, uploads, texture requests, fence waits; the build's symbol list (eboot-symbols.txt)
//      in the artifact, to name the guide's "Measure performance" offsets.
// Fix 138: "executing" also held the GPU thread's WAIT_REG_MEM sleeps (Red Dead Redemption with
// fix 134c: 989 ms/s either way while those sleeps went from 39-47 to 206-208 ms/s).
//  (e) the sleeps timed on the GPU thread only, taken out of "executing" and shown after "idle
//      for the guest" as "waiting for the guest (WAIT_REG_MEM)", the old figure the sum of the
//      two; the reading counts them with the idle time; the line without sleeps unchanged.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../include/xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fc = xbox360ps5::frame_cost;

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void ResetTotals() {
  for (uint32_t c = 0; c < fc::kCauses; ++c) {
    fc::totals.wait_ns[c] = 0;
    fc::totals.waits[c] = 0;
  }
  fc::totals.execute_ns = fc::totals.idle_ns = fc::totals.other_wait_ns = fc::totals.other_waits = 0;
  fc::totals.guest_wait_ns = fc::totals.guest_waits = 0;  // fix 138
  fc::totals.gpu_busy_ns = fc::totals.gpu_samples = fc::totals.submissions = 0;
}

void CheckGpuBusy() {
  ResetTotals();
  fc::GpuBusy busy;
  busy.SetPeriod(1.0, 64);
  Expect(busy.Add(0, 100) == 100, "first interval");
  Expect(busy.Add(50, 150) == 50, "an overlap counts once");
  Expect(busy.Add(120, 140) == 0, "inside the previous: nothing more");
  Expect(busy.Add(200, 250) == 50, "a gap is not busy");
  Expect(busy.Add(300, 290) == 0, "a wrapped interval is skipped");
  Expect(fc::totals.gpu_busy_ns == 200 && fc::totals.gpu_samples == 4 && fc::totals.submissions == 5, "totals");
  fc::GpuBusy masked;
  masked.SetPeriod(10.0, 36);  // 36 valid bits, 10 ns a tick
  Expect(masked.Add((uint64_t(1) << 40) | 5, (uint64_t(1) << 40) | 25) == 200, "bits above the valid ones ignored, period");
}

void CheckWaits() {
  ResetTotals();
  // Another thread (the UI thread's presenter): apart from the GPU thread's causes.
  std::thread([] { fc::AddFenceWait(7000); }).join();
  Expect(fc::totals.other_wait_ns == 7000 && fc::totals.waits[fc::kOther] == 0, "other threads' waits apart");
  std::thread([] {
    fc::MarkGpuThread();
    fc::AddFenceWait(1000);  // no cause: other
    {
      fc::CauseScope readback(fc::kMemexport);
      fc::AddFenceWait(2000);
      {
        fc::CauseScope throttle(fc::kThrottle);
        fc::AddFenceWait(3000);
      }
      fc::AddFenceWait(4000);  // the outer cause again
    }
    fc::AddFenceWait(500);
    { fc::WaitTimer pipeline(fc::kPipeline); }
  }).join();
  Expect(fc::totals.wait_ns[fc::kOther] == 1500 && fc::totals.waits[fc::kOther] == 2, "no cause: other");
  Expect(fc::totals.wait_ns[fc::kMemexport] == 6000 && fc::totals.waits[fc::kMemexport] == 2, "memexport readback");
  Expect(fc::totals.wait_ns[fc::kThrottle] == 3000, "a nested cause, then restored");
  Expect(fc::totals.waits[fc::kPipeline] == 1, "a pipeline wait on the GPU thread");
  { fc::WaitTimer not_gpu(fc::kPipeline); }
  Expect(fc::totals.waits[fc::kPipeline] == 1, "no pipeline wait counted off the GPU thread");
}

// A made-up 30 s for the summary: values in ms per second.
std::string Line(double gpu_busy, double execute, double idle, double throttle, double memexport, double pipelines,
                 bool timestamps = true) {
  ResetTotals();
  fc::Reporter reporter;
  (void)reporter.Line(30, timestamps);  // starts the window here
  const auto ns = [](double ms_per_s) { return uint64_t(ms_per_s * 30 * 1e6); };
  fc::totals.gpu_busy_ns = ns(gpu_busy);
  fc::totals.gpu_samples = 3000;
  fc::totals.submissions = 3000;
  fc::totals.wait_ns[fc::kThrottle] = ns(throttle);
  fc::totals.waits[fc::kThrottle] = throttle ? 600 : 0;
  fc::totals.wait_ns[fc::kMemexport] = ns(memexport);
  fc::totals.waits[fc::kMemexport] = memexport ? 9000 : 0;
  fc::totals.wait_ns[fc::kPipeline] = ns(pipelines);
  fc::totals.waits[fc::kPipeline] = pipelines ? 30 : 0;
  fc::totals.execute_ns = ns(execute + throttle + memexport + pipelines);
  fc::totals.idle_ns = ns(idle);
  const std::string line = reporter.Line(30, timestamps);
  std::printf("   %s\n", line.c_str());
  return line;
}

void CheckLine() {
  std::string line = Line(400, 250, 100, 50, 300, 0);
  Expect(Contains(line, "Frame time: GPU busy 400 ms/s over 100 submissions/s"), "GPU busy");
  Expect(Contains(line, "GPU thread executing 250 ms/s (waits taken out), idle for the guest 100 ms/s"), "own time");
  Expect(Contains(line, "waiting for the GPU 350 ms/s (frames in flight 50 in 20/s, memexport readback 300 in 300/s)"),
         "waits by cause");
  Expect(Contains(line, "reading: synchronisation"), "readbacks: synchronisation");
  Expect(Contains(Line(920, 300, 50, 500, 0, 0), "reading: GPU-bound (the GPU is busy"), "GPU busy: GPU-bound");
  Expect(Contains(Line(300, 200, 650, 0, 0, 0), "reading: the guest's CPU time"), "idle GPU thread: guest CPU");
  Expect(Contains(Line(300, 700, 50, 0, 0, 0), "reading: the GPU command thread's own work"), "busy command thread");
  Expect(Contains(Line(300, 200, 100, 0, 0, 400), "reading: shader compilation"), "pipelines");
  Expect(Contains(Line(500, 200, 100, 500, 0, 0, false), "GPU busy not measured (no timestamps)"), "no timestamps");
  Expect(Contains(Line(500, 200, 100, 500, 0, 0, false), "reading: GPU-bound (the GPU thread waits for frames in flight)"),
         "no timestamps: frames in flight");
}

// Fix 138: the GPU thread's WAIT_REG_MEM sleeps out of "executing".
void CheckGuestWait() {
  ResetTotals();
  {
    fc::WaitRegTimer not_gpu(true, 0x200);
    not_gpu.Missed();
    not_gpu.AfterSleep(not_gpu.BeforeSleep());
  }
  Expect(fc::totals.guest_waits == 0 && fc::totals.guest_wait_ns == 0, "fix 138: sleeps counted on the GPU thread only");
  std::thread([] {
    fc::MarkGpuThread();
    fc::WaitRegTimer slept(true, 0x200);
    for (int n = 0; n < 2; ++n) {
      slept.Missed();
      const uint64_t before = slept.BeforeSleep();
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      slept.AfterSleep(before);
    }
  }).join();
  Expect(fc::totals.guest_waits == 2 && fc::totals.guest_wait_ns >= 6000000 && fc::totals.guest_wait_ns < 200000000,
         "fix 138: two sleeps of 3 ms timed (" + std::to_string(fc::totals.guest_wait_ns) + " ns)");
  // Red Dead Redemption with fix 134c, mode 1: executing 989 ms/s with 207 ms/s of sleeps.
  ResetTotals();
  fc::Reporter reporter;
  (void)reporter.Line(30, true);
  const auto ns = [](double ms_per_s) { return uint64_t(ms_per_s * 30 * 1e6); };
  fc::totals.gpu_busy_ns = ns(304);
  fc::totals.gpu_samples = fc::totals.submissions = 6300;
  fc::totals.execute_ns = ns(989);
  fc::totals.idle_ns = ns(12);
  fc::totals.guest_wait_ns = ns(207);
  fc::totals.guest_waits = 6000;
  const std::string line = reporter.Line(30, true);
  std::printf("   %s\n", line.c_str());
  Expect(Contains(line, "GPU thread executing 782 ms/s (waits taken out), idle for the guest 12 ms/s, "
                        "waiting for the guest (WAIT_REG_MEM) 207 ms/s in 200 sleeps/s, waiting for the GPU 0 ms/s"),
         "fix 138: the sleeps out of executing, on their own (782 + 207 = the old 989)");
  Expect(Contains(line, "reading: the GPU command thread's own work"), "fix 138: still the command thread at 782 ms/s");
  // A thread that mostly sleeps for the guest: the reading says so.
  ResetTotals();
  (void)reporter.Line(30, true);
  fc::totals.gpu_busy_ns = ns(300);
  fc::totals.gpu_samples = fc::totals.submissions = 3000;
  fc::totals.execute_ns = ns(800);
  fc::totals.idle_ns = ns(100);
  fc::totals.guest_wait_ns = ns(450);
  fc::totals.guest_waits = 3000;
  Expect(Contains(reporter.Line(30, true), "reading: the guest's CPU time"),
         "fix 138: idle plus the WAIT_REG_MEM sleeps read as the guest's CPU time");
  // Without sleeps the line is as before (0 in 0 sleeps/s).
  Expect(Contains(Line(400, 250, 100, 50, 300, 0),
                  "GPU thread executing 250 ms/s (waits taken out), idle for the guest 100 ms/s, "
                  "waiting for the guest (WAIT_REG_MEM) 0 ms/s in 0 sleeps/s, waiting for the GPU 350 ms/s"),
         "fix 138: no sleeps, the same figures");
}

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
size_t Count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
  return n;
}
void ExpectIn(const std::string& path, const std::vector<std::string>& parts) {
  const std::string text = Read(path);
  Expect(!text.empty(), "read " + path);
  for (const auto& part : parts) Expect(Contains(text, part), path + " has: " + part);
}

void Spin(uint64_t ns) {
  const uint64_t until = fc::NowNs() + ns;
  while (fc::NowNs() < until) {
  }
}

void CheckSections() {
  for (uint32_t n = 0; n < fc::kSections; ++n) fc::section_ticks[n] = fc::section_count[n] = 0;
  fc::SectionReporter reporter;
  (void)reporter.Line(1, 0);  // starts the window
  { fc::SectionScope not_gpu(fc::kDraw); }
  Expect(fc::section_count[fc::kDraw] == 0, "sections count on the GPU thread only");
  const uint64_t start = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    fc::SectionScope draw(fc::kDraw);
    Spin(20000000);  // 20 ms drawing
    {
      fc::SectionScope submit(fc::kSubmit);
      Spin(10000000);  // 10 ms submitting
      {
        fc::SectionScope wait(fc::kWait);
        Spin(30000000);  // 30 ms waiting
      }
    }
    Spin(10000000);  // 10 ms more drawing
  }).join();
  const double seconds = double(fc::NowNs() - start) / 1e9;
  const std::string line = reporter.Line(seconds, uint64_t(80000000));  // 80 ms executing
  std::printf("   %s\n", line.c_str());
  const auto ms = [&](const char* name) {
    const size_t at = line.find(name);
    if (at == std::string::npos) return -1.0;
    return std::atof(line.c_str() + at + std::strlen(name) + 1) * seconds;
  };
  const double draw = ms("draws"), submit = ms("submissions"), wait = ms("GPU waits");
  Expect(draw > 25 && draw < 40, "draws: 30 ms of their own, not the nested 40 (" + std::to_string(draw) + ")");
  Expect(submit > 7 && submit < 15, "submissions: 10 ms without the wait inside (" + std::to_string(submit) + ")");
  Expect(wait > 25 && wait < 40, "the wait: 30 ms (" + std::to_string(wait) + ")");
  Expect(fc::section_count[fc::kDraw] == 1 && fc::section_count[fc::kSubmit] == 1 && fc::section_count[fc::kWait] == 1,
         "one of each");
  Expect(line.find("Frame time detail (GPU thread, exclusive): draws ") == 0, "the detail line");
  Expect(line.find("the rest of executing (packets, registers, constants) ") != std::string::npos, "the rest");
}

void CheckTree() {
  const std::string x = ".deps/xenia-canary/src/xenia/";
  ExpectIn(x + "ui/vulkan/functions/device_1_0.inc",
           {"XE_UI_VULKAN_FUNCTION(vkCmdWriteTimestamp)", "XE_UI_VULKAN_FUNCTION(vkGetQueryPoolResults)"});
  ExpectIn(x + "ui/vulkan/vulkan_gpu_completion_timeline.cc",
           {"const uint64_t frame_cost_start = xbox360ps5::frame_cost::NowNs();",
            "xbox360ps5::frame_cost::AddFenceWait(xbox360ps5::frame_cost::NowNs() -"});
  ExpectIn(x + "gpu/command_processor.cc",
           {"xbox360ps5::frame_cost::MarkGpuThread();", "xbox360ps5::frame_cost::totals.idle_ns.fetch_add(",
            "xbox360ps5::frame_cost::totals.execute_ns.fetch_add("});
  const std::string vk = Read(x + "gpu/vulkan/vulkan_command_processor.cc");
  Expect(Contains(vk, "pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;"), "the timestamp pool");
  Expect(Count(vk, "dfn.vkCmdWriteTimestamp(command_buffer.buffer,") == 2, "a timestamp at a submission's start and end");
  const size_t begin = vk.find("VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT");
  const size_t execute = vk.find("deferred_command_buffer_.Execute(command_buffer.buffer);", begin);
  const size_t end = vk.find("VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT", execute);
  const size_t close = vk.find("dfn.vkEndCommandBuffer(command_buffer.buffer)", end);
  Expect(begin != std::string::npos && execute != std::string::npos && end != std::string::npos &&
             close != std::string::npos,
         "start before the recorded commands, end after them, before the buffer ends");
  Expect(Contains(vk, "if (!submission || submission > completed) {"), "read only once completed");
  Expect(Count(vk, "xbox360ps5::frame_cost::kMemexport);") == 1, "the memexport readback wait");
  Expect(Count(vk, "xbox360ps5::frame_cost::kResolve);") == 3, "the resolve readback waits");
  Expect(Count(vk, "xbox360ps5::frame_cost::kQuery);") == 3, "the query waits");
  Expect(Contains(vk, "is_opening_frame ? xbox360ps5::frame_cost::kThrottle"), "the frames-in-flight wait");
  Expect(Count(Read(x + "gpu/vulkan/vulkan_pipeline_cache.cc"), "xbox360ps5::frame_cost::WaitTimer frame_cost_wait") == 2,
         "both pipeline waits timed");
  // Candidate 6.
  ExpectIn(x + "gpu/vulkan/vulkan_command_processor.cc",
           {"xbox360ps5::frame_cost::kDraw);", "xbox360ps5::frame_cost::kCopy);", "xbox360ps5::frame_cost::kSubmit);",
            "xbox360ps5::frame_cost::kReadback);"});
  ExpectIn(x + "gpu/vulkan/vulkan_shared_memory.cc", {"xbox360ps5::frame_cost::kUpload);"});
  ExpectIn(x + "gpu/texture_cache.cc", {"xbox360ps5::frame_cost::kTexture);"});
  ExpectIn(x + "ui/vulkan/vulkan_gpu_completion_timeline.cc", {"xbox360ps5::frame_cost::kWait);"});
  ExpectIn("src/canary_game_main.cpp", {"static xbox360ps5::frame_cost::SectionReporter frame_cost_sections;"});
  ExpectIn("src/canary_game_main.cpp", {"static xbox360ps5::frame_cost::Reporter frame_cost_reporter;"});
}

}  // namespace

int main() {
  CheckGpuBusy();
  CheckWaits();
  CheckLine();
  CheckGuestWait();
  CheckSections();
  CheckTree();
  if (failures) {
    std::printf("check-frame-cost: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-frame-cost: ok\n");
  return 0;
}
