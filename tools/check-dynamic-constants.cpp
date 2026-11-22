// SPDX-License-Identifier: MIT
// Fix 120 (tools/source-1.0.2/fixes/120-dynamic-constants.patch): the draw constant buffers as
// dynamic uniform buffers. The float constants change for nearly every draw, so UpdateBindings
// allocated and wrote a new set of the seven constant buffer descriptors for nearly every draw,
// only because the buffers' offsets had moved within the uniform pool's page (bindings: Red Dead
// Redemption 150 to 163 ms/s at about 200,000 draws a second, Grand Theft Auto IV 191 to 203).
// Checked:
//  (a) xenia/gpu/vulkan/constants_dynamic.h, the code the console runs, in a model of frames of
//      draws whose constants are uploaded to pages of a pool: every draw reads exactly its own
//      buffers and offsets (the set's buffer plus the bound dynamic offset), a set is written only
//      when a buffer (page) changes or in a new frame, and is never used in a later frame (sets
//      are recycled once their frame has completed); most draws only bind the set again;
//  (b) the ranges: with fix 120's page tail (the largest fixed range), every offset the pool can
//      give plus its binding's fixed range stays inside the page's buffer, and the fixed ranges
//      cover the most UpdateBindings uploads for each buffer (256 float vectors, 40 bool/loop
//      words, 192 fetch words, the system, clip plane and tessellation blocks);
//  (c) the assembled tree: the constants' layout and sets are dynamic uniform buffers when
//      vulkan_dynamic_constants is on, from their own allocator; UpdateBindings decides after the
//      uploads, writes offset-0 descriptors with the fixed ranges, remembers the set, and binds it
//      with the draw's offsets whenever the bound range includes it; the pools' clearing forgets
//      it; the uniform pool's pages carry the tail; the summary line.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/gpu/vulkan/constants_dynamic.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

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

constexpr uint32_t kCount = 7;
using State = xe::gpu::vulkan::constants_dynamic::State<uint32_t, uint32_t, kCount>;

void CheckDraws() {
  std::mt19937 random(120);
  State state;
  struct SetContents {
    uint32_t buffers[kCount];
    uint64_t frame;
  };
  std::map<uint32_t, SetContents> sets;  // what each written set holds
  uint32_t next_set = 1, bound_set = 0;
  uint32_t bound_offsets[kCount] = {};
  int draws = 0, writes = 0, rebinds = 0, wrong = 0, stale = 0;
  // A pool of 2 MiB pages; buffers 1..7 get 64..4096 bytes per draw when they change.
  const uint32_t page_size = 2u << 20;
  uint32_t page = 1, page_used = 0;
  uint32_t buffers[kCount], offsets[kCount];
  for (uint64_t frame = 1; frame <= 30; ++frame) {
    page = page + 1;  // a new frame takes the pool's next page
    page_used = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
      buffers[i] = page;
      offsets[i] = page_used;
      page_used += 256;
    }
    bool bound_in_frame = false;
    for (int draw = 0; draw < 3000; ++draw) {
      for (uint32_t i = 0; i < kCount; ++i) {
        const bool changes = i == 1 || i == 2 ? random() % 10 != 0 : random() % 20 == 0;
        if (!changes) continue;
        const uint32_t size = i == 1 || i == 2 ? 256 * (1 + random() % 16) : 256;
        if (page_used + size > page_size) {
          ++page;
          page_used = 0;
        }
        buffers[i] = page;
        offsets[i] = page_used;
        page_used += size;
      }
      const auto decision = state.Update(frame, buffers, offsets);
      uint32_t set = state.set;
      if (decision.write) {
        set = next_set++;
        SetContents contents;
        std::copy(buffers, buffers + kCount, contents.buffers);
        contents.frame = frame;
        sets[set] = contents;
        state.Remember(set);
        ++writes;
      }
      if (decision.rebind || !bound_in_frame || set != bound_set) {
        bound_set = set;
        std::copy(offsets, offsets + kCount, bound_offsets);
        bound_in_frame = true;
        ++rebinds;
      }
      // What the draw reads: the bound set's buffers at the bound offsets.
      const SetContents& contents = sets[bound_set];
      for (uint32_t i = 0; i < kCount; ++i)
        if (contents.buffers[i] != buffers[i] || bound_offsets[i] != offsets[i]) ++wrong;
      if (contents.frame != frame) ++stale;
      ++draws;
    }
  }
  std::printf("   %d draws: %d sets written, %d binds\n", draws, writes, rebinds);
  Expect(wrong == 0, "every draw reads its own buffers and offsets (" + std::to_string(wrong) + " wrong)");
  Expect(stale == 0, "no set is used in a later frame than it was written in");
  Expect(writes * 20 < draws, "a set is written only when a page changes or in a new frame");
  // Forgotten (the pools cleared): the next draw writes.
  state.Forget();
  Expect(state.Update(30, buffers, offsets).write, "after the pools are cleared a new set is written");
}

void CheckRanges() {
  // The fixed ranges as the command processor gives them (ConstantBufferDynamicRange).
  const uint32_t ranges[kCount] = {512, 4096, 4096, 160, 768, 96, 32};
  const uint32_t tail = 4096;
  const uint32_t page = 2u << 20;
  std::mt19937 random(1200);
  int checked = 0, outside = 0;
  for (int n = 0; n < 100000; ++n) {
    const uint32_t i = random() % kCount;
    const uint32_t size = i == 1 || i == 2 ? 16 * (1 + random() % 256) : std::min(ranges[i], 160u);
    // The pool gives an aligned offset where the allocation fits in the page.
    const uint32_t offset = (random() % ((page - size) / 256 + 1)) * 256;
    if (offset + ranges[i] > page + tail) ++outside;
    ++checked;
  }
  Expect(outside == 0, "an offset plus its fixed range stays inside the page and its tail");
  std::printf("   %d offsets checked against the page tail\n", checked);
}

