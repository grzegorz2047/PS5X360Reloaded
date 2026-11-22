// SPDX-License-Identifier: MIT
// Fix 105 (tools/source-1.0.2/fixes/105-fetch-unchanged.patch, on top of fix 103): a fetch constant
// written with the bits it has is skipped, as an unchanged float constant is
// (vulkan_skip_redundant_constants, the default). Every fetch constant write put its texture
// binding out of sync and the fetch constant buffer out of date, changed or not; Grand Theft Auto
// IV rewrites its fetch constants for each draw, mostly unchanged, so the next draw derived those
// bindings again (fix 49's "textures": 79 ms/s in 194,000 draws a second on 1.0.5-work-fix93e) and
// uploaded the fetch constants again. Checked:
//  (a) xenia/gpu/register_run.h, the code the console runs, in draws shaped as GTA IV's: four
//      fetch constants rewritten per draw, one of them changed: only that one goes out of sync and
//      the fetch buffer is outdated only then; runs of changed fetch constants are reported as
//      runs (first..last), the last fetch constant included; with the option off every write
//      still marks its fetch constant, as before;
//  (b) the assembled tree: on the console WriteRegister skips an unchanged fetch constant before
//      anything else, with the option on only; register_run.h marks only the fetch constants it
//      writes; nothing else in the texture cache needs a rewrite to notice a change: a binding is
//      derived from the fetch constant's bits (RequestTextures, only for bindings out of sync, a
//      texture looked up only when its key changes), and an outdated or destroyed texture and a new
//      frame reset the bindings themselves.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/gpu/register_run.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
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
namespace rr = xe::gpu::register_run;

constexpr uint32_t kFetch = 1u << 4;

struct Cache {
  std::vector<uint32_t> regs = std::vector<uint32_t>(0x5003, 0);
  uint64_t maps[4] = {};
  uint32_t up_to_date = ~0u, in_sync = ~0u;
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  int fetch_uploads = 0, bindings_derived = 0;
  void Write(uint32_t start, const std::vector<uint32_t>& values, bool skip) {
    std::vector<uint32_t> be;
    for (uint32_t v : values) be.push_back(__builtin_bswap32(v));
    const rr::State state{regs.data(), skip, true, maps, maps, {1, 2, kFetch, 8}, &up_to_date};
    rr::Write(state, start, be.data(), uint32_t(be.size()),
              [this](uint32_t first, uint32_t last) {
                runs.emplace_back(first, last);
                for (uint32_t i = first; i <= last; ++i) in_sync &= ~(1u << i);
              },
              [](uint32_t, uint32_t) {});
  }
  void Draw(uint32_t used) {  // UpdateBindings and RequestTextures
    if (!(up_to_date & kFetch)) {
      ++fetch_uploads;
      up_to_date |= kFetch;
    }
    for (uint32_t i = 0; i < 32; ++i)
      if ((used & (1u << i)) && !(in_sync & (1u << i))) {
        ++bindings_derived;
        in_sync |= 1u << i;
      }
  }
};

std::vector<uint32_t> FetchWords(uint32_t texture, uint32_t frame_value) {
  return {0x10000000u | texture, 0x2000u + texture, 0x00FF00FFu, frame_value, 0, 0x200u};
}

void CheckDraws() {
  for (const bool skip : {true, false}) {
    Cache c;
    for (uint32_t draw = 0; draw < 1000; ++draw) {
      // Four textures per draw (fetch constants 0-3), rewritten every draw; constant 2's last
      // words change every 10th draw (a new material).
      for (uint32_t t = 0; t < 4; ++t) c.Write(0x4800 + 6 * t, FetchWords(t, t == 2 ? draw / 10 : 7), skip);
      c.Draw(0xF);
    }
    std::printf("   option %s: %d fetch buffer uploads, %d bindings derived in 1000 draws\n", skip ? "on" : "off",
                c.fetch_uploads, c.bindings_derived);
    if (skip) {
      Expect(c.fetch_uploads == 100 && c.bindings_derived == 4 + 99,
             "only a changed fetch constant uploads the buffer and derives its binding again");
    } else {
      Expect(c.fetch_uploads == 1000 && c.bindings_derived == 4000, "with the option off every write still marks them");
    }
  }
  // Runs: constants 3, 4, 5 and 31 change in one write of the whole range.
  Cache c;
  std::vector<uint32_t> all(192, 0);
  for (uint32_t i : {3u * 6 + 1, 4u * 6, 5u * 6 + 5, 31u * 6 + 2}) all[i] = 1;
  c.Write(0x4800, all, true);
  Expect(c.runs == std::vector<std::pair<uint32_t, uint32_t>>{{3, 5}, {31, 31}} && c.in_sync == ~0x80000038u,
         "the changed fetch constants as runs, the last one included");
  c.runs.clear();
  c.Write(0x4800, all, true);
  Expect(c.runs.empty() && !(c.in_sync & 0x38) && !(c.up_to_date & kFetch) == true, "nothing again for the same bits");
}

