// SPDX-License-Identifier: MIT
// Fix 130 (include/xbox360ps5/frame_cost.hpp, vulkan_command_processor.cc): where a submission's
// time goes on the GPU thread, to measure what an asynchronous submit thread (F2) could take off
// it. Red Dead Redemption on 1.0.5-work-fix102-111: submissions ~100 ms/s at 2.3 ms each; Modern
// Warfare 2 on 1.0.5-work-fix120: 170 ms/s; The Sims 3 on 1.0.5-work-fix125: 140 ms/s in 262/s.
//  (a) SubmitTimer: each mark gives the time since the previous one to its part, only on the GPU
//      thread; the replayed bytes and the submissions are counted;
//  (b) the line: the parts in ms per second, the KiB replayed per submission;
//  (c) the assembled tree: EndSubmission marks the parts in their order around the calls they
//      time (the deferred command buffer's replay between begin and replay, vkQueueSubmit's
//      fence submit before queue submit), the replayed bytes, and the summary logs the line after
//      fix 77's.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

namespace fc = xbox360ps5::frame_cost;
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
void Busy(int us) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
  while (std::chrono::steady_clock::now() < until) {
  }
}

void CheckTimer() {
  fc::SubmitPartsReporter reporter;
  reporter.Line(1.0);  // starts the period and the tick rate
  {
    std::thread other([] {
      fc::SubmitTimer timer;  // not the GPU thread: nothing counted
      Busy(2000);
      timer.Mark(fc::kSubmitReplay);
      timer.Replayed(1 << 20);
      timer.Done();
    });
    other.join();
  }
  fc::MarkGpuThread();  // as CommandProcessor::WorkerThreadMain does
  for (int n = 0; n < 10; ++n) {
    fc::SubmitTimer timer;
    Busy(100);
    timer.Mark(fc::kSubmitPrepare);
    Busy(1000);
    timer.Mark(fc::kSubmitBegin);
    timer.Replayed(64 * 1024);
    Busy(3000);
    timer.Mark(fc::kSubmitReplay);
    Busy(500);
    timer.Mark(fc::kSubmitEnd);
    Busy(2000);
    timer.Mark(fc::kSubmitQueue);
    timer.Mark(fc::kSubmitAfter);
    timer.Done();
  }
  const std::string line = reporter.Line(1.0);
  std::printf("%s\n", line.c_str());
  Expect(line.find("Frame time submissions (GPU thread, fix 130): 10/s, 64.0 KiB replayed each; prepare ") == 0,
         "the line: submissions and KiB replayed each, nothing from another thread");
  double replay = 0, queue = 0, begin = 0;
  std::sscanf(line.c_str() + line.find("begin "), "begin %lf", &begin);
  std::sscanf(line.c_str() + line.find("replay "), "replay %lf", &replay);
  std::sscanf(line.c_str() + line.find("queue submit "), "queue submit %lf", &queue);
  Expect(replay > 25 && replay < 60 && queue > 15 && queue < 40 && begin > 8 && begin < 25,
         "each part gets the time since the previous mark (replay " + std::to_string(replay) + ", queue " +
             std::to_string(queue) + ", begin " + std::to_string(begin) + ")");
  const std::string again = reporter.Line(1.0);
  Expect(again.find("0/s, 0.0 KiB replayed each; prepare 0.0 ms/s") != std::string::npos,
         "the next line counts only what came since");
  fc::gpu_thread_self.store(0);
}

void CheckTree() {
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t fn = vk.find("bool VulkanCommandProcessor::EndSubmission(bool is_swap) {");
  // The whole function (fixes 142e to 142g add lines to it; a fixed length cut it short).
  const size_t fn_end = fn != std::string::npos ? vk.find("\n}\n", fn) : std::string::npos;
  const std::string body = fn_end != std::string::npos ? vk.substr(fn, fn_end - fn) : "";
  const char* const order[] = {
      "xbox360ps5::frame_cost::SubmitTimer frame_cost_submit;",
      "EndRenderPass();",
      "frame_cost_submit.Mark(xbox360ps5::frame_cost::kSubmitPrepare);",
      "// Submit sparse binds earlier",
      "frame_cost_submit.Mark(xbox360ps5::frame_cost::kSubmitSparse);",
      "vkResetCommandPool(device, command_buffer.pool, 0)",
      "frame_cost_submit.Mark(xbox360ps5::frame_cost::kSubmitBegin);",
      "frame_cost_submit.Replayed(deferred_command_buffer_.stream_bytes());",
      "deferred_command_buffer_.Execute(command_buffer.buffer);",
      "frame_cost_submit.Mark(xbox360ps5::frame_cost::kSubmitReplay);",
      "vkEndCommandBuffer(command_buffer.buffer)",
      "frame_cost_submit.Mark(xbox360ps5::frame_cost::kSubmitEnd);",
      "completion_timeline_.AcquireFenceAndSubmit(",
      "frame_cost_submit.Mark(xbox360ps5::frame_cost::kSubmitQueue);",
      "PumpPendingRetire();",
      "frame_cost_submit.Mark(xbox360ps5::frame_cost::kSubmitAfter);",
      "frame_cost_submit.Done();",
  };
  size_t at = 0;
  bool in_order = fn != std::string::npos;
  for (const char* item : order) {
    const size_t found = body.find(item, at);
    if (found == std::string::npos) {
      std::printf("  missing or out of order: %s\n", item);
      in_order = false;
      break;
    }
    at = found + 1;
  }
  Expect(in_order, "EndSubmission marks the parts in order around what they time");
  const std::string dcb = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/deferred_command_buffer.h");
  Expect(dcb.find("size_t stream_bytes() const { return command_stream_.size() * sizeof(uintmax_t); }") !=
             std::string::npos,
         "the deferred command buffer gives its size");
  const std::string game_main = Read("src/canary_game_main.cpp");
  const size_t wait_reg = game_main.find("XELOGW(\"{}\", frame_cost_wait_reg.Line(double(summary_samples)));");
  const size_t parts = game_main.find("XELOGW(\"{}\", frame_cost_submit_parts.Line(double(summary_samples)));");
  Expect(wait_reg != std::string::npos && parts != std::string::npos && parts > wait_reg,
         "the summary logs the line after fix 77's");
}

}  // namespace

int main(int argc, char** argv) {
  CheckTimer();
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-submit-parts: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-submit-parts: ok\n");
  return 0;
}