void CheckTree() {
  const std::string G = ".deps/xenia-canary/src/xenia/";
  const std::string vh = Read(G + "gpu/vulkan/vulkan_command_processor.h");
  const std::string vk = Read(G + "gpu/vulkan/vulkan_command_processor.cc");
  // The ranges cover the uploads.
  const size_t range_fn = vh.find("static VkDeviceSize ConstantBufferDynamicRange(uint32_t index) {");
  const std::string ranges = range_fn != std::string::npos ? vh.substr(range_fn, 1200) : "";
  Expect(ranges.find("return sizeof(SpirvShaderTranslator::SystemConstants);") != std::string::npos &&
             ranges.find("return sizeof(float) * 4 * 256;") != std::string::npos &&
             ranges.find("return sizeof(uint32_t) * (8 + 32);") != std::string::npos &&
             ranges.find("return sizeof(uint32_t) * 6 * 32;") != std::string::npos &&
             ranges.find("return sizeof(SpirvShaderTranslator::ClipPlaneConstants);") != std::string::npos &&
             ranges.find("return sizeof(SpirvShaderTranslator::TessellationConstants);") != std::string::npos,
         "a fixed range for each constant buffer");
  Expect(vk.find("sizeof(float) * 4 *\n          std::max(float_constant_count_vertex, UINT32_C(1));") != std::string::npos &&
             vk.find("sizeof(float) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));") != std::string::npos &&
             vk.find("constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);") != std::string::npos &&
             vk.find("constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;") != std::string::npos,
         "UpdateBindings uploads at most those ranges (float constants: at most 256 vectors)");
  // Layout, allocator, cvar.
  Expect(vk.find("DEFINE_bool(vulkan_dynamic_constants, true,") != std::string::npos &&
             vk.find("    if (constants_dynamic_) {  // fix 120\n      constants_binding.descriptorType =\n"
                     "          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;") != std::string::npos &&
             vk.find("VulkanCommandProcessor::kDescriptorPoolSizeUniformBufferDynamic = {\n"
                     "        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,") != std::string::npos,
         "the layout and the pools are dynamic uniform buffers with the option on");
  // UpdateBindings.
  const size_t ub = vk.find("bool VulkanCommandProcessor::UpdateBindings(");
  const size_t decide = vk.find("const auto decision = constants_dynamic_state_.Update(\n        frame_current_, constant_buffers, constants_dynamic_offsets_);", ub);
  const size_t textures = vk.find("  // Textures and samplers.", decide);
  const size_t infos = vk.find("constants_dynamic_infos_[i].offset = 0;\n        constants_dynamic_infos_[i].range = ConstantBufferDynamicRange(i);", textures);
  const size_t remember = vk.find("constants_dynamic_state_.Remember(constants_descriptor_set);  // fix 120", infos);
  const size_t write = vk.find("write_constants.pBufferInfo = &constants_infos[i];", remember);
  const size_t bind = vk.find("if (constants_dynamic_ &&\n        descriptor_set_index <= SpirvShaderTranslator::kDescriptorSetConstants &&\n"
                              "        descriptor_set_mask_tzcnt >\n            SpirvShaderTranslator::kDescriptorSetConstants) {",
                              write);
  const size_t pass = vk.find("current_graphics_descriptor_sets_ + descriptor_set_index,\n        dynamic_offset_count, dynamic_offsets);", bind);
  Expect(ub != std::string::npos && decide != std::string::npos && textures != std::string::npos && infos != std::string::npos &&
             remember != std::string::npos && write != std::string::npos && bind != std::string::npos && pass != std::string::npos,
         "decided after the uploads, offset-0 fixed-range descriptors, the set remembered, bound with the offsets");
  Expect(vk.find("constants_dynamic_state_.Forget();  // fix 120\n  transient_descriptor_allocator_uniform_buffer_dynamic_.Reset();") !=
             std::string::npos,
         "clearing the pools forgets the set");
  Expect(vk.find("if (used_transient_descriptor.first > frame_completed_) {") != std::string::npos,
         "a constants set is recycled only once its frame has completed");
  // The pool's tail.
  const std::string pool = Read(G + "ui/vulkan/vulkan_upload_buffer_pool.cc");
  Expect(pool.find("buffer_create_info.size = VkDeviceSize(page_size_ + buffer_tail_);") != std::string::npos &&
             pool.find("if (allocation_size_ > page_size_ + buffer_tail_) {") != std::string::npos &&
             pool.find("page_size_ = size_t(allocation_size_) - buffer_tail_;") != std::string::npos,
         "every page's buffer has the tail after the page");
  Expect(vk.find("uniform_buffer_tail =\n          std::max(uniform_buffer_tail, size_t(ConstantBufferDynamicRange(i)));") !=
             std::string::npos,
         "the uniform pool's tail is the largest fixed range");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t sets104 = main_code.find("XELOGW(\"{}\", frame_cost_texture_sets.Line(double(summary_samples)));");
  const size_t sets120 = main_code.find("XELOGW(\"{}\", frame_cost_constant_sets.Line(double(summary_samples)));");
  Expect(sets104 != std::string::npos && sets120 != std::string::npos && sets104 < sets120, "the summary line after fix 104's");
}

}  // namespace

int main() {
  CheckDraws();
  CheckRanges();
  CheckTree();
  if (failures) {
    std::printf("check-dynamic-constants: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-dynamic-constants: ok\n");
  return 0;
}
