// SPDX-License-Identifier: MIT
// Fix 137 (xenia/gpu/vulkan/swap_fxaa.h, vulkan_command_processor.cc, the FXAA shaders, the
// launcher's "antialiasing" option): FXAA and FXAA Extreme for the Vulkan swap, Off by default.
// 1x games have no anti-aliasing, and FSR/CAS sharpen their jagged edges; the core had the
// SwapPostEffect setting, the luma shaders and the FXAA library, but the Vulkan swap never used
// them (Triang3l's TODOs).
//  (a) the decisions: the setting's values (unknown is Off), Off on OpenGL, when the two passes run,
//      when the intermediate image is made again (1x <-> 2x), the push constants, the deferred
//      destruction, the 8 bpc hint, the format and its fallback;
//  (b) the model of IssueSwap's passes: Off (and FXAA without resources) is the one pass it always
//      was; FXAA is luma pass, barrier, FXAA pass;
//  (c) the assembled tree: the shaders and their presets (fxaa.cs.hlsl's and
//      fxaa_extreme.cs.hlsl's), the swap's wiring with the one-pass path's statements kept in their
//      order for Off, the 8 bpc TODO answered, the lazy resources with per-frame descriptor sets and
//      the deferred destruction, the shutdown; the launcher option (Video, live, per game, after
//      Image filter), Off on OpenGL in ForGame and refused there by the launcher, and
//      ApplySwapPostEffect next to every ApplyPicture.
// Fix 137b: the effect no longer reaches the command processor thread through CallInThread, whose
// pending_fns_ is a plain std::queue the GPU thread pops without a lock (fix 137 changes the effect
// live from the launcher's thread): SetDesiredSwapPostEffect stores an atomic, and
// GetActualSwapPostEffect, read once per swap, loads it.
//  (d) the tree: the atomic, the store without CallInThread, the load in GetActualSwapPostEffect, no
//      copy left; a model of a launcher thread switching while a GPU thread swaps (each swap sees one
//      whole value, the last switch wins by the next swap).
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xenia/gpu/vulkan/swap_fxaa.h"

namespace {

namespace sf = xe::gpu::swap_fxaa;
int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-9f; }
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
// Whitespace removed, so re-indented statements compare equal.
std::string Squeeze(const std::string& text) {
  std::string out;
  for (char c : text)
    if (c != ' ' && c != '\n' && c != '\t') out += c;
  return out;
}
size_t Count(const std::string& text, const std::string& what) {
  size_t count = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++count;
  return count;
}
// The text between `begin` and the next `end` after it.
std::string Between(const std::string& text, const std::string& begin, const std::string& end) {
  const size_t from = text.find(begin);
  if (from == std::string::npos) return {};
  const size_t to = text.find(end, from + begin.size());
  return to == std::string::npos ? text.substr(from) : text.substr(from, to - from);
}
// Each statement in order (whitespace ignored).
bool InOrder(const std::string& text, const std::vector<std::string>& statements) {
  const std::string squeezed = Squeeze(text);
  size_t at = 0;
  for (const std::string& statement : statements) {
    at = squeezed.find(Squeeze(statement), at);
    if (at == std::string::npos) return false;
    at += Squeeze(statement).size();
  }
  return true;
}

