// SPDX-License-Identifier: MIT
// Fix 173 (tools/source-1.0.2/fixes/173-exit-cache-timing.patch): the exit checkpoint of the Vulkan
// pipeline cache notes each step (queued, taken by the GPU thread, the snapshot's data read, written)
// and its line gives them, to tell a busy GPU thread from a slow drive (GTA IV's 30 s timeout).
//  (a) include/xbox360ps5/exit_cache_timing.hpp: steps noted only between Begin and End, in ms from
//      the request, the bytes and the outcome; a timeout's line names the steps not reached;
//  (b) the tree: canary_game_main.cpp's CheckpointExitCache begins before the request and logs the
//      steps on a timeout and on completion; the GPU thread notes when it takes the request and
//      finishes; the pipeline cache notes the data read before its write.
// Run from the assembled tree's root.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/exit_cache_timing.hpp"

namespace {
namespace et = xbox360ps5::exit_cache_timing;
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Squeezed(const char* path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string out;
  for (char c : buffer.str())
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
  return out;
}

void Steps() {
  et::Timing t;
  et::Mark(t, et::kTaken, 5'000'000);
  Expect(t.at_ns[et::kTaken] == -1, "(a) nothing noted before Begin (other snapshots)");
  et::Begin(t, 1'000'000'000);
  et::Mark(t, et::kTaken, 1'012'300'000);
  et::Mark(t, et::kRead, 1'040'100'000, 31457280);
  et::Finish(t, 2'650'200'000, true);
  Expect(et::Line(t) == "queued at 0 ms; taken by the GPU thread after 12.3 ms; snapshot of 31457280 bytes read after "
                        "40.1 ms (its write starts); written after 1650.2 ms (saved)",
         "(a) all steps: " + et::Line(t));
  et::End(t);
  et::Finish(t, 9'000'000'000, false);
  Expect(t.saved == 1, "(a) nothing noted after End (a late snapshot of a timed-out checkpoint)");

  et::Timing slow;
  et::Begin(slow, 0);
  et::Mark(slow, et::kTaken, 29'000'000'000);
  Expect(et::Line(slow) == "queued at 0 ms; taken by the GPU thread after 29000.0 ms; snapshot read not reached (its "
                           "write starts); written not reached",
         "(a) a timeout: the steps not reached named: " + et::Line(slow));
  et::Timing busy;
  et::Begin(busy, 0);
  Expect(et::Line(busy).find("taken by the GPU thread not reached") != std::string::npos,
         "(a) the GPU thread never took it");
  et::Timing failed;
  et::Begin(failed, 0);
  et::Finish(failed, 1'000'000, false);
  Expect(et::Line(failed).find("(failed or unavailable)") != std::string::npos, "(a) a failed snapshot");
}

void Wiring() {
  const std::string main = Squeezed("src/canary_game_main.cpp");
  const size_t begin = main.find("exit_timing::Begin(exit_timing::Global(),exit_timing::NowNs());");
  const size_t request = main.find("autocompletion=state->processor->RequestPipelineCacheSnapshot();", begin);
  const size_t timeout = main.find("\"Vulkancache:exitcheckpointtimedoutafter{}ms\"", request);
  const size_t line_timeout = main.find("\"Vulkancache(fix173):exitcheckpointsteps:{}\"", timeout);
  const size_t done = main.find("\"Vulkancache:exitcheckpoint{}\"", line_timeout);
  const size_t line_done = main.find("\"Vulkancache(fix173):exitcheckpointsteps:{}\"", done);
  Expect(begin != std::string::npos && request != std::string::npos && timeout != std::string::npos &&
             line_timeout != std::string::npos && done != std::string::npos && line_done != std::string::npos,
         "(b) the checkpoint begins before its request and logs the steps on a timeout and on completion");
  const std::string processor = Squeezed(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t taken = processor.find("timing::Mark(timing::Global(),timing::kTaken,timing::NowNs());");
  const size_t save = processor.find("pipeline_cache_->SavePipelineCacheSnapshot();", taken);
  const size_t finish = processor.find("timing::Finish(timing::Global(),timing::NowNs(),saved);", save);
  Expect(taken != std::string::npos && save != std::string::npos && finish != std::string::npos,
         "(b) the GPU thread notes taking the request, then the outcome");
  const std::string cache = Squeezed(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc");
  const size_t read = cache.find("xbox360ps5::exit_cache_timing::kRead,xbox360ps5::exit_cache_timing::NowNs(),written);");
  const size_t write = cache.find("xbox360ps5::WriteCacheAtomically(", read);
  Expect(read != std::string::npos && write != std::string::npos, "(b) the data read noted before its write");
}
}  // namespace

int main() {
  Steps();
  if (std::ifstream("src/canary_game_main.cpp")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: the exit checkpoint's steps are timed and logged\n");
  return 0;
}
