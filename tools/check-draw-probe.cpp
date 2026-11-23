// SPDX-License-Identifier: MIT
// Fix 142d (research/parallel-gpu-commands/STAGE2.md, step 2.0): diagnostics of the draws for
// stage 2. Where fix 73's "bindings" and "dynamic state and constants" go (UpdateBindings by part,
// vkUpdateDescriptorSets with its calls and descriptors, the constant uploads with their bytes),
// and how often a draw's primitive processing, render pass and framebuffer, pipeline and shaders
// come out as the previous draw's, with the time those calls take either way. Nothing it adds
// changes what is done.
//  (a) one draw in 8 timed on the GPU thread, every draw counted; nothing off the GPU thread;
//  (b) the parts' own time (scopes and marks), the counts of uploads, bytes and descriptor
//      updates;
//  (c) the "same as the previous draw" counts, and their time only for timed draws;
//  (d) the two lines' arithmetic and text;
//  (e) the assembled tree: the marks in UpdateBindings in its order, the draw's start, the four
//      comparisons, dynamic state and system constants, the lines after fix 73's;
//  (f) the patch only adds lines (argv[1]: the fixes folder).
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xbox360ps5/frame_cost.hpp"

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
uint64_t Load(const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); }

// (a)
void CheckSampling() {
  // Off the GPU thread: nothing counted, nothing timed.
  std::thread other([] {
    fc::BindBeginDraw();
    fc::CountBindUpload(fc::kBindSystem, 100);
    fc::CountDescriptorUpdate(3);
    fc::NoteSame(fc::kSamePipeline, true, fc::BindMark());
  });
  other.join();
  Expect(Load(fc::bind_draws) == 0 && Load(fc::bind_uploads[0]) == 0 && Load(fc::bind_update_calls) == 0 &&
             Load(fc::same_counts[fc::kSamePipeline * 2 + 1]) == 0,
         "nothing counted off the GPU thread");
  fc::MarkGpuThread();
  uint64_t active = 0;
  for (int n = 0; n < 80000; ++n) {
    fc::BindBeginDraw();
    active += fc::bind_probe_active;
  }
  Expect(Load(fc::bind_draws) == 80000 && Load(fc::bind_sampled) == active,
         "every draw counted, the timed ones counted as timed");
  Expect(active > 8000 && active < 12000, "about one draw in 8 timed (" + std::to_string(active) + ")");
}

// (b) and (c)
void CheckParts() {
  // A timed draw: a scope and a mark both add own ticks.
  fc::bind_probe_active = true;
  const uint64_t before = Load(fc::bind_part_ticks[fc::kBindUpdateSets]);
  {
    const fc::BindScope scope(fc::kBindUpdateSets);
    volatile uint64_t spin = 0;
    for (int i = 0; i < 200000; ++i) spin += i;
  }
  Expect(Load(fc::bind_part_ticks[fc::kBindUpdateSets]) > before, "a timed scope adds its ticks");
  const fc::BindMark mark = fc::BindStart();
  Expect(mark.ticks != 0, "a timed draw's mark has a start");
  fc::NoteSame(fc::kSameTargets, true, mark);
  Expect(Load(fc::same_counts[fc::kSameTargets * 2 + 1]) == 1 && Load(fc::same_timed[fc::kSameTargets * 2 + 1]) == 1,
         "a timed comparison is counted and timed");
  // An untimed draw: no ticks, still counted.
  fc::bind_probe_active = false;
  const uint64_t ticks = Load(fc::bind_part_ticks[fc::kBindBind]);
  {
    const fc::BindScope scope(fc::kBindBind);
  }
  const fc::BindMark none = fc::BindStart();
  Expect(none.ticks == 0 && Load(fc::bind_part_ticks[fc::kBindBind]) == ticks, "an untimed draw adds no ticks");
  fc::NoteSame(fc::kSameTargets, false, none);
  Expect(Load(fc::same_counts[fc::kSameTargets * 2]) == 1 && Load(fc::same_timed[fc::kSameTargets * 2]) == 0,
         "an untimed comparison is counted, not timed");
  fc::CountBindUpload(fc::kBindFloatVertex, 4096);
  fc::CountBindUpload(fc::kBindFloatVertex, 1024);
  fc::CountBindUpload(7, 10);  // not a buffer: ignored
  fc::CountDescriptorUpdate(5);
  Expect(Load(fc::bind_uploads[fc::kBindFloatVertex]) == 2 && Load(fc::bind_upload_bytes[fc::kBindFloatVertex]) == 5120 &&
             Load(fc::bind_update_calls) == 1 && Load(fc::bind_update_descriptors) == 5,
         "uploads, bytes and descriptor updates counted");
}