void Decisions() {
  // The setting (0 Off, 1 FXAA, 2 FXAA Extreme; anything else Off).
  Expect(sf::EffectFromSetting(0) == sf::Effect::kNone, "0 is Off");
  Expect(sf::EffectFromSetting(1) == sf::Effect::kFxaa, "1 is FXAA");
  Expect(sf::EffectFromSetting(2) == sf::Effect::kFxaaExtreme, "2 is FXAA Extreme");
  Expect(sf::EffectFromSetting(-1) == sf::Effect::kNone, "-1 is Off");
  Expect(sf::EffectFromSetting(3) == sf::Effect::kNone, "3 is Off");
  Expect(sf::EffectFromSetting(99) == sf::Effect::kNone, "99 is Off");
  // CommandProcessor::SwapPostEffect's numbers.
  Expect(uint32_t(sf::Effect::kNone) == 0 && uint32_t(sf::Effect::kFxaa) == 1 &&
             uint32_t(sf::Effect::kFxaaExtreme) == 2,
         "the effect's numbers are SwapPostEffect's");
  // OpenGL: always Off.
  for (int value = -1; value <= 3; ++value)
    Expect(sf::EffectForRenderer(value, true) == sf::Effect::kNone,
           "OpenGL gets Off for " + std::to_string(value));
  Expect(sf::EffectForRenderer(1, false) == sf::Effect::kFxaa, "Vulkan gets FXAA");
  Expect(sf::EffectForRenderer(2, false) == sf::Effect::kFxaaExtreme, "Vulkan gets FXAA Extreme");
  Expect(std::string(sf::EffectName(sf::Effect::kFxaa)) == "FXAA" &&
             std::string(sf::EffectName(sf::Effect::kFxaaExtreme)) == "FXAA Extreme" &&
             std::string(sf::EffectName(sf::Effect::kNone)) == "off",
         "effect names");

  // When the two passes run.
  Expect(!sf::UseFxaa(sf::Effect::kNone, 1280, 720, true), "Off never runs FXAA");
  Expect(!sf::UseFxaa(sf::Effect::kFxaa, 1280, 720, false), "no resources: the one pass");
  Expect(!sf::UseFxaa(sf::Effect::kFxaa, 0, 720, true), "zero width: the one pass");
  Expect(!sf::UseFxaa(sf::Effect::kFxaaExtreme, 1280, 0, true), "zero height: the one pass");
  Expect(sf::UseFxaa(sf::Effect::kFxaa, 1280, 720, true), "FXAA at 1x");
  Expect(sf::UseFxaa(sf::Effect::kFxaaExtreme, 2560, 1440, true), "FXAA Extreme at 2x");

  // The intermediate image's size follows the front buffer.
  Expect(sf::NeedsNewSource(0, 0, 1280, 720), "none yet: made");
  Expect(!sf::NeedsNewSource(1280, 720, 1280, 720), "same size: kept");
  Expect(sf::NeedsNewSource(1280, 720, 2560, 1440), "1x -> 2x: made again");
  Expect(sf::NeedsNewSource(2560, 1440, 1280, 720), "2x -> 1x: made again");
  Expect(sf::NeedsNewSource(1280, 720, 1280, 704), "a game's own resolution change: made again");
  Expect(!sf::NeedsNewSource(1280, 720, 0, 0), "zero size: nothing made");
  Expect(!sf::NeedsNewSource(0, 0, 0, 720), "zero width: nothing made");

  // Push constants: 1 / size, 8 bytes (fxaa.glsli's vec2).
  static_assert(sizeof(sf::PushConstants) == 8);
  const sf::PushConstants at_720p = sf::MakePushConstants(1280, 720);
  Expect(Near(at_720p.size_inv_x, 1.0f / 1280.0f) && Near(at_720p.size_inv_y, 1.0f / 720.0f),
         "push constants at 1280x720");
  const sf::PushConstants at_1440p = sf::MakePushConstants(2560, 1440);
  Expect(Near(at_1440p.size_inv_x, 1.0f / 2560.0f) && Near(at_1440p.size_inv_y, 1.0f / 1440.0f),
         "push constants at 2560x1440");
  const sf::PushConstants zero = sf::MakePushConstants(0, 720);
  Expect(zero.size_inv_x == 0.0f && zero.size_inv_y == 0.0f, "zero size: zero push constants");

  // Deferred destruction by submission.
  Expect(!sf::CanDestroy(10, 9), "still in flight: kept");
  Expect(sf::CanDestroy(10, 10), "its submission completed: destroyed");
  Expect(sf::CanDestroy(10, 15), "later submissions completed: destroyed");
  Expect(sf::CanDestroy(0, 0), "never used: destroyed");

  // The 8 bpc hint (Triang3l's TODO): only the table ramp without FXAA is 8 bits.
  Expect(sf::Is8bpc(false, false), "table ramp, Off: 8 bpc (as before)");
  Expect(!sf::Is8bpc(true, false), "PWL ramp, Off: more (as before)");
  Expect(!sf::Is8bpc(false, true), "table ramp with FXAA: more than 8 bits");
  Expect(!sf::Is8bpc(true, true), "PWL ramp with FXAA: more");

  // The format: RGBA16F, else RGBA8, else none.
  Expect(sf::ChooseSourceFormat(true, true) == sf::SourceFormat::kRgba16Float, "RGBA16F first");
  Expect(sf::ChooseSourceFormat(true, false) == sf::SourceFormat::kRgba16Float, "RGBA16F alone");
  Expect(sf::ChooseSourceFormat(false, true) == sf::SourceFormat::kRgba8Unorm, "RGBA8 fallback");
  Expect(sf::ChooseSourceFormat(false, false) == sf::SourceFormat::kNone, "neither: no FXAA");
  Expect(std::string(sf::SourceFormatName(sf::SourceFormat::kRgba16Float)) ==
                 "R16G16B16A16_SFLOAT" &&
             std::string(sf::SourceFormatName(sf::SourceFormat::kRgba8Unorm)) == "R8G8B8A8_UNORM",
         "format names");

  // The log line changes with the state only.
  sf::LineState a{sf::Effect::kFxaa, 1280, 720, sf::SourceFormat::kRgba16Float};
  sf::LineState b = a;
  Expect(a == b, "same state: no new line");
  b.width = 2560;
  Expect(!(a == b), "a new size: a new line");
  b = a;
  b.effect = sf::Effect::kFxaaExtreme;
  Expect(!(a == b), "a new effect: a new line");
}

