// SPDX-License-Identifier: MIT
// Fix 145 (GFX-03; xenia/gpu/vulkan/swap_smaa.h, the smaa_* shaders, vulkan_command_processor.cc,
// third_party/smaa from iryoku/smaa 71c806a): SMAA 1x as the fourth anti-aliasing choice in the
// Vulkan swap, on the effect path of fixes 137 and 141.
//  (a) the setting: 3 is SMAA 1x, 0-2 keep their meaning, unknown values are Off, OpenGL is Off;
//  (b) the pinned upstream files: SMAA.hlsl, AreaTex.h, SearchTex.h and LICENSE.txt byte for byte
//      (SHA-256), their sizes as swap_smaa.h has them;
//  (c) the LUT orientation: AreaTex as AreaTex.h stores it, row 0 at v = 0 (Vulkan's origin is
//      Direct3D's): its first subtexture (offset 0, near v = 0 in SMAAArea) has the diagonal areas
//      in its right half and its last has none, and a straight edge without crossings (e1 = e2 =
//      0, the top-left 16x16) has no area;
//  (d) the decisions and the passes: SMAA only when asked for and ready, its targets follow the
//      front buffer's size, the LUT upload once, the edges and weights cleared before they are
//      drawn, the blend last (in the swap render pass, with the crop and the colour preset), the
//      raw bytes at 720p (the card's ~5.45 MiB);
//  (e) the assembled tree: the shaders (SMAA_GLSL_4, the high preset, the pinned header, the
//      targets' formats), the render passes that clear, the linear clamp sampler for every input,
//      the upload without a flip, the passes' order, the resources only on the first SMAA swap and
//      the targets let go of when not asked for, destroyed by completed submission, the shutdown,
//      the launcher's fourth choice.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "third_party/smaa/AreaTex.h"
#include "third_party/smaa/SearchTex.h"
#include "xenia/gpu/vulkan/swap_fxaa.h"
#include "xenia/gpu/vulkan/swap_smaa.h"

