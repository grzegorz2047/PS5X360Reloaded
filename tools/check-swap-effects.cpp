// SPDX-License-Identifier: MIT
// Fix 141 (GFX-01A; xenia/gpu/vulkan/swap_effects.h, swap_post.glsli, vulkan_command_processor.cc,
// the launcher's color_preset and overscan_crop): a colour preset and an overscan crop in the
// Vulkan swap, on fix 137's effect path, both Off by default, live and per game.
//  (a) the settings: unknown values are the default, OpenGL is Off, one packed value;
//  (b) the colour math (the C++ mirror of swap_post.glsli): every preset keeps 0 and 1, never turns
//      a lighter shade darker than a darker one, keeps grey grey, and keeps the 8-bit fields 0, 8, 16
//      and 235, 247, 255 apart in order (the prototype's OLED preset clipped 8 to 0 and 251 to 255);
//  (c) the crop: 0-3% of each edge in whole pixels, symmetric, the proportions kept within one pixel
//      an edge, at 1x and 2x;
//  (d) when the effect path runs (Off + 0% without FXAA never), the push constants;
//  (e) the assembled tree: the shaders, the swap's wiring (the effects read before the guest output is
//      refreshed, the guest output the crop's size, the one-pass statements kept in their order when
//      nothing is asked for), the core's atomic setting, the launcher options (Video, live, per game,
//      after Anti-aliasing), Off on OpenGL, and the settings handed to the core next to every
//      ApplyPicture.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xenia/gpu/vulkan/swap_effects.h"