void Model() {
  // Off, and FXAA that could not be made (UseFxaa false), record exactly today's one pass.
  for (bool ready : {false, true})
    for (int setting : {0, 1, 2, 7}) {
      const bool use = sf::UseFxaa(sf::EffectFromSetting(setting), 1280, 720, ready);
      const sf::Passes passes = sf::SwapPasses(use);
      if (setting == 0 || setting == 7 || !ready) {
        Expect(!use && passes.count == 1 && passes.pass[0] == sf::Pass::kApplyGammaToOutput,
               "the one-pass swap for setting " + std::to_string(setting) +
                   (ready ? " with resources" : " without resources"));
      } else {
        Expect(use && passes.count == 3 && passes.pass[0] == sf::Pass::kApplyGammaLumaToSource &&
                   passes.pass[1] == sf::Pass::kSourceBarrier &&
                   passes.pass[2] == sf::Pass::kFxaaToOutput,
               "the FXAA passes for setting " + std::to_string(setting));
      }
    }
  // A live switch: each swap decides on its own (Off after FXAA is the one pass again).
  const int sequence[] = {0, 1, 2, 0, 2, 1, 0};
  for (int setting : sequence) {
    const sf::Passes passes =
        sf::SwapPasses(sf::UseFxaa(sf::EffectFromSetting(setting), 1280, 720, true));
    Expect((setting == 0) == (passes.count == 1), "live switch to " + std::to_string(setting));
  }
}

