// SPDX-License-Identifier: MIT
// Fix 142g (research/parallel-gpu-commands/STAGE2.md, section 15): repeat memos for the system
// constants, the samplers and the viewport (xenia/gpu/vulkan/draw_memo.h), and fix 142f-diag's
// diagnostic bugs.
//  (a) draw-memo.txt, the key and the slot, the line;
//  (b) the assembled tree:
//      - the system constants' key holds every register, parameter field, cache answer and
//        member UpdateSystemConstantValues reads, and the function's only return is the skip;
//      - the samplers' key holds what GetSamplerParameters reads (the fetch constant, the
//        binding's filters, anisotropic_override);
//      - the viewport's key is GetViewportInfoArgs' compared union, and GetHostViewportInfo
//        reads no setting but half_pixel_offset;
//      - each memo checked in mode 1 and committed only after a complete computation; the
//        modes read at the end of a submission, 0 by default;
//      - fix 142f-diag's bugs: the switch's share of a block, the descriptor indexing line
//        logged, the watch faults counted in the branch the PS5 build takes, the texture set
//        history out of fix 142d's reuse check span, the switch file's time logged.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include "xenia/gpu/vulkan/draw_memo.h"

namespace {

namespace dm = xe::gpu::draw_memo;
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
std::string Body(const std::string& text, const std::string& signature) {
  const size_t at = text.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = text.find('{', at + signature.size() - 1);
  int depth = 0;
  for (size_t i = open; i < text.size(); ++i) {
    if (text[i] == '{') ++depth;
    if (text[i] == '}' && --depth == 0) return text.substr(at, i + 1 - at);
  }
  return std::string();
}
bool Has(const std::string& text, const std::string& part) {
  const bool found = text.find(part) != std::string::npos;
  if (!found) std::printf("  (not found: %s)\n", part.substr(0, 100).c_str());
  return found;
}

// (a)
void CheckModel() {
  dm::Settings s;
  Expect(dm::Parse("2\n", s) == 0 && s.mode[0] == 2 && s.mode[1] == 2 && s.mode[2] == 2, "one number for all");
  Expect(dm::Parse("system 1\nsamplers 0 # off\n", s) == 0 && s.mode[0] == 1 && s.mode[1] == 0 && s.mode[2] == 2,
         "a line per memo; what a line does not set stays");
  Expect(dm::Parse("viewport 3\nfoo 1\nsystem\nsystem 1 2\n", s) == 4 && s.mode[2] == 2, "lines not understood");
  dm::Settings zero;
  Expect(dm::Parse("", zero) == 0 && zero.mode[0] == 0, "an empty file sets nothing");

  dm::Slot slot;
  dm::Key& k1 = slot.Begin();
  k1.Add(uint32_t(5));
  k1.AddBytes("abcdefg", 7);
  Expect(!slot.Predict(), "nothing predicted before a commit");
  slot.Commit();
  dm::Key& k2 = slot.Begin();
  k2.Add(uint32_t(5));
  k2.AddBytes("abcdefg", 7);
  Expect(slot.Predict(), "the same key repeats");
  dm::Key& k3 = slot.Begin();
  k3.Add(uint32_t(5));
  k3.AddBytes("abcdefh", 7);
  Expect(!slot.Predict(), "a byte differs");
  dm::Key& k4 = slot.Begin();
  k4.Add(uint32_t(5));
  Expect(!slot.Predict(), "a shorter key");
  dm::Key& k5 = slot.Begin();
  k5.Add(uint32_t(5));
  k5.AddBytes("abcdefg", 7);
  slot.Invalidate();
  Expect(!slot.Predict(), "invalidated (a mode change, a computation not finished)");

  dm::Stats before[dm::kMemos] = {}, now[dm::kMemos] = {};
  now[0] = {3000, 2700, 2700, 0};
  now[1] = {6000, 3000, 0, 4};
  before[1].mismatches = 1;
  dm::Settings mixed;
  mixed.mode[0] = 2;
  mixed.mode[1] = 1;
  const std::string line = dm::Line(mixed, now, before, 30.0);
  const std::string want =
      "Draw memo (fix 142g): system mode 2: 100/s, repeats 90%, skipped 90/s, mismatches 0 (0 in all); "
      "samplers mode 1: 200/s, repeats 50%, skipped 0/s, mismatches 3 (4 in all); viewport mode 0: 0/s, "
      "repeats 0%, skipped 0/s, mismatches 0 (0 in all)";
  Expect(line == want, "the line: " + line);
}

// (b)
std::set<std::string> RegistersIn(const std::string& text) {
  std::set<std::string> out;
  static const std::regex reg(R"(\b(?:reg::|XE_GPU_REG_)([A-Z][A-Z0-9_]+))");
  for (std::sregex_iterator it(text.begin(), text.end(), reg), end; it != end; ++it) out.insert((*it)[1]);
  return out;
}

void CheckTree() {
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string tc = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_texture_cache.cc");
  const std::string du = Read(".deps/xenia-canary/src/xenia/gpu/draw_util.cc");
  const std::string duh = Read(".deps/xenia-canary/src/xenia/gpu/draw_util.h");
  Expect(!cp.empty() && !tc.empty() && !du.empty() && !duh.empty(),
         "the tree's files are there (run from the assembled tree's root)");

  // The system constants.
  const std::string system = Body(cp, "void VulkanCommandProcessor::UpdateSystemConstantValues(");
  const size_t memo_end = system.find("auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();");
  Expect(!system.empty() && memo_end != std::string::npos, "UpdateSystemConstantValues and its memo found");
  const std::string key = system.substr(0, memo_end);
  const std::string work = system.substr(memo_end);
  std::set<std::string> keyed = RegistersIn(key);
  if (key.find("reg::RB_COLOR_INFO::rt_register_indices[i]") != std::string::npos) keyed.insert("RB_COLOR_INFO");
  if (key.find("reg::RB_BLENDCONTROL::rt_register_indices[i]") != std::string::npos) keyed.insert("RB_BLENDCONTROL");
  if (key.find("&regs.values[XE_GPU_REG_PA_CL_UCP_0_X], 6 * 4 * sizeof(uint32_t)") != std::string::npos)
    keyed.insert("PA_CL_UCP_0_X");
  for (const std::string& name : RegistersIn(work)) {
    if (name == "VGT_DRAW_INITIATOR") continue;  // the two fields below
    Expect(keyed.count(name) != 0, "the system constants' key holds " + name + ", which the update reads");
  }
  static const std::regex field(
      R"(\b(primitive_processing_result\.|viewport_info\.|vgt_draw_initiator\.|host_depth_polygon_offset->)([a-z_]+))");
  for (std::sregex_iterator it(work.begin(), work.end(), field), end; it != end; ++it) {
    const std::string object = (*it)[1], name = (*it)[2];
    if (object == "vgt_draw_initiator.") {
      Expect(Has(key, "initiator." + name), "the key holds VGT_DRAW_INITIATOR's " + name);
    } else if (object == "host_depth_polygon_offset->") {
      Expect(Has(key, "key.Add(*host_depth_polygon_offset);"), "the key holds the polygon offset");
    } else {
      Expect(Has(key, "key.Add(" + object + name + ");"), "the key holds " + object + name);
    }
  }
  static const std::regex call(R"(\b(texture_cache_|render_target_cache_)->([A-Za-z0-9_]+)\()");
  for (std::sregex_iterator it(work.begin(), work.end(), call), end; it != end; ++it) {
    Expect(Has(key, std::string((*it)[1]) + "->" + std::string((*it)[2]) + "("),
           "the key holds " + std::string((*it)[2]) + "'s answer");
  }
  static const std::regex member(R"(\b([a-z][a-z0-9_]*_)\b)");
  const std::set<std::string> outputs = {"system_constants_", "clip_plane_constants_",
                                         "current_constant_buffers_up_to_date_", "register_file_",
                                         "zpd_counter_index_force_update_", "texture_cache_",
                                         "render_target_cache_"};
  for (std::sregex_iterator it(work.begin(), work.end(), member), end; it != end; ++it) {
    const std::string name = (*it)[1];
    if (outputs.count(name)) continue;
    Expect(Has(key, name), "the key holds the member " + name + ", which the update reads");
  }
  Expect(work.find("cvars::") == std::string::npos || work.find("fix 142f-diag") != std::string::npos,
         "the update reads no setting");
  Expect(work.find("return") == std::string::npos, "the update has no return after the memo");
  Expect(Has(key, "!zpd_counter_index_force_update_ && fix142g::system_slot.Predict()"),
         "a forced zpd counter update is never a repeat");
  Expect(Has(work, "if (fix142g_predicted && dirty) {") && Has(work, "fix142g::system_slot.Commit();"),
         "mode 1: a repeat that changed the constants is a mismatch; committed at the end");

  // The samplers.
  const std::string params = Body(tc, "VulkanTextureCache::SamplerParameters VulkanTextureCache::GetSamplerParameters(");
  Expect(!params.empty(), "GetSamplerParameters found");
  static const std::regex cvar(R"(cvars::([a-z0-9_]+))");
  for (std::sregex_iterator it(params.begin(), params.end(), cvar), end; it != end; ++it) {
    Expect(std::string((*it)[1]) == "anisotropic_override", "GetSamplerParameters reads " + std::string((*it)[1]));
  }
  Expect(RegistersIn(params).empty() && Has(params, "regs.GetTextureFetch(binding.fetch_constant);"),
         "GetSamplerParameters reads the binding's fetch constant only");
  const size_t samplers = cp.find("// Fix 142g: the samplers' repeat memo, per stage.");
  const std::string sampler_key = cp.substr(samplers, cp.find("fix142g::sampler_active[j] = true;", samplers) - samplers);
  for (const char* part : {"key.Add(memo_shader);", "key.Add(GetCurrentSubmission());",
                           "key.Add(texture_cache_->binding_epoch());", "key.Add(int32_t(cvars::anisotropic_override));",
                           "uint32_t(binding.mag_filter) << 8", "uint32_t(binding.min_filter) << 12",
                           "uint32_t(binding.mip_filter) << 16", "uint32_t(binding.aniso_filter) << 20",
                           "&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 6]"}) {
    Expect(Has(sampler_key, part), std::string("the samplers' key: ") + part);
  }
  Expect(Has(sampler_key, "if (i || !memo_shader) {"), "not on the retry after an overflow, nor without a shader");
  const size_t skip = cp.find("fix142g::sampler_active[j] = false;  // the stage keeps the last draw's samplers");
  Expect(skip != std::string::npos && cp.find("continue;", skip) < cp.find("shader_samplers.clear();", skip),
         "a repeat skips the stage before its samplers are cleared");
  Expect(cp.find("fix142g::sampler_slots[j].Invalidate();", skip) < cp.find("shader_samplers.clear();", skip),
         "a stage computed again is no memo until it completes");
  const size_t check = cp.find("// Fix 142g: check (mode 1) and keep each stage's samplers");
  Expect(check != std::string::npos && cp.find("if (samplers_overflowed_count) {", check) <
                                           cp.find("fix142g::sampler_slots[j].Commit();", check),
         "an overflow commits nothing");

  // The viewport.
  const std::string args = duh.substr(duh.find("struct GetViewportInfoArgs {"));
  Expect(Has(args, "union alignas(64) {") && Has(args, "__m128i first4;") && Has(args, "__m128i last4;") &&
             Has(args, "padding_set_to_0 = 0;  // important to zero this"),
         "GetViewportInfoArgs' compared part is its 64-byte union, the padding zeroed");
  const std::string viewport = Body(du, "void GetHostViewportInfo(GetViewportInfoArgs* XE_RESTRICT args,");
  for (std::sregex_iterator it(viewport.begin(), viewport.end(), cvar), end; it != end; ++it) {
    Expect(std::string((*it)[1]) == "half_pixel_offset", "GetHostViewportInfo reads " + std::string((*it)[1]));
  }
  Expect(RegistersIn(viewport).empty(), "GetHostViewportInfo reads no register");
  Expect(Has(cp, "key.AddBytes(&gviargs, 64);") && Has(cp, "key.Add(uint32_t(cvars::half_pixel_offset));"),
         "the viewport's key");
  Expect(Has(cp, "std::memcmp(&viewport_info, &fix142g::viewport_last, sizeof(viewport_info))"),
         "mode 1 compares the viewport");

  // The modes.
  for (const char* name : {"DEFINE_uint32(draw_memo_system, 0,", "DEFINE_uint32(draw_memo_samplers, 0,",
                           "DEFINE_uint32(draw_memo_viewport, 0,"}) {
    Expect(Has(cp, name), std::string(name) + " (off by default)");
  }
  const std::string end_submission = Body(cp, "bool VulkanCommandProcessor::EndSubmission(bool is_swap) {");
  Expect(Has(end_submission, "fix142g::Refresh();"), "the modes read at the end of a submission");
  Expect(Has(cp, ": draw_memo::kOff;") && Has(cp, "XBOX360PS5_DATA_ROOT \"/draw-memo.txt\""),
         "a setting that is not a mode is off; the file in the title folder");

  // Fix 142f-diag's bugs.
  const std::string fc = Read("include/xbox360ps5/frame_cost.hpp");
  Expect(Has(fc, "on for %.0f%% of this block"), "the switch's share of the block");
  const std::string device = Read(".deps/xenia-canary/src/xenia/ui/vulkan/vulkan_device.cc");
  Expect(Has(device, "XELOGW(  // fix 142g: the game log leaves out XELOGI\n        \"Vulkan descriptor indexing"),
         "the descriptor indexing line is a warning, which the game log keeps");
  const std::string mmio = Read(".deps/xenia-canary/src/xenia/cpu/mmio_handler.cc");
  const size_t linux_branch = mmio.find("#if XE_PLATFORM_LINUX");
  const size_t other_branch = mmio.find("#else", linux_branch);
  const size_t fault = mmio.find("xbox360ps5::frame_cost::NoteFault(is_write", linux_branch);
  Expect(linux_branch != std::string::npos && fault < other_branch,
         "the watch faults counted in the branch the PS5 build takes");
  const size_t history = cp.find("xbox360ps5::frame_cost::texture_set_history.Note(");
  const size_t reuse = cp.find("const fc142d::BindMark fix142d_reuse = fc142d::BindStart();");
  Expect(history != std::string::npos && history < reuse &&
             cp.find("texture_set_history.Note(", history + 60) == std::string::npos,
         "the texture set history before fix 142d's reuse check span, once");
  Expect(Has(cp, "the file \"\n             \"written at {} ({} s before)"), "the switch file's write time logged");
}

}  // namespace

int main(int argc, char** argv) {
  CheckModel();
  const bool model_only = argc > 1 && std::string(argv[1]) == "--model-only";
  if (!model_only) CheckTree();
  if (failures) {
    std::printf("check-draw-memo: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-draw-memo: ok\n");
  return 0;
}