namespace {

namespace se = xe::gpu::swap_effects;
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
std::string Between(const std::string& text, const std::string& begin, const std::string& end) {
  const size_t from = text.find(begin);
  if (from == std::string::npos) return {};
  const size_t to = text.find(end, from + begin.size());
  return to == std::string::npos ? text.substr(from) : text.substr(from, to - from);
}
bool InOrder(const std::string& text, const std::vector<std::string>& statements) {
  const std::string squeezed = Squeeze(text);
  size_t at = 0;
  for (const std::string& statement : statements) {
    at = squeezed.find(Squeeze(statement), at);
    if (at == std::string::npos) {
      std::printf("  (not found in order: %s)\n", statement.c_str());
      return false;
    }
    at += Squeeze(statement).size();
  }
  return true;
}
int Level(float x) { return int(std::lround(x * 255.0f)); }
int Out(int level, const se::Grade& grade) { return Level(se::GradeChannel(level / 255.0f, grade)); }

void Settings() {
  Expect(se::ColorPresetFromSetting(0) == se::ColorPreset::kOff &&
             se::ColorPresetFromSetting(1) == se::ColorPreset::kOledDeepBlack &&
             se::ColorPresetFromSetting(2) == se::ColorPreset::kVibrant &&
             se::ColorPresetFromSetting(3) == se::ColorPreset::kCleanNeutral,
         "color_preset 0-3");
  for (int bad : {-1, 4, 5, 99})
    Expect(se::ColorPresetFromSetting(bad) == se::ColorPreset::kOff,
           "color_preset " + std::to_string(bad) + " is Off");
  for (int percent = 0; percent <= 3; ++percent)
    Expect(se::OverscanFromSetting(percent) == uint32_t(percent), "overscan_crop " + std::to_string(percent));
  for (int bad : {-1, 4, 10})
    Expect(se::OverscanFromSetting(bad) == 0, "overscan_crop " + std::to_string(bad) + " is 0%");
  for (int preset = 0; preset < 4; ++preset)
    for (int crop = 0; crop <= 3; ++crop) {
      const uint32_t packed = se::PackSettings(preset, crop, false);
      Expect(se::UnpackPreset(packed) == se::ColorPreset(uint32_t(preset)) &&
                 se::UnpackOverscan(packed) == uint32_t(crop),
             "pack " + std::to_string(preset) + "/" + std::to_string(crop));
      Expect(se::PackSettings(preset, crop, true) == 0, "OpenGL: Off and 0%");
    }
  Expect(se::PackSettings(0, 0, false) == 0, "the defaults pack to 0");
  Expect(se::UnpackPreset(0xFF) == se::ColorPreset::kOff && se::UnpackOverscan(0x7F00) == 0,
         "unknown packed values are the defaults");
}

void Colour() {
  Expect(se::IsIdentity(se::GradeFor(se::ColorPreset::kOff)), "Off is no grading");
  const se::ColorPreset presets[] = {se::ColorPreset::kOledDeepBlack, se::ColorPreset::kVibrant,
                                     se::ColorPreset::kCleanNeutral};
  for (se::ColorPreset preset : presets) {
    const std::string name = se::ColorPresetName(preset);
    const se::Grade grade = se::GradeFor(preset);
    Expect(!se::IsIdentity(grade), name + " grades");
    Expect(se::GradeChannel(0.0f, grade) == 0.0f && std::fabs(se::GradeChannel(1.0f, grade) - 1.0f) < 1e-6f,
           name + ": 0 and 1 stay");
    // Monotonic on a fine grid (no shade darker than the one below it).
    float previous = -1.0f;
    bool monotonic = true;
    for (int i = 0; i <= 4096; ++i) {
      const float y = se::GradeChannel(i / 4096.0f, grade);
      if (y < previous) monotonic = false;
      previous = y;
    }
    Expect(monotonic, name + ": monotonic");
    // The 8-bit fields of the card, in order and apart.
    Expect(Out(0, grade) < Out(8, grade) && Out(8, grade) < Out(16, grade),
           name + ": 0, 8, 16 apart (" + std::to_string(Out(0, grade)) + ", " +
               std::to_string(Out(8, grade)) + ", " + std::to_string(Out(16, grade)) + ")");
    Expect(Out(235, grade) < Out(247, grade) && Out(247, grade) < Out(255, grade),
           name + ": 235, 247, 255 apart (" + std::to_string(Out(235, grade)) + ", " +
               std::to_string(Out(247, grade)) + ", " + std::to_string(Out(255, grade)) + ")");
    // Grey stays grey (vibrance too).
    bool grey = true;
    for (int level = 0; level <= 255; level += 5) {
      float rgb[3] = {level / 255.0f, level / 255.0f, level / 255.0f};
      se::GradeColor(rgb, grade);
      if (std::fabs(rgb[0] - rgb[1]) > 1e-6f || std::fabs(rgb[1] - rgb[2]) > 1e-6f) grey = false;
    }
    Expect(grey, name + ": grey stays grey");
    // Colours stay in range and keep their hue order.
    float rgb[3] = {0.8f, 0.4f, 0.1f};
    se::GradeColor(rgb, grade);
    Expect(rgb[0] >= rgb[1] && rgb[1] >= rgb[2] && rgb[0] <= 1.0f && rgb[2] >= 0.0f,
           name + ": an orange stays orange");
  }
  // What the presets do: OLED deepens the shadows, Vibrant saturates, Clean Neutral lifts mid-tones.
  const se::Grade oled = se::GradeFor(se::ColorPreset::kOledDeepBlack);
  Expect(Out(16, oled) < 16 && Out(32, oled) < 32, "OLED Deep Black: darker shadows");
  float muted[3] = {0.5f, 0.45f, 0.4f};
  se::GradeColor(muted, se::GradeFor(se::ColorPreset::kVibrant));
  Expect(muted[0] - muted[2] > 0.1f, "Vibrant: a muted colour more saturated");
  Expect(Out(128, se::GradeFor(se::ColorPreset::kCleanNeutral)) > 128, "Clean Neutral: brighter mid-tones");
  // The prototype (black level 0.03, contrast 1.05, hard clamp) lost the shades the card names.
  auto prototype = [](int level) {
    float x = level / 255.0f;
    x = (x - 0.03f) / (1.0f - 0.03f);
    x = (x - 0.5f) * 1.05f + 0.5f;
    return Level(x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x));
  };
  Expect(prototype(0) == prototype(8) && prototype(251) == prototype(255),
         "the prototype's OLED curve clipped 8 to 0 and 251 to 255 (why the curve is smooth now)");
  Expect(Out(251, oled) < Out(255, oled) && Out(4, oled) < Out(8, oled),
         "OLED Deep Black keeps 4, 8 and 251, 255 apart");
}