namespace {

namespace ss = xe::gpu::swap_smaa;
namespace sf = xe::gpu::swap_fxaa;
int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string ReadRaw(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
std::string Read(const std::string& path) {
  std::string out;
  for (char c : ReadRaw(path))
    if (c != '\r') out += c;
  return out;
}
std::string Squeeze(const std::string& text) {
  std::string out;
  for (char c : text)
    if (c != ' ' && c != '\n' && c != '\t') out += c;
  return out;
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

// SHA-256 (FIPS 180-4), for the pinned upstream files.
std::string Sha256(const std::string& data) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string message = data;
  const uint64_t bits = uint64_t(data.size()) * 8;
  message += char(0x80);
  while (message.size() % 64 != 56) message += char(0);
  for (int i = 7; i >= 0; --i) message += char((bits >> (i * 8)) & 0xFF);
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(uint8_t(message[chunk + 4 * i])) << 24 |
             uint32_t(uint8_t(message[chunk + 4 * i + 1])) << 16 |
             uint32_t(uint8_t(message[chunk + 4 * i + 2])) << 8 |
             uint32_t(uint8_t(message[chunk + 4 * i + 3]));
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
  }
  char out[65];
  for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
  return out;
}

void Setting() {
  Expect(sf::EffectFromSetting(0) == sf::Effect::kNone && sf::EffectFromSetting(1) == sf::Effect::kFxaa &&
             sf::EffectFromSetting(2) == sf::Effect::kFxaaExtreme,
         "0-2 keep their meaning");
  Expect(sf::EffectFromSetting(3) == sf::Effect::kSmaa1x && uint32_t(sf::Effect::kSmaa1x) == 3,
         "3 is SMAA 1x");
  for (int bad : {-1, 4, 9}) Expect(sf::EffectFromSetting(bad) == sf::Effect::kNone, "unknown is Off");
  Expect(sf::EffectForRenderer(3, true) == sf::Effect::kNone, "OpenGL: Off");
  Expect(std::string(sf::EffectName(sf::Effect::kSmaa1x)) == "SMAA 1x", "its name");
}

void Pinned() {
  const std::string dir = ".deps/xenia-canary/third_party/smaa/";
  struct File {
    const char* name;
    const char* sha256;
  };
  for (File file : {File{"SMAA.hlsl", "4db53d92ef0b45661f8450303589ffabce93c352cae4282a6f32dd2b80efbeb9"},
                    File{"AreaTex.h", "1933bf43ce86ba71f7ed5f0fa94b148610c634dc3395b9bc67a817d51ee73af2"},
                    File{"SearchTex.h", "1fac315ab5f87b60083c9b8387af17f056204d3d1bf21500f2acc5b3e8c1a37a"},
                    File{"LICENSE.txt", "2b848b67ba50ac40b53e11cf1ccdaee2a190ca83b4652cba695a5f1ce81f52ae"}}) {
    const std::string data = ReadRaw(dir + file.name);
    Expect(!data.empty() && Sha256(data) == file.sha256,
           std::string("iryoku/smaa 71c806a ") + file.name + " byte for byte (sha256 " +
               (data.empty() ? std::string("missing") : Sha256(data)) + ")");
  }
  Expect(Sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "the SHA-256 itself");
  // The sizes swap_smaa.h has are the headers'.
  Expect(AREATEX_WIDTH == ss::kAreaTexWidth && AREATEX_HEIGHT == ss::kAreaTexHeight &&
             AREATEX_SIZE == ss::kAreaTexBytes && sizeof(areaTexBytes) == ss::kAreaTexBytes,
         "AreaTex: 160x560 R8G8");
  Expect(SEARCHTEX_WIDTH == ss::kSearchTexWidth && SEARCHTEX_HEIGHT == ss::kSearchTexHeight &&
             SEARCHTEX_SIZE == ss::kSearchTexBytes && sizeof(searchTexBytes) == ss::kSearchTexBytes,
         "SearchTex: 64x16 R8");
  Expect(ss::kSearchTexStagingOffset % 4 == 0 && ss::kSearchTexStagingOffset >= ss::kAreaTexBytes &&
             ss::kStagingBytes == ss::kSearchTexStagingOffset + ss::kSearchTexBytes,
         "the staging buffer: AreaTex, then SearchTex aligned");
}

void Orientation() {
  // AreaTex is 7 subtextures of 80 rows; left half orthogonal areas, right half diagonal ones.
  auto sum = [](uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
    uint64_t total = 0;
    for (uint32_t y = y0; y < y1; ++y)
      for (uint32_t x = x0; x < x1; ++x)
        total += areaTexBytes[(y * ss::kAreaTexWidth + x) * 2] +
                 areaTexBytes[(y * ss::kAreaTexWidth + x) * 2 + 1];
    return total;
  };
  Expect(sum(80, 160, 0, 80) > 0, "AreaTex row 0 is subtexture 0 (it has diagonal areas)");
  Expect(sum(80, 160, 480, 560) == 0, "the last rows are subtexture 6 (no diagonal areas)");
  Expect(sum(0, 16, 0, 16) == 0 && sum(16, 32, 0, 16) > 0,
         "a straight edge without crossings has no area; one crossing has");
  Expect(sum(16, 32, 0, 16) == sum(0, 16, 16, 32), "e1 along u, e2 along v (symmetric)");
}

void Decisions() {
  Expect(!ss::UseSmaa(false, 1280, 720, true), "not asked: no SMAA");
  Expect(!ss::UseSmaa(true, 1280, 720, false), "no resources: no SMAA (the path without AA)");
  Expect(!ss::UseSmaa(true, 0, 720, true), "no size: no SMAA");
  Expect(ss::UseSmaa(true, 1280, 720, true) && ss::UseSmaa(true, 2560, 1440, true), "1x and 2x");
  Expect(ss::NeedsNewTargets(0, 0, 1280, 720) && !ss::NeedsNewTargets(1280, 720, 1280, 720) &&
             ss::NeedsNewTargets(1280, 720, 2560, 1440) && !ss::NeedsNewTargets(1280, 720, 0, 0),
         "the targets follow the front buffer");
  const ss::RtMetrics metrics = ss::MakeRtMetrics(1280, 720);
  Expect(std::fabs(metrics.x - 1.0f / 1280.0f) < 1e-9f && metrics.z == 1280.0f && metrics.w == 720.0f,
         "SMAA_RT_METRICS");
  const double mib = double(ss::RawBytes(1280, 720)) / (1024.0 * 1024.0);
  Expect(mib > 5.4 && mib < 5.5, "720p: edges, weights and LUTs about 5.45 MiB");
  // The passes.
  const ss::Passes first = ss::SmaaPasses(false);
  const ss::Passes later = ss::SmaaPasses(true);
  Expect(first.count == 10 && later.count == 9 && first.pass[2] == ss::Pass::kLutUpload,
         "the LUT upload on the first SMAA swap only");
  for (const ss::Passes& passes : {first, later}) {
    int clear_edges = -1, edges = -1, clear_weights = -1, weights = -1, blend = -1;
    for (uint32_t i = 0; i < passes.count; ++i) {
      if (passes.pass[i] == ss::Pass::kEdgesClear) clear_edges = int(i);
      if (passes.pass[i] == ss::Pass::kEdges) edges = int(i);
      if (passes.pass[i] == ss::Pass::kWeightsClear) clear_weights = int(i);
      if (passes.pass[i] == ss::Pass::kWeights) weights = int(i);
      if (passes.pass[i] == ss::Pass::kBlendToOutput) blend = int(i);
    }
    Expect(clear_edges >= 0 && clear_edges < edges && edges < clear_weights &&
               clear_weights < weights && weights < blend && blend == int(passes.count) - 1,
           "cleared targets, edges, weights, then the blend");
  }
}

void Tree() {
  const std::string core = ".deps/xenia-canary/";
  const std::string shaders = core + "src/xenia/gpu/shaders/";
  const std::string edge = Read(shaders + "smaa_edge.ps.glsl");
  const std::string weight = Read(shaders + "smaa_weight.ps.glsl");
  const std::string blend = Read(shaders + "smaa_blend.ps.glsl");
  const std::string post = Read(shaders + "swap_post.glsli");
  for (const std::string* shader : {&edge, &weight, &post}) {
    Expect(InOrder(*shader, {"layout(push_constant) uniform XeSwapPostConstants {",
                             "#define SMAA_RT_METRICS vec4(xe_swap_post.size_inv, xe_swap_post.grade_on.zw)",
                             "#define SMAA_GLSL_4 1", "#define SMAA_PRESET_HIGH 1",
                             "#include \"../../../../third_party/smaa/SMAA.hlsl\""}),
           "the SMAA shaders: GLSL 4, the high preset, the pinned header, the size in the push constants");
  }
  Expect(edge.find("out vec2 xe_smaa_edges;") != std::string::npos &&
             edge.find("SMAALumaEdgeDetectionPS(texcoord, offset, xe_smaa_color)") != std::string::npos,
         "edges: luma detection, two channels");
  Expect(InOrder(weight, {"binding = 0) uniform sampler2D xe_smaa_edges;",
                          "binding = 1) uniform sampler2D xe_smaa_area;",
                          "binding = 2) uniform sampler2D xe_smaa_search;",
                          "SMAABlendingWeightCalculationPS("}),
         "weights: the edges, AreaTex, SearchTex");
  Expect(blend.find("#define XE_SWAP_POST_SMAA 1") != std::string::npos &&
             InOrder(post, {"#elif defined(XE_SWAP_POST_SMAA)",
                            "vec2 smaa_texcoord = pixel * xe_swap_post.size_inv;",
                            "SMAANeighborhoodBlendingPS(smaa_texcoord, smaa_offset,",
                            "if (xe_swap_post.grade_on.x != 0.0) {"}),
         "the blend: at the crop's corner, before the colour preset");
  Expect(Read(core + "src/xenia/gpu/command_processor.h").find("    kSmaa1x,") != std::string::npos,
         "SwapPostEffect::kSmaa1x");

  const std::string cc = Read(core + "src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  Expect(cc.find("#include \"third_party/smaa/AreaTex.h\"") != std::string::npos &&
             cc.find("smaa_edge_ps.h\"") != std::string::npos &&
             cc.find("smaa_weight_ps.h\"") != std::string::npos &&
             cc.find("smaa_blend_ps.h\"") != std::string::npos,
         "the LUTs and the three shaders are included");
  const std::string resources =
      Between(cc, "bool VulkanCommandProcessor::CreateSwapSmaaResources() {", "\n}\n");
  Expect(InOrder(resources, {"{VK_FORMAT_R8G8_UNORM, kTargetFeatures},",
                             "{VK_FORMAT_R8G8B8A8_UNORM, kTargetFeatures},",
                             "{VK_FORMAT_R8_UNORM, kLutFeatures},",
                             "CreateSwapEffectRenderPass(VK_FORMAT_R8G8_UNORM, true);",
                             "CreateSwapEffectRenderPass(VK_FORMAT_R8G8B8A8_UNORM, true);",
                             "swap_fxaa_sampler_, swap_fxaa_sampler_",
                             "std::memcpy(mapping, areaTexBytes, swap_smaa::kAreaTexBytes);",
                             "searchTexBytes, swap_smaa::kSearchTexBytes);",
                             "ShutdownSwapSmaa();", "return false;"}),
         "resources: the formats checked, the targets' render passes clear, fix 137's linear clamp "
         "sampler, the LUTs staged as the headers store them, a failure shuts it down");
  const std::string render_pass =
      Between(cc, "VkRenderPass VulkanCommandProcessor::CreateSwapEffectRenderPass(", "\n}\n");
  Expect(render_pass.find("clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;") !=
             std::string::npos,
         "a render pass that clears");
  const std::string passes =
      Between(cc, "void VulkanCommandProcessor::RecordSwapSmaaPasses(", "\nvoid VulkanCommandProcessor::");
  Expect(InOrder(passes, {"if (!swap_smaa_luts_uploaded_) {", "VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);",
                          "copy.bufferOffset = 0;", "copy.imageExtent.width = swap_smaa::kAreaTexWidth;",
                          "CmdVkCopyBufferToImage(", "copy.bufferOffset = swap_smaa::kSearchTexStagingOffset;",
                          "CmdVkCopyBufferToImage(", "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);",
                          "destroy_buffers_.emplace_back(GetCurrentSubmission(),",
                          "swap_smaa_luts_uploaded_ = true;",
                          "swap_smaa_targets_.last_submission = GetCurrentSubmission();",
                          "dfn.vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);",
                          "render_pass_begin_info.clearValueCount = 1;", "CmdVkDraw(3, 1, 0, 0);",
                          "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);", "SubmitBarriers(true);"}) &&
             passes.find("imageOffset") == std::string::npos,
         "passes: the LUTs once without a flip (offset 0, the header's rows), the staging buffer freed "
         "by submission, the targets' last submission, the cleared passes, barriers to shader read");
  const std::string prepare =
      Between(cc, "bool VulkanCommandProcessor::PrepareSwapSmaa(", "\n}\n");
  Expect(InOrder(prepare, {"DestroyRetiredSwapSmaaTargets(false);", "if (!asked || !width || !height) {",
                           "RetireSwapSmaaTargets();", "return false;",
                           "swap_smaa_state_ = CreateSwapSmaaResources() ? 1 : -1;",
                           "swap_smaa::NeedsNewTargets(", "CreateSwapSmaaTargets(width, height)",
                           "swap_smaa::UseSmaa("}),
         "PrepareSwapSmaa: nothing made unless asked for, the targets let go of otherwise, made "
         "again for a new size");
  Expect(Between(cc, "void VulkanCommandProcessor::DestroyRetiredSwapSmaaTargets(", "\n}\n")
                 .find("swap_fxaa::CanDestroy(retired.last_submission, completed_submission)") !=
             std::string::npos,
         "retired targets destroyed by completed submission");
  const std::string effects = Between(
      cc, "VulkanCommandProcessor::SwapEffectsPlan VulkanCommandProcessor::PrepareSwapEffects(", "\n}\n");
  Expect(InOrder(effects, {"const bool smaa_asked = fxaa_effect == swap_fxaa::Effect::kSmaa1x;",
                           "PrepareSwapSmaa(smaa_asked && ready, width, height);",
                           "plan.aa = !plan.use", "? (smaa ? SwapPostEffect::kSmaa1x",
                           ": SwapPostEffect::kNone)"}),
         "PrepareSwapEffects: SMAA in the AA slot, none without its resources");
  const std::string swap =
      Between(cc, "void VulkanCommandProcessor::IssueSwap(", "\nbool VulkanCommandProcessor::");
  Expect(InOrder(swap, {"if (swap_plan.use) {", "RecordSwapEffectsSourcePass(",
                        "if (swap_plan.aa == SwapPostEffect::kSmaa1x) {",
                        "RecordSwapSmaaPasses(swap_plan, swap_frame_index);",
                        "VkRenderPassBeginInfo render_pass_begin_info;",
                        "RecordSwapEffectsPass(swap_post_effect, swap_plan, swap_frame_index);"}),
         "IssueSwap: the SMAA passes after the source pass, before the swap render pass");
  const std::string pass = Between(cc, "void VulkanCommandProcessor::RecordSwapEffectsPass(", "\n}\n");
  Expect(InOrder(pass, {"if (plan.aa == SwapPostEffect::kSmaa1x) {",
                        "BindExternalGraphicsPipeline(swap_smaa_blend_pipeline_);",
                        "swap_effects::MakePushConstants(plan.source_width, plan.source_height,",
                        "plan.crop, plan.preset);", "CmdVkDraw(3, 1, 0, 0);", "return;",
                        "effect = plan.aa;"}),
         "the effect pass: SMAA's blend with the crop and the preset, else the plan's AA");
  Expect(InOrder(Between(cc, "void VulkanCommandProcessor::ShutdownSwapFxaa() {", "\n}\n"),
                 {"ShutdownSwapSmaa();", "DestroyRetiredSwapFxaaSources(true);"}),
         "the shutdown: SMAA first (its set layouts use fix 137's sampler)");

  const std::string launcher = Read("src/launcher.cpp");
  Expect(InOrder(launcher, {"{\"antialiasing\", &S::antialiasing, 0, W::live, 0, true, \"Anti-aliasing\",",
                            "{\"Off\", \"FXAA\", \"FXAA Extreme\", \"SMAA 1x\"}},"}),
         "the launcher: SMAA 1x as the fourth choice");
}

}  // namespace

int main() {
  Setting();
  Pinned();
  Orientation();
  Decisions();
  Tree();
  if (failures) {
    std::printf("check-swap-smaa: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-swap-smaa: ok\n");
  return 0;
}
