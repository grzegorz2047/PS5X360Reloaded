// SPDX-License-Identifier: MIT
// Fix 73 (tools/source-1.0.2/fixes/73-draw-cost.patch): the 30-second summary splits the draws'
// own time (IssueDrawInner, the largest cost of the GPU command thread in the 3D games:
// Modern Warfare 2 448 ms/s on 1.0.4_candidate_31) into primitives, render targets, pipeline,
// dynamic state and constants, bindings, vertex and memexport buffers, recording and after.
//  (a) include/xbox360ps5/frame_cost.hpp: DrawTimer counts on the GPU thread only (one in
//      draw_sample_every draws timed, at random since fix 77, the parts scaled back to all); each mark
//      takes the time since the previous one less what nested sections (uploads, textures) took;
//      what follows the last mark counts as "after"; DrawReporter gives each part in ms/s, the
//      draws per second and the microseconds per draw, since its previous line;
//  (b) the assembled tree: the marks in IssueDrawInner in order (primitives before the render
//      targets' update, targets after it, pipeline before the dynamic state, state before the
//      bindings, bindings after them, buffers before the draw, recording after it), and the line
//      in the summary after fix 62's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
namespace fc = xbox360ps5::frame_cost;
void Spin(uint64_t ns) {
  const uint64_t end = fc::NowNs() + ns;
  while (fc::NowNs() < end) {
  }
}
double After(const std::string& line, const std::string& name) {
  const size_t at = line.find(name);
  return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + name.size());
}

void CheckTimer() {
  fc::draw_sample_every = 1;  // every draw timed here; sampling is checked below
  fc::DrawReporter reporter;
  (void)reporter.Line(1);  // starts the window
  { fc::DrawTimer not_gpu; Spin(1000000); not_gpu.Mark(fc::kDrawPipeline); }
  Expect(fc::draw_calls == 0, "counted on the GPU thread only");
  const uint64_t start = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    fc::SectionScope draw(fc::kDraw);
    for (int n = 0; n < 2; ++n) {
      fc::DrawTimer timer;
      Spin(5000000);  // 5 ms primitives
      timer.Mark(fc::kDrawPrimitive);
      {
        fc::SectionScope upload(fc::kUpload);
        Spin(20000000);  // 20 ms in a nested upload: not the draw's
      }
      Spin(4000000);  // 4 ms of render targets
      timer.Mark(fc::kDrawTargets);
      Spin(3000000);  // 3 ms after the last mark
    }
  }).join();
  const double seconds = double(fc::NowNs() - start) / 1e9;
  const std::string line = reporter.Line(seconds);
  std::printf("   %s\n", line.c_str());
  const auto ms = [&](const char* name) { return After(line, std::string(name) + " ") * seconds; };
  const double primitives = ms("primitives"), targets = ms("render targets"), after = ms("after");
  Expect(primitives > 8 && primitives < 13, "primitives: 2 x 5 ms (" + std::to_string(primitives) + ")");
  Expect(targets > 6 && targets < 11, "render targets: 2 x 4 ms, the nested upload taken out (" + std::to_string(targets) + ")");
  Expect(after > 4 && after < 9, "after: 2 x 3 ms (" + std::to_string(after) + ")");
  Expect(line.find("Frame time draws (GPU thread, own time by part): ") == 0, "the line");
  Expect(fc::draw_calls == 2, "two draws");
  const double each = After(line, "draws/s, ");
  Expect(each > 10000 && each < 14000, "about 12 ms of own time per draw (" + std::to_string(each) + " us)");
  Expect(reporter.Line(1).find(": 0 draws/s") != std::string::npos, "only since the previous line");

  // Sampling: one in 4 draws timed at random (fix 77), the parts scaled back to all draws.
  fc::draw_sample_every = 4;
  fc::DrawReporter sampled;
  (void)sampled.Line(1);
  const uint64_t sampled_before = fc::draw_sampled;
  const uint64_t begin = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    fc::SectionScope draw(fc::kDraw);
    for (int n = 0; n < 400; ++n) {
      fc::DrawTimer timer;
      Spin(100000);  // 0.1 ms primitives each
      timer.Mark(fc::kDrawPrimitive);
    }
  }).join();
  const double span = double(fc::NowNs() - begin) / 1e9;
  const std::string scaled = sampled.Line(span);
  std::printf("   %s\n", scaled.c_str());
  const double primitives400 = After(scaled, "primitives ") * span;
  Expect(primitives400 > 28 && primitives400 < 52, "about 100 of 400 draws timed, scaled to about 40 ms (" +
                                                       std::to_string(primitives400) + ")");
  const uint64_t timed = fc::draw_sampled - sampled_before;
  Expect(timed > 60 && timed < 140, "about one in 4 draws timed (" + std::to_string(timed) + ")");
}

void CheckTree() {
  const std::string code = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t inner = code.find("bool VulkanCommandProcessor::IssueDrawInner(");
  const size_t timer = code.find("fc::DrawTimer draw_timer;  // fix 73", inner);
  size_t at = timer;
  bool ordered = timer != std::string::npos;
  for (const char* step : {"draw_timer.Mark(fc::kDrawPrimitive);", "if (!render_target_cache_->Update(",
                           "draw_timer.Mark(fc::kDrawTargets);", "pipeline_cache_->ConfigurePipeline(",
                           "draw_timer.Mark(fc::kDrawPipeline);", "UpdateDynamicState(",
                           "UpdateSystemConstantValues(", "draw_timer.Mark(fc::kDrawState);",
                           "if (!UpdateBindings(vertex_shader, pixel_shader)) {", "draw_timer.Mark(fc::kDrawBindings);",
                           "shared_memory_->RequestRange(vfetch_addresses[i] << 2,", "draw_timer.Mark(fc::kDrawBuffers);",
                           "deferred_command_buffer_.CmdVkDraw(", "deferred_command_buffer_.CmdVkDrawIndexed(",
                           "draw_timer.Mark(fc::kDrawRecord);"}) {
    const size_t next = ordered ? code.find(step, at) : std::string::npos;
    if (next == std::string::npos) {
      ordered = false;
      std::printf("   not found in order: %s\n", step);
      break;
    }
    at = next;
  }
  Expect(ordered, "the marks follow IssueDrawInner's steps");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t uploads = main_code.find("XELOGW(\"{}\", frame_cost_uploads.Line(double(summary_samples)));");
  const size_t draws = main_code.find("XELOGW(\"{}\", frame_cost_draws.Line(double(summary_samples)));");
  Expect(uploads != std::string::npos && draws != std::string::npos && uploads < draws, "the summary gives the line after fix 62's");
}

}  // namespace

int main() {
  CheckTimer();
  CheckTree();
  if (failures) {
    std::printf("check-draw-cost: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-draw-cost: ok\n");
  return 0;
}