void CropAndPath() {
  struct Size {
    uint32_t width, height;
  };
  for (Size size : {Size{1280, 720}, Size{2560, 1440}, Size{1280, 704}, Size{1024, 600}}) {
    for (uint32_t percent = 0; percent <= 3; ++percent) {
      const se::CropRect crop = se::Crop(size.width, size.height, percent);
      const std::string at = std::to_string(size.width) + "x" + std::to_string(size.height) + " " +
                             std::to_string(percent) + "%";
      Expect(crop.x * 2 + crop.width == size.width && crop.y * 2 + crop.height == size.height,
             at + ": symmetric");
      Expect(std::fabs(float(crop.x) - size.width * percent / 100.0f) <= 0.5f &&
                 std::fabs(float(crop.y) - size.height * percent / 100.0f) <= 0.5f,
             at + ": the percent of each edge, rounded");
      // Proportions: the cropped picture fills the same rectangle, so its aspect must be the
      // original's within one pixel an edge.
      const float ideal_height = float(crop.width) * size.height / size.width;
      Expect(std::fabs(ideal_height - float(crop.height)) <= 2.0f, at + ": proportions kept");
      if (!percent)
        Expect(crop.x == 0 && crop.y == 0 && crop.width == size.width && crop.height == size.height,
               at + ": 0% is the whole picture");
    }
  }
  const se::CropRect c720 = se::Crop(1280, 720, 2);
  Expect(c720.x == 26 && c720.y == 14 && c720.width == 1228 && c720.height == 692, "1280x720 at 2%");
  Expect(se::Crop(4, 4, 3).width == 4, "too small to crop: whole");
  Expect(se::Crop(1280, 720, 9).width == 1280, "an unknown percent: whole");

  // The effect path: never for Off + 0% without FXAA, whatever the resources.
  for (bool ready : {false, true})
    Expect(!se::UseEffectPath(false, se::ColorPreset::kOff, 0, 1280, 720, ready),
           "Off + 0% without FXAA: the one-pass swap");
  Expect(se::UseEffectPath(true, se::ColorPreset::kOff, 0, 1280, 720, true), "FXAA alone");
  Expect(se::UseEffectPath(false, se::ColorPreset::kVibrant, 0, 1280, 720, true), "a preset alone");
  Expect(se::UseEffectPath(false, se::ColorPreset::kOff, 2, 1280, 720, true), "a crop alone");
  Expect(!se::UseEffectPath(false, se::ColorPreset::kVibrant, 2, 1280, 720, false),
         "no resources: the one-pass swap");
  Expect(!se::UseEffectPath(true, se::ColorPreset::kVibrant, 2, 0, 720, true), "no size: one pass");

  static_assert(sizeof(se::PushConstants) == 48);
  const se::PushConstants pc =
      se::MakePushConstants(1280, 720, c720, se::ColorPreset::kOledDeepBlack);
  Expect(std::fabs(pc.size_inv_x - 1.0f / 1280.0f) < 1e-9f && std::fabs(pc.size_inv_y - 1.0f / 720.0f) < 1e-9f &&
             pc.offset_x == 26.0f && pc.offset_y == 14.0f && pc.grade_on == 1.0f && pc.toe > 0.0f,
         "push constants: size, the crop's corner, the preset");
  const se::PushConstants plain = se::MakePushConstants(1280, 720, se::Crop(1280, 720, 0),
                                                        se::ColorPreset::kOff);
  Expect(plain.offset_x == 0.0f && plain.offset_y == 0.0f && plain.grade_on == 0.0f,
         "FXAA alone: no offset, no grading");

  se::LineState a, b;
  a.preset = se::ColorPreset::kVibrant;
  b = a;
  Expect(a == b, "same state: no new line");
  b.overscan = 2;
  Expect(!(a == b), "a new crop: a new line");
}