// (d)
void CheckLines() {
  // A reporter that has taken everything counted so far, then known counts over 30 s: 300,000
  // draws/s, 37,500 of them timed (scale 8), one tick taken as one ns, so a part's ms/s is
  // ticks * 8 / 1e6 / 30.
  fc::BindReporter fresh;
  fresh.Lines(30, 1.0);
  auto ticks_for = [](double ms_per_s) { return uint64_t(ms_per_s * 1e6 * 30 / 8); };
  fc::bind_draws.fetch_add(30 * 300000);
  fc::bind_sampled.fetch_add(30 * 300000 / 8);
  fc::bind_calls.fetch_add(30 * 310000);
  fc::bind_part_ticks[fc::kBindFloatVertex].fetch_add(ticks_for(20));
  fc::bind_part_ticks[fc::kBindFloatPixel].fetch_add(ticks_for(30));
  fc::bind_part_ticks[fc::kBindImageInfos].fetch_add(ticks_for(25));
  fc::bind_part_ticks[fc::kBindUpdateSets].fetch_add(ticks_for(60));
  fc::bind_part_ticks[fc::kBindBind].fetch_add(ticks_for(10));
  fc::bind_part_ticks[fc::kBindTotal].fetch_add(ticks_for(180));
  fc::bind_part_ticks[fc::kBindDynamicState].fetch_add(ticks_for(12));
  fc::bind_uploads[fc::kBindFloatVertex].fetch_add(30 * 290000);
  fc::bind_upload_bytes[fc::kBindFloatVertex].fetch_add(uint64_t(30) * 290000 * 1024);
  fc::bind_update_calls.fetch_add(30 * 150000);
  fc::bind_update_descriptors.fetch_add(uint64_t(30) * 150000 * 6);
  // The same pipeline as the previous draw for 60% of the draws; a tenth of each kind timed, so
  // their ticks scale by 10: 9 ms/s when the same, 3 when not.
  const uint32_t pipe = fc::kSamePipeline * 2;
  fc::same_counts[pipe + 1].fetch_add(30 * 180000);
  fc::same_counts[pipe].fetch_add(30 * 120000);
  fc::same_timed[pipe + 1].fetch_add(30 * 18000);
  fc::same_timed[pipe].fetch_add(30 * 12000);
  fc::same_ticks[pipe + 1].fetch_add(uint64_t(9 * 1e6 * 30 / 10));
  fc::same_ticks[pipe].fetch_add(uint64_t(3 * 1e6 * 30 / 10));
  const std::string text = fresh.Lines(30, 1.0);
  const size_t split = text.find('\n');
  Expect(split != std::string::npos, "two lines");
  const std::string bindings = text.substr(0, split), same = text.substr(split + 1);
  Expect(bindings.find("Frame time bindings (GPU thread, fix 142d, 1 draw in 8 timed): 300000 draws/s, 310000 "
                       "UpdateBindings/s, ") == 0,
         "the bindings line's start: " + bindings);
  Expect(bindings.find("float VS 20.0 ms/s (290000/s, 1024 B each), float PS 30.0 ms/s") != std::string::npos,
         "the float constants: " + bindings);
  Expect(bindings.find("texture image infos 25.0 ms/s") != std::string::npos &&
             bindings.find("vkUpdateDescriptorSets 60.0 ms/s in 150000 calls/s (6.0 descriptors each)") !=
                 std::string::npos &&
             bindings.find("binds 10.0 ms/s, the rest 35.0 ms/s; dynamic state 12.0 ms/s") != std::string::npos,
         "the parts, the updates and the rest (180 - 145): " + bindings);
  Expect(same.find("Frame time draws like the previous one (GPU thread, fix 142d): ") == 0 &&
             same.find("pipeline 60.0% (9.0 ms/s when the same, 3.0 when not)") != std::string::npos,
         "the draws like the previous one: " + same);
}