void CheckTree() {
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t wr = vk.find("void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {\n#if XE_PLATFORM_PS5\n"
                            "  // Fix 105:");
  const size_t skip = vk.find("  if (cvars::vulkan_skip_redundant_constants &&\n      index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&\n"
                              "      index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 &&\n      register_file_->values[index] == value) {\n"
                              "    return;\n  }\n#endif",
                              wr);
  const size_t base = vk.find("CommandProcessor::WriteRegister(index, value);", wr);
  Expect(wr != std::string::npos && skip != std::string::npos && base != std::string::npos && skip < base,
         "WriteRegister skips an unchanged fetch constant first, on the console, with the option on");
  const std::string rr_code = Read(".deps/xenia-canary/src/xenia/gpu/register_run.h");
  const size_t fetch = rr_code.find("} else if (index >= kFetchFirst && index <= kFetchLast) {");
  const size_t unchanged = rr_code.find("if (state.skip_redundant && regs[fetch] == value) {\n          continue;", fetch);
  const size_t mark = rr_code.find("written |= UINT32_C(1) << ((fetch - kFetchFirst) / 6);", unchanged);
  const size_t outdated = rr_code.find("if (written) {\n        *state.up_to_date &= ~state.buffers.fetch;", mark);
  Expect(fetch != std::string::npos && unchanged != std::string::npos && mark != std::string::npos && outdated != std::string::npos,
         "register_run.h marks only the fetch constants it writes");
  const std::string tc = Read(".deps/xenia-canary/src/xenia/gpu/texture_cache.cc");
  const size_t request = tc.find("void TextureCache::RequestTextures(uint32_t used_texture_mask) {");
  const size_t outdated_reset = tc.find("if (texture_became_outdated_.exchange(false, std::memory_order_acquire)) {", request);
  const size_t reset_call = tc.find("ResetTextureBindings();", outdated_reset);
  const size_t remaining = tc.find("uint32_t textures_remaining = used_texture_mask & ~texture_bindings_in_sync_;", reset_call);
  const size_t derive = tc.find("BindingInfoFromFetchConstant(fetch, binding.key, &binding.swizzled_signs);", remaining);
  const size_t lookup = tc.find("if (key_changed) {\n        binding.texture = FindOrCreateTexture(binding.key);", derive);
  Expect(request != std::string::npos && outdated_reset != std::string::npos && reset_call - outdated_reset < 400 &&
             remaining != std::string::npos && derive != std::string::npos && lookup != std::string::npos,
         "a binding is derived from the fetch constant's bits, only out of sync, a texture looked up when its key changes; "
         "an outdated texture resets the bindings");
  Expect(tc.find("void TextureCache::BeginFrame() {") != std::string::npos &&
             tc.find("      ResetTextureBindings();\n    }\n    // Remove the texture from the map and destroy it") != std::string::npos,
         "a new frame and a destroyed texture reset the bindings");
  const std::string th = Read(".deps/xenia-canary/src/xenia/gpu/texture_cache.h");
  size_t clears = 0;
  for (size_t at = th.find("texture_bindings_in_sync_ &="); at != std::string::npos; at = th.find("texture_bindings_in_sync_ &=", at + 1))
    ++clears;
  Expect(clears == 2, "the bindings go out of sync only through the fetch notifications and the resets");
}

}  // namespace

int main() {
  CheckDraws();
  CheckTree();
  if (failures) {
    std::printf("check-fetch-unchanged: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-fetch-unchanged: ok\n");
  return 0;
}