void Tree() {
  const std::string core = ".deps/xenia-canary/";
  const std::string shaders = core + "src/xenia/gpu/shaders/";
  const std::string glsli = Read(shaders + "fxaa.glsli");
  const std::string fxaa = Read(shaders + "fxaa.ps.glsl");
  const std::string extreme = Read(shaders + "fxaa_extreme.ps.glsl");
  Expect(!glsli.empty() && !fxaa.empty() && !extreme.empty(), "the FXAA shaders are in the tree");
  Expect(!Read(core + "third_party/fxaa/FXAA3_11.h").empty() &&
             glsli.find("#include \"../../../../third_party/fxaa/FXAA3_11.h\"") != std::string::npos,
         "fxaa.glsli includes the tree's FXAA3_11.h");
  Expect(glsli.find("#define FXAA_GLSL_130 1") != std::string::npos &&
             glsli.find("#define FXAA_GREEN_AS_LUMA 0") != std::string::npos &&
             glsli.find("layout(push_constant)") != std::string::npos &&
             glsli.find("layout(set = 0, binding = 0) uniform sampler2D") != std::string::npos,
         "fxaa.glsli: GLSL, luma in alpha, push constants, one combined image sampler");
  // fxaa.cs.hlsl's and fxaa_extreme.cs.hlsl's settings.
  const std::string hlsl = Read(shaders + "fxaa.cs.hlsl");
  const std::string hlsl_extreme = Read(shaders + "fxaa_extreme.cs.hlsl");
  for (const char* line : {"FXAA_QUALITY__PRESET 12", "FXAA_QUALITY__SUBPIX 0.75",
                           "FXAA_QUALITY__EDGE_THRESHOLD 0.166",
                           "FXAA_QUALITY__EDGE_THRESHOLD_MIN 0.0833"}) {
    Expect(fxaa.find(line) != std::string::npos, std::string("fxaa.ps.glsl: ") + line);
    Expect(hlsl.find(line) != std::string::npos, std::string("fxaa.cs.hlsl has ") + line);
  }
  for (const char* line : {"FXAA_QUALITY__PRESET 39", "FXAA_QUALITY__SUBPIX 1.0",
                           "FXAA_QUALITY__EDGE_THRESHOLD 0.063",
                           "FXAA_QUALITY__EDGE_THRESHOLD_MIN 0.0312"}) {
    Expect(extreme.find(line) != std::string::npos, std::string("fxaa_extreme.ps.glsl: ") + line);
    Expect(hlsl_extreme.find(line) != std::string::npos,
           std::string("fxaa_extreme.cs.hlsl has ") + line);
  }
  // The core builds every shaders/*.glsl into bytecode/vulkan_spirv/<name>_<stage>.h.
  const std::string helpers = Read(core + "cmake/XeniaHelpers.cmake");
  Expect(helpers.find("\"${shader_dir}/*.glsl\"") != std::string::npos,
         "XeniaHelpers.cmake compiles shaders/*.glsl to SPIR-V");

  const std::string cc = Read(core + "src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string h = Read(core + "src/xenia/gpu/vulkan/vulkan_command_processor.h");
  Expect(cc.find("#include \"xenia/gpu/shaders/bytecode/vulkan_spirv/fxaa_ps.h\"") !=
                 std::string::npos &&
             cc.find("#include \"xenia/gpu/shaders/bytecode/vulkan_spirv/fxaa_extreme_ps.h\"") !=
                 std::string::npos &&
             cc.find("#include \"xenia/gpu/vulkan/swap_fxaa.h\"") != std::string::npos,
         "the FXAA shaders and swap_fxaa.h are included");
  Expect(cc.find("static_assert(uint32_t(CommandProcessor::SwapPostEffect::kFxaaExtreme) ==") !=
             std::string::npos,
         "the effect numbers are asserted against SwapPostEffect");
  Expect(cc.find("// TODO(Triang3l): FXAA can result in more than 8 bits of precision.") ==
                 std::string::npos &&
             cc.find("context.SetIs8bpc(swap_fxaa::Is8bpc(use_pwl_gamma_ramp, use_fxaa));") !=
                 std::string::npos &&
             cc.find("context.SetIs8bpc(!use_pwl_gamma_ramp);") == std::string::npos,
         "the 8 bpc TODO: SetIs8bpc counts FXAA");
  Expect(cc.find("// TODO(Triang3l): FXAA combined image and sampler sources.") ==
             std::string::npos,
         "the descriptor pool's FXAA TODO is answered");

  const std::string swap =
      Between(cc, "void VulkanCommandProcessor::IssueSwap(", "\nbool VulkanCommandProcessor::");
  Expect(!swap.empty(), "IssueSwap found");
  // The effect is the GPU thread's (SetDesiredSwapPostEffect sets it with CallInThread), and the
  // resources are made before the 8 bpc hint, which needs to know whether FXAA runs.
  Expect(InOrder(swap,
                 {"GetActualSwapPostEffect()",
                  "const bool use_fxaa = PrepareSwapFxaa(",
                  "context.SetIs8bpc(swap_fxaa::Is8bpc(use_pwl_gamma_ramp, use_fxaa));"}),
         "IssueSwap: the effect, the resources, then the 8 bpc hint");
  // Off: the one-pass path's statements, in their order, unchanged (whitespace aside).
  Expect(InOrder(swap,
                 {"render_pass_begin_info.renderPass = swap_apply_gamma_render_pass_;",
                  "render_pass_begin_info.framebuffer = swap_framebuffer.framebuffer;",
                  "deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info, "
                  "VK_SUBPASS_CONTENTS_INLINE);",
                  "current_render_pass_ = swap_apply_gamma_render_pass_;",
                  "SetViewport(viewport);", "SetScissor(scissor);",
                  "if (!use_fxaa) {",
                  "BindExternalGraphicsPipeline(use_pwl_gamma_ramp ? "
                  "swap_apply_gamma_pwl_pipeline_ : swap_apply_gamma_256_entry_table_pipeline_);",
                  "VkDescriptorSet swap_descriptor_source = "
                  "swap_descriptors_source_[swap_frame_index];",
                  "swap_descriptor_source_image_info.imageView = swap_texture_view;",
                  "dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_source_write, 0, "
                  "nullptr);",
                  "deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,"
                  " swap_apply_gamma_pipeline_layout_, 0, uint32_t(swap_descriptor_sets.size()), "
                  "swap_descriptor_sets.data(), 0, nullptr);",
                  "deferred_command_buffer_.CmdVkDraw(3, 1, 0, 0);",
                  "} else {", "RecordSwapFxaaPass(", "}",
                  "deferred_command_buffer_.CmdVkEndRenderPass();",
                  "current_render_pass_ = VK_NULL_HANDLE;",
                  "PushImageMemoryBarrier(vulkan_context.image(),",
                  "EndSubmission(true);"}),
         "IssueSwap: Off keeps the one-pass statements in order");
  // FXAA's luma pass comes only with use_fxaa, after the guest output's barrier and before the
  // swap render pass.
  Expect(InOrder(swap, {"if (vulkan_context.image_ever_written_previously()) {",
                        "SubmitBarriers(true);",
                        "if (use_fxaa) {", "RecordSwapFxaaLumaPass(",
                        "VkRenderPassBeginInfo render_pass_begin_info;"}),
         "IssueSwap: the luma pass, only with use_fxaa, before the swap render pass");
  Expect(Count(swap, "RecordSwapFxaaLumaPass(") == 1 && Count(swap, "RecordSwapFxaaPass(") == 1,
         "IssueSwap records each FXAA pass once");

  const std::string luma =
      Between(cc, "void VulkanCommandProcessor::RecordSwapFxaaLumaPass(", "\n}\n");
  Expect(InOrder(luma, {"CmdVkBeginRenderPass(", "swap_fxaa_luma_pipelines_[",
                        "vkUpdateDescriptorSets(", "CmdVkBindDescriptorSets(",
                        "CmdVkDraw(3, 1, 0, 0);", "CmdVkEndRenderPass();",
                        "VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,",
                        "VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,",
                        "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL", "SubmitBarriers(true);"}),
         "the luma pass: draw into the source, then the barrier to shader read");
  const std::string pass = Between(cc, "void VulkanCommandProcessor::RecordSwapFxaaPass(", "\n}\n");
  Expect(InOrder(pass, {"swap_fxaa_descriptor_sets_[swap_frame_index]", "vkUpdateDescriptorSets(",
                        "BindExternalGraphicsPipeline(", "CmdVkBindDescriptorSets(",
                        "CmdVkPushConstants(", "CmdVkDraw(3, 1, 0, 0);"}),
         "the FXAA pass: this frame's descriptor set, pipeline, push constants, draw");
  const std::string prepare = Between(cc, "bool VulkanCommandProcessor::PrepareSwapFxaa(", "\n}\n");
  Expect(InOrder(prepare, {"DestroyRetiredSwapFxaaSources(", "swap_fxaa::UseFxaa("}) &&
             prepare.find("swap_fxaa::NeedsNewSource(") != std::string::npos &&
             prepare.find("RetireSwapFxaaSource(") != std::string::npos,
         "PrepareSwapFxaa: retired sources destroyed when completed, a new one on a size change");
  Expect(cc.find("swap_fxaa::CanDestroy(") != std::string::npos &&
             cc.find("last_submission = GetCurrentSubmission();") != std::string::npos,
         "the source's last submission is recorded and checked before destruction");
  const std::string shutdown =
      Between(cc, "void VulkanCommandProcessor::ShutdownContext()", "\n}\n");
  Expect(InOrder(shutdown, {"AwaitAllQueueOperationsCompletion();", "ShutdownSwapFxaa();"}),
         "ShutdownContext destroys the FXAA resources after the GPU is idle");
  Expect(h.find("std::array<VkDescriptorSet, kMaxFramesInFlight> swap_fxaa_descriptor_sets_") !=
             std::string::npos,
         "a descriptor set per frame in flight");
  Expect(h.find("std::vector<SwapFxaaSource> swap_fxaa_retired_sources_;") != std::string::npos,
         "retired sources kept until completed");

  // The launcher.
  const std::string launcher = Read("src/launcher.cpp");
  const std::string settings_h = Read("include/xbox360ps5/launcher.hpp");
  Expect(settings_h.find("int antialiasing = 0;") != std::string::npos, "Settings::antialiasing, Off");
  Expect(InOrder(launcher,
                 {"{\"image_filter\", &S::image_filter, 0, W::live,",
                  "{\"antialiasing\", &S::antialiasing, 0, W::live, 0, true, \"Anti-aliasing\",",
                  "{\"Off\", \"FXAA\", \"FXAA Extreme\"}},",
                  "{\"sharpness\", &S::sharpness,"}),
         "the option: Video, live, per game, Off recommended, after Image filter");
  const std::string for_game = Between(launcher, "Settings ForGame(", "\n}\n");
  Expect(InOrder(for_game, {"if (effective.renderer == int(Renderer::kOpenGL)) {",
                            "effective.antialiasing = 0;"}),
         "ForGame: Off on OpenGL");
  Expect(Count(launcher, "option.field == &Settings::antialiasing") >= 2,
         "the launcher refuses the option on OpenGL (general and per game)");

  const std::string game_main = Read("src/canary_game_main.cpp");
  const size_t pictures = Count(game_main, "ApplyPicture(settings, emulator.graphics_system()->presenter());");
  Expect(pictures >= 3 &&
             Count(game_main, "ApplySwapPostEffect(settings, emulator.graphics_system());") ==
                 pictures,
         "ApplySwapPostEffect next to every ApplyPicture (start, guide, web settings)");
  Expect(game_main.find("xe::gpu::swap_fxaa::EffectForRenderer(") != std::string::npos &&
             game_main.find("SetDesiredSwapPostEffect(") != std::string::npos,
         "ApplySwapPostEffect: Off on OpenGL, through SetDesiredSwapPostEffect");
}

void AtomicEffect() {
  const std::string core = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string h = Read(core + "command_processor.h");
  const std::string cc = Read(core + "command_processor.cc");
  Expect(h.find("std::atomic<SwapPostEffect> swap_post_effect_desired_{SwapPostEffect::kNone};") !=
                 std::string::npos &&
             h.find("swap_post_effect_actual_") == std::string::npos &&
             cc.find("swap_post_effect_actual_") == std::string::npos,
         "137b: one atomic effect, no copy made on the command processor thread");
  const std::string set =
      Between(cc, "void CommandProcessor::SetDesiredSwapPostEffect(", "\n}\n");
  Expect(!set.empty() && set.find("CallInThread(") == std::string::npos &&
             set.find("swap_post_effect_desired_.store(swap_post_effect, "
                      "std::memory_order_release);") != std::string::npos,
         "137b: SetDesiredSwapPostEffect stores the atomic without CallInThread");
  const std::string actual = Between(h, "SwapPostEffect GetActualSwapPostEffect() const {", "}");
  const std::string desired = Between(h, "SwapPostEffect GetDesiredSwapPostEffect() const {", "}");
  Expect(actual.find("swap_post_effect_desired_.load(std::memory_order_acquire);") !=
                 std::string::npos &&
             desired.find("swap_post_effect_desired_.load(std::memory_order_acquire);") !=
                 std::string::npos,
         "137b: both getters load the atomic");
  const std::string vk = Read(core + "vulkan/vulkan_command_processor.cc");
  const std::string swap =
      Between(vk, "void VulkanCommandProcessor::IssueSwap(", "\nbool VulkanCommandProcessor::");
  Expect(Count(swap, "GetActualSwapPostEffect()") == 1,
         "137b: IssueSwap reads the effect once per swap");

  // The model: a launcher thread switches Off/FXAA/Extreme while the GPU thread swaps.
  std::atomic<sf::Effect> effect_atomic{sf::Effect::kNone};
  std::atomic<bool> done{false};
  std::atomic<int> bad{0};
  std::thread gpu([&] {
    while (!done.load()) {
      const sf::Effect effect = effect_atomic.load(std::memory_order_acquire);  // once per swap
      const uint32_t value = uint32_t(effect);
      if (value > 2) bad.fetch_add(1);
      const sf::Passes passes = sf::SwapPasses(sf::UseFxaa(effect, 1280, 720, true));
      if ((effect == sf::Effect::kNone) != (passes.count == 1)) bad.fetch_add(1);
    }
  });
  for (int i = 0; i < 200000; ++i)
    effect_atomic.store(sf::EffectFromSetting(i % 3), std::memory_order_release);
  effect_atomic.store(sf::Effect::kFxaaExtreme, std::memory_order_release);
  done.store(true);
  gpu.join();
  Expect(bad.load() == 0, "137b model: every swap sees one whole effect");
  Expect(effect_atomic.load(std::memory_order_acquire) == sf::Effect::kFxaaExtreme,
         "137b model: the last switch is what the next swap draws");
}

}  // namespace

int main() {
  Decisions();
  Model();
  Tree();
  AtomicEffect();
  if (failures) {
    std::printf("check-vulkan-fxaa: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-vulkan-fxaa: ok\n");
  return 0;
}