// (e)
void CheckTree() {
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string main = Read("src/canary_game_main.cpp");
  const size_t bind = vk.find("bool VulkanCommandProcessor::UpdateBindings(");
  Expect(bind != std::string::npos, "UpdateBindings found");
  const std::string body = vk.substr(bind, vk.find("\nuint32_t VulkanCommandProcessor::WriteTransientTextureBindings", bind) - bind);
  const char* const in_order[] = {
      "fc142d::BindScope fix142d_total(fc142d::kBindTotal)",
      "fix142d_buffer(fc142d::kBindSystem)",
      "fix142d_buffer(fc142d::kBindClip)",
      "fix142d_buffer(fc142d::kBindTessellation)",
      "fix142d_buffer(fc142d::kBindFloatVertex)",
      "CountBindUpload(fc142d::kBindFloatVertex, float_constants_size)",
      "fix142d_buffer(fc142d::kBindFloatPixel)",
      "CountBindUpload(fc142d::kBindFloatPixel, float_constants_size)",
      "fix142d_buffer(fc142d::kBindBoolLoop)",
      "fix142d_buffer(fc142d::kBindFetch)",
      "fix142d_sets(fc142d::kBindConstantSets)",
      "fix142d_infos = fc142d::BindStart()",
      "descriptor_write_image_info_.clear();",
      "BindStop(fc142d::kBindImageInfos, fix142d_infos)",
      "const uint64_t texture_set_epoch",
      "CountTextureSets(",
      "BindStop(fc142d::kBindReuseCheck, fix142d_reuse)",
      "fix142d_allocate = fc142d::BindStart()",
      "WriteTransientTextureBindings(",
      "BindStop(fc142d::kBindAllocate, fix142d_allocate)",
      "fix142d_update = fc142d::BindStart()",
      "dfn.vkUpdateDescriptorSets(",
      "BindStop(fc142d::kBindUpdateSets, fix142d_update)",
      "CountDescriptorUpdate(fix142d_descriptors)",
      "fix142d_bind = fc142d::BindStart()",
      "CmdVkBindDescriptorSets(",
      "BindStop(fc142d::kBindBind, fix142d_bind)",
  };
  size_t at = 0;
  for (const char* mark : in_order) {
    const size_t found = body.find(mark, at);
    Expect(found != std::string::npos, std::string("UpdateBindings, in order: ") + mark);
    if (found != std::string::npos) at = found;
  }
  const size_t draw = vk.find("bool VulkanCommandProcessor::IssueDrawInner(");
  const char* const draw_order[] = {
      "fc::DrawTimer draw_timer;",
      "XE_FIX142D(fc::BindBeginDraw();)",
      "fix142d_primitive = fc::BindStart()",
      "primitive_processor_->Process(primitive_processing_result)",
      "fc::NoteSame(fc::kSamePrimitive, same, fix142d_primitive)",
      "fix142d_targets = fc::BindStart()",
      "render_target_cache_->Update(",
      "fc::NoteSame(fc::kSameTargets,",
      "fix142d_pipeline = fc::BindStart()",
      "pipeline_cache_->ConfigurePipeline(",
      "fc::NoteSame(fc::kSamePipeline, pipeline == fix142d_last_pipeline, fix142d_pipeline)",
      "fc::NoteSame(fc::kSameShaders,",
      "fix142d_dynamic = fc::BindStart()",
      "UpdateDynamicState(viewport_info",
      "BindStop(fc::kBindDynamicState, fix142d_dynamic)",
      "fix142d_system = fc::BindStart()",
      "UpdateSystemConstantValues(",
      "BindStop(fc::kBindSystemConstants, fix142d_system)",
      "UpdateBindings(vertex_shader, pixel_shader)",
  };
  at = draw;
  for (const char* mark : draw_order) {
    const size_t found = vk.find(mark, at);
    Expect(draw != std::string::npos && found != std::string::npos, std::string("IssueDrawInner, in order: ") + mark);
    if (found != std::string::npos) at = found;
  }
  const size_t draws_line = main.find("frame_cost_draws.Line(double(summary_samples))");
  const size_t bind_lines = main.find("frame_cost_bindings.Lines(double(summary_samples))");
  Expect(draws_line != std::string::npos && bind_lines != std::string::npos && draws_line < bind_lines,
         "the two lines right after fix 73's");
}

// (f)
void CheckPatch(const std::string& fixes) {
  const std::string patch = Read(fixes + "/142d-draw-probe.patch");
  Expect(!patch.empty(), "the fix's patch is there");
  std::istringstream lines(patch);
  std::string line;
  int removed = 0;
  while (std::getline(lines, line)) {
    if (line.rfind("--- ", 0) == 0) continue;
    if (!line.empty() && line[0] == '-') ++removed;
  }
  Expect(removed == 0, "the patch only adds lines: what the emulator does is unchanged");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef XBOX360PS5_FRAME_COST_DIAG_SWITCH
  // Fix 142f-diag: these probes are behind its switch (gpu-diag-detail.txt), on here.
  fc::diag_detail.store(true);
#endif
  CheckSampling();
  CheckParts();
  CheckLines();
  const bool model_only = argc > 1 && std::string(argv[1]) == "--model-only";
  if (!model_only) {
    CheckTree();
    if (argc > 1) CheckPatch(argv[1]);
  }
  if (failures) {
    std::printf("check-draw-probe: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-draw-probe: ok\n");
  return 0;
}