void Tree() {
  const std::string core = ".deps/xenia-canary/";
  const std::string shaders = core + "src/xenia/gpu/shaders/";
  const std::string glsli = Read(shaders + "swap_post.glsli");
  const std::string copy = Read(shaders + "swap_post.ps.glsl");
  const std::string fxaa = Read(shaders + "fxaa.ps.glsl");
  const std::string extreme = Read(shaders + "fxaa_extreme.ps.glsl");
  Expect(!glsli.empty() && copy.find("#define XE_SWAP_POST_FXAA 0") != std::string::npos &&
             fxaa.find("#define XE_SWAP_POST_FXAA 1") != std::string::npos &&
             extreme.find("#define XE_SWAP_POST_FXAA 1") != std::string::npos &&
             Read(shaders + "fxaa.glsli").empty(),
         "the shaders: swap_post.glsli for FXAA, FXAA Extreme and no AA (fxaa.glsli folded in)");
  // The GLSL mirrors GradeChannel and GradeColor.
  Expect(InOrder(glsli, {"x = clamp(x, 0.0, 1.0);", "x = x * x * (1.0 + toe) / (x + toe);",
                         "x = x + curve * (x * x * (3.0 - 2.0 * x) - x);", "x = pow(x, inv_gamma);",
                         "return clamp(x, 0.0, 1.0);",
                         "float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));",
                         "float amount = 1.0 + vibrance * (1.0 - (max_c - min_c));",
                         "vec2 pixel = gl_FragCoord.xy + xe_swap_post.offset;",
                         "vec3 color = texelFetch(xe_swap_post_source, ivec2(pixel), 0).rgb;",
                         "if (xe_swap_post.grade_on.x != 0.0) {", "color = XeGrade(color);"}),
         "swap_post.glsli: the grading of swap_effects.h, the crop's offset, no filtering without AA");

  const std::string cc = Read(core + "src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string h = Read(core + "src/xenia/gpu/vulkan/vulkan_command_processor.h");
  const std::string cp_h = Read(core + "src/xenia/gpu/command_processor.h");
  Expect(cp_h.find("std::atomic<uint32_t> swap_effects_{0};") != std::string::npos &&
             cp_h.find("swap_effects_.store(packed, std::memory_order_release);") != std::string::npos &&
             cp_h.find("return swap_effects_.load(std::memory_order_acquire);") != std::string::npos,
         "CommandProcessor: the packed effects as an atomic, set from the launcher's thread");
  Expect(cc.find("#include \"xenia/gpu/shaders/bytecode/vulkan_spirv/swap_post_ps.h\"") !=
                 std::string::npos &&
             cc.find("#include \"xenia/gpu/vulkan/swap_effects.h\"") != std::string::npos,
         "the no-AA shader and swap_effects.h are included");
  const std::string swap =
      Between(cc, "void VulkanCommandProcessor::IssueSwap(", "\nbool VulkanCommandProcessor::");
  Expect(InOrder(swap, {"const SwapPostEffect swap_post_effect = GetActualSwapPostEffect();",
                        "const uint32_t swap_effects = GetSwapEffects();",
                        "const SwapEffectsPlan swap_plan = PrepareSwapEffects(",
                        "presenter->RefreshGuestOutput(swap_plan.output_width, swap_plan.output_height,",
                        "context.SetIs8bpc(swap_fxaa::Is8bpc(use_pwl_gamma_ramp, swap_plan.use));"}),
         "IssueSwap: the effects read once, the plan made before the guest output is refreshed at "
         "its size, then the 8 bpc hint");
  Expect(InOrder(swap, {"swap_framebuffer_create_info.width = swap_plan.output_width;",
                        "swap_framebuffer_create_info.height = swap_plan.output_height;",
                        "if (swap_plan.use) {", "RecordSwapEffectsSourcePass(",
                        "VkRenderPassBeginInfo render_pass_begin_info;",
                        "render_pass_begin_info.renderArea.extent.width = swap_plan.output_width;",
                        "viewport.width = float(swap_plan.output_width);",
                        "scissor.extent.width = swap_plan.output_width;", "if (!swap_plan.use) {",
                        "BindExternalGraphicsPipeline(use_pwl_gamma_ramp ? "
                        "swap_apply_gamma_pwl_pipeline_ : swap_apply_gamma_256_entry_table_pipeline_);",
                        "swap_descriptor_source_image_info.imageView = swap_texture_view;",
                        "deferred_command_buffer_.CmdVkDraw(3, 1, 0, 0);", "} else {",
                        "RecordSwapEffectsPass(swap_post_effect, swap_plan, swap_frame_index);", "}",
                        "deferred_command_buffer_.CmdVkEndRenderPass();", "EndSubmission(true);"}),
         "IssueSwap: the guest output at the plan's size; with nothing asked for the one-pass "
         "statements in their order");
  const std::string prepare =
      Between(cc, "VulkanCommandProcessor::SwapEffectsPlan VulkanCommandProcessor::PrepareSwapEffects(", "\n}\n");
  Expect(InOrder(prepare, {"DestroyRetiredSwapFxaaSources(false);", "swap_effects::UseEffectPath(",
                           "swap_effects::Crop(", "plan.output_width = crop.width;"}) &&
             prepare.find("plan.output_width = width;") != std::string::npos,
         "PrepareSwapEffects: the crop only on the effect path, else the front buffer's size");
  const std::string pass = Between(cc, "void VulkanCommandProcessor::RecordSwapEffectsPass(", "\n}\n");
  Expect(InOrder(pass, {"swap_fxaa_descriptor_sets_[swap_frame_index]", "vkUpdateDescriptorSets(",
                        "BindExternalGraphicsPipeline(", "CmdVkBindDescriptorSets(",
                        "swap_effects::MakePushConstants(", "CmdVkPushConstants(",
                        "CmdVkDraw(3, 1, 0, 0);"}),
         "the effect pass: this frame's set, the pipeline, the push constants with the crop and the "
         "preset, the draw");
  Expect(h.find("std::array<VkPipeline, 3> swap_fxaa_pipelines_{};") != std::string::npos,
         "three effect pipelines: FXAA, FXAA Extreme, no AA");

  // The launcher.
  const std::string launcher = Read("src/launcher.cpp");
  const std::string settings_h = Read("include/xbox360ps5/launcher.hpp");
  Expect(settings_h.find("int color_preset = 0;") != std::string::npos &&
             settings_h.find("int overscan_crop = 0;") != std::string::npos,
         "Settings: color_preset and overscan_crop, Off and 0%");
  Expect(InOrder(launcher,
                 {"{\"antialiasing\", &S::antialiasing, 0, W::live,",
                  "{\"color_preset\", &S::color_preset, 0, W::live, 0, true, \"Color preset\",",
                  "{\"Off\", \"OLED Deep Black\", \"Vibrant\", \"Clean Neutral\"}},",
                  "{\"overscan_crop\", &S::overscan_crop, 0, W::live, 0, true, \"Overscan crop\",",
                  "{\"0%\", \"1%\", \"2%\", \"3%\"}},", "{\"sharpness\", &S::sharpness,"}),
         "the options: Video, live, per game, Off recommended, after Anti-aliasing");
  const std::string for_game = Between(launcher, "Settings ForGame(", "\n}\n");
  Expect(InOrder(for_game, {"if (effective.renderer == int(Renderer::kOpenGL)) {",
                            "effective.color_preset = 0;", "effective.overscan_crop = 0;"}),
         "ForGame: Off and 0% on OpenGL");
  Expect(Count(launcher, "option.field == &Settings::color_preset || option.field == &Settings::overscan_crop") >= 2,
         "the launcher refuses both on OpenGL (general and per game)");
  const std::string game_main = Read("src/canary_game_main.cpp");
  const std::string apply = Between(game_main, "void ApplySwapPostEffect(", "\n}\n");
  Expect(apply.find("xe::gpu::swap_effects::PackSettings(") != std::string::npos &&
             apply.find("SetSwapEffects(") != std::string::npos,
         "ApplySwapPostEffect also hands the preset and the crop to the core (start, guide, web)");
}

}  // namespace

int main() {
  Settings();
  Colour();
  CropAndPath();
  Tree();
  if (failures) {
    std::printf("check-swap-effects: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-swap-effects: ok\n");
  return 0;
}
