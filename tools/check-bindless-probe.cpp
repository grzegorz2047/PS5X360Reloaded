// SPDX-License-Identifier: MIT
// Fix 160 (docs/design/bindless.md, step B0): the bindless probe, diagnostics only, off unless
// bindless-probe.txt in the title folder says "on" (read once, at start).
//  The real header (xbox360ps5/bindless_probe.hpp):
//  (a) the switch file: on/off, "sample N", "heap-test", "gpu-timing", comments; read once;
//  (b) 1 draw in N timed, on its own random sequence (fix 73's stays as it was);
//  (c) the binding counts per stage, the share of draws over maxPushDescriptors (none without
//      the extension) and over a push-constant table of 16-bit indices;
//  (d) the parts' own ticks only on timed draws, scaled to ms/s; what bindless and push
//      descriptors would remove and their shares of the GPU thread; the probe's own cost;
//  (e) the image views and samplers made, destroyed and at their peak, counted only while on;
//  (f) the start-up heap test's expected colors.
//  The assembled tree:
//  (g) the device enables the descriptor indexing features only with the probe on, and logs
//      features, limits and extensions (push descriptors with maxPushDescriptors);
//  (h) the start-up test (heap, a write while pending and one after the bind, a slot rewritten
//      after its use, non-uniform and fixed) and the GPU timing run only with the probe on;
//  (i) UpdateBindings' parts are timed in fix 142d's places, every mark behind the switch;
//  (j) the 30 s line from EndSubmission; the texture cache counts views and samplers;
//  (k) the three compute shaders: only the non-uniform one uses nonuniformEXT.
// Runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("xbox360ps5/bindless_probe.hpp")
#include "xbox360ps5/bindless_probe.hpp"
#define HAVE_PROBE 1
#endif

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
bool Has(const std::string& text, const std::string& part) {
  const bool found = text.find(part) != std::string::npos;
  if (!found) std::printf("  (not found: %s)\n", part.substr(0, 100).c_str());
  return found;
}
std::string Body(const std::string& text, const std::string& signature) {
  const size_t at = text.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t end = text.find("\n}\n", at);
  return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

#ifdef HAVE_PROBE
namespace bp = xbox360ps5::bindless_probe;

void CheckSettings() {
  // (a)
  Expect(!bp::Parse("").on && bp::Parse("").sample == 8, "(a) no file text: off, 1 in 8");
  bp::Settings s = bp::Parse("on\n");
  Expect(s.on && s.sample == 8 && s.heap_test && s.gpu_timing, "(a) on: everything, 1 in 8");
  s = bp::Parse("on # probe\nsample 16\nheap-test off\r\ngpu-timing off\n");
  Expect(s.on && s.sample == 16 && !s.heap_test && !s.gpu_timing,
         "(a) sample, heap-test and gpu-timing, comments and CRLF");
  Expect(bp::Parse("on\nsample 0").sample == 1 && bp::Parse("on\nsample 99999").sample == 8 &&
             bp::Parse("on\nsample x").sample == 8 && bp::Parse("on\nsample 5000").sample == 1024,
         "(a) sample clamped to 1-1024, nonsense ignored");
  Expect(!bp::Parse("on\noff").on && !bp::Parse("# on\n").on, "(a) the last on/off wins");
  const char* path = "build/fix-checks/bindless-probe-test.txt";
  if (FILE* f = std::fopen(path, "wb")) {
    std::fputs("on\nsample 4\n", f);
    std::fclose(f);
  }
  const bp::Settings& loaded = bp::Load(path);
  Expect(loaded.on && loaded.sample == 4 && bp::enabled, "(a) Load reads the file");
  if (FILE* f = std::fopen(path, "wb")) {
    std::fputs("off\n", f);
    std::fclose(f);
  }
  Expect(bp::Load(path).on && bp::enabled, "(a) read once: a changed file is not read again");
  std::remove(path);
}

void CheckProbe() {
  // (b)
  bp::Probe probe;
  uint64_t timed = 0;
  for (int i = 0; i < 80000; ++i) timed += probe.BeginDraw(8);
  Expect(timed > 9000 && timed < 11000, "(b) about 1 draw in 8 timed");
  bp::Counters c = probe.Take();
  Expect(c.draws == 80000 && c.timed == timed, "(b) draws and timed draws counted");
  Expect(probe.Take().draws == 0, "(b) Take starts afresh");
  // (c)
  probe.BeginDraw(1);
  probe.CountBindings(3, 2, 20, 16, 32, 256);  // pixel 36 > 32
  probe.BeginDraw(1);
  probe.CountBindings(4, 4, 10, 10, 32, 256);
  probe.BeginDraw(1);
  probe.CountBindings(60, 10, 60, 10, 32, 256);  // 140 indices > 128
  c = probe.Take();
  Expect(c.max_vertex == 70 && c.max_pixel == 70 && c.max_both == 140 &&
             c.max_vertex_textures == 60 && c.max_pixel_samplers == 16,
         "(c) the largest counts per stage and in all");
  Expect(c.over_push_descriptors == 2 && c.over_push_constants == 1,
         "(c) draws over 32 push descriptors a stage, over 128 16-bit indices");
  probe.BeginDraw(1);
  probe.CountBindings(60, 10, 60, 10, 0, 256);
  Expect(probe.Take().over_push_descriptors == 0, "(c) no push descriptors: never over");
  // (d)
  probe.BeginDraw(1u << 30);  // not timed
  probe.Stop(bp::kUpdate, 100, 500);
  probe.CountUpdate(4);
  Expect(probe.Take().ticks[bp::kUpdate] == 0, "(d) an untimed draw adds no time");
  for (int i = 0; i < 1000; ++i) {
    if (!probe.BeginDraw(4)) continue;
    probe.Stop(bp::kReuseCheck, 1000, 1100);
    probe.Stop(bp::kAllocate, 1000, 1200);
    probe.Stop(bp::kUpdate, 1000, 1300);
    probe.Stop(bp::kBind, 1000, 1400);
    probe.Stop(bp::kImageInfos, 0, 9999);  // a zero mark: untimed part
    probe.CountUpdate(5);
  }
  c = probe.Take();
  bp::Window w;
  w.seconds = 1;
  w.ns_per_tick = 1;
  w.executing_ns = 1000000000;
  bp::own_cost.draw_ns = 10;
  bp::own_cost.timed_ns = 100;
  const bp::Summary s = bp::Summarize(c, w);
  const double scale = double(c.draws) / double(c.timed);
  const double reuse = 100.0 * c.timed * scale / 1e6, allocate = 2 * reuse,
               update = 3 * reuse, bind = 4 * reuse;
  auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9 + 1e-6 * std::fabs(b); };
  Expect(near(s.ms_s[bp::kReuseCheck], reuse) && near(s.ms_s[bp::kBind], bind) &&
             s.ms_s[bp::kImageInfos] == 0,
         "(d) the timed parts scaled to all draws, in ms/s");
  Expect(near(s.push_ms_s, reuse + allocate + update) &&
             near(s.bindless_ms_s, reuse + allocate + update + bind) &&
             near(s.bindless_share, s.bindless_ms_s / 1000.0),
         "(d) push descriptors: reuse check + allocation + vkUpdateDescriptorSets; bindless: "
         "the same and the binds; shares of the GPU thread");
  Expect(near(s.own_ms_s, (double(c.draws) * 10 + double(c.timed) * 100) / 1e6),
         "(d) the probe's own cost from the calibration");
  const std::string line = bp::Line(c, w);
  Expect(Has(line, "Bindless probe, last 30 s (fix 160, 1 draw in 4 timed)") &&
             Has(line, "push descriptors: not available") &&
             Has(line, "vkUpdateDescriptorSets 5.0 descriptors a call") &&
             Has(line, "bindless would remove up to"),
         "(d) the line");
  w.max_push_descriptors = 32;
  Expect(Has(bp::Line(c, w), "of draws over 32 a stage"), "(d) the push descriptor limit named");
}

void CheckObjects() {
  // (e)
  bp::enabled = false;
  bp::ViewMade(bp::kView3D);
  bp::SamplerMade();
  Expect(bp::objects.views_made[bp::kView3D].load() == 0 && bp::objects.samplers_made.load() == 0,
         "(e) nothing counted while off");
  bp::enabled = true;
  bp::ViewMade(bp::kViewCube);
  bp::ViewMade(bp::kViewCube);
  bp::ViewMade(bp::kViewCube);
  bp::ViewDestroyed(bp::kViewCube, 2);
  bp::ViewMade(bp::kViewCube);
  Expect(bp::objects.views_made[bp::kViewCube].load() == 4 &&
             bp::objects.views_destroyed[bp::kViewCube].load() == 2 &&
             bp::objects.views_peak[bp::kViewCube].load() == 3,
         "(e) views made, destroyed and the peak of live ones");
  bp::SamplerMade();
  bp::SamplerMade();
  bp::SamplerDestroyed();
  Expect(bp::objects.samplers_made.load() == 2 && bp::objects.samplers_destroyed.load() == 1 &&
             bp::objects.samplers_peak.load() == 2,
         "(e) samplers made, destroyed, peak");
  // (f)
  const auto& checks = bp::HeapChecks();
  const float red[4] = {1, 0, 0, 1}, yellow[4] = {1, 1, 0, 1}, off[4] = {1, 0, 0.5f, 1};
  Expect(checks.size() == 6 && bp::Near(red, checks[0].expected) &&
             bp::Near(yellow, checks[3].expected) && !bp::Near(off, checks[0].expected),
         "(f) six checks: slot 7, pending write, after the bind, rewritten, non-uniform, fixed");
}
#endif

void CheckTree() {
  const std::string device = Read(".deps/xenia-canary/src/xenia/ui/vulkan/vulkan_device.cc");
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string tc = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_texture_cache.cc");
  const std::string shaders = ".deps/xenia-canary/src/xenia/gpu/shaders/";
  Expect(!device.empty() && !cp.empty() && !tc.empty(),
         "the tree is there (run from the assembled tree's root)");
  // (g)
  const size_t gate = device.find("if (probe.on && with_gpu_emulation) {");
  const size_t create = device.find("ifn.vkCreateDevice(");
  Expect(Has(device, "bp::Load(XBOX360PS5_DATA_ROOT \"/bindless-probe.txt\")") &&
             gate != std::string::npos && gate < create &&
             device.find("use.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;") > gate &&
             device.find("enabled_features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;") >
                 gate &&
             device.find("use.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;") < create,
         "(g) the features enabled only with the probe on, before the device is made");
  Expect(Has(device, "VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR") &&
             Has(device, "\"VK_KHR_push_descriptor\"") && Has(device, "\"VK_EXT_descriptor_buffer\"") &&
             Has(device, "Bindless probe (fix 160) extensions: VK_KHR_push_descriptor {}{}") &&
             Has(device, "Bindless probe (fix 160) limits: maxPushConstantsSize {}"),
         "(g) the features, limits and extensions logged");
  // (h)
  const std::string test = Body(cp, "void StartupTest(const ui::vulkan::VulkanDevice* vulkan_device) {");
  Expect(Has(cp, "  if (xbox360ps5::bindless_probe::enabled) {\n    // Fix 160: the start-up test") &&
             Has(test, "VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT") &&
             Has(test, "VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT") &&
             Has(test, "VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT") &&
             Has(test, "pending_at_write = dfn.vkGetFenceStatus(device, fences[0]) == VK_NOT_READY;"),
         "(h) the start-up test, only with the probe on");
  const size_t pending = test.find("pending_at_write =");
  const size_t write_40000 = test.find("write_heap(40000, 1);");
  const size_t bind_second = test.find("bind(cbs[2], 1);");
  const size_t write_50000 = test.find("write_heap(50000, 2);");
  const size_t waited = test.find("check(wait(fences[0]) && wait(fences[1]), \"waits\");");
  const size_t rewrite = test.find("write_heap(7, 3);");
  Expect(pending < write_40000 && bind_second < write_50000 && waited < rewrite &&
             write_40000 < waited,
         "(h) slot 40000 written while the first submission may run, 50000 after the bind, "
         "slot 7 rewritten after both completed");
  Expect(Has(test, "VK_FENCE_CREATE_SIGNALED_BIT") && test.find("vkResetCommandBuffer") == std::string::npos,
         "(h) unused fences do not stall the clean-up; one command buffer per use");
  // (i)
  const std::string bindings = Body(cp, "bool VulkanCommandProcessor::UpdateBindings(");
  const char* const order[] = {
      "const uint64_t fix160_total = fix160_on ? fix160::Mark() : 0;",
      "xbox360ps5::bindless_probe::probe.CountBindings(",
      "const uint64_t fix160_infos = fix160_on ? fix160::Mark() : 0;",
      "kImageInfos,", "// Fix 104: reuse or write, for each stage.", "kReuseCheck,",
      "// Write the new descriptor sets.", "kAllocate,", "dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count,",
      "kUpdate, fix160_update,", "// Bind the new descriptor sets.", "kBind, fix160_bind,",
      "kTotal, fix160_total,"};
  size_t at = 0;
  bool ordered = !bindings.empty();
  for (const char* item : order) {
    const size_t next = bindings.find(item, at);
    if (next == std::string::npos) {
      std::printf("  (out of order or missing: %s)\n", item);
      ordered = false;
      break;
    }
    at = next;
  }
  Expect(ordered, "(i) UpdateBindings timed in fix 142d's places, in order");
  Expect(Has(bindings, "const bool fix160_on = xbox360ps5::bindless_probe::enabled;") &&
             Has(bindings, "  if (fix160_on) {\n    xbox360ps5::bindless_probe::probe.CountBindings(") &&
             Has(cp, "return bp::probe.timed() ? fc::Ticks() - fc::OtherSectionTicks() : 0;"),
         "(i) every mark behind the switch; untimed draws give 0 marks");
  // (j)
  Expect(Has(cp, "  if (xbox360ps5::bindless_probe::enabled) fix160::ReportEvery30s();  // fix 160") &&
             Has(cp, "if (!first && now - report.at < std::chrono::seconds(30)) return;"),
         "(j) the 30 s line from EndSubmission, only with the probe on");
  Expect(Has(tc, "xbox360ps5::bindless_probe::ViewMade(Fix160ViewKind());  // fix 160") &&
             Has(tc, "xbox360ps5::bindless_probe::ViewDestroyed(Fix160ViewKind(), views_.size());") &&
             Has(tc, "xbox360ps5::bindless_probe::ViewMade(xbox360ps5::bindless_probe::kView3DAs2D);") &&
             Has(tc, "xbox360ps5::bindless_probe::SamplerMade();  // fix 160") &&
             Has(tc, "xbox360ps5::bindless_probe::SamplerDestroyed();  // fix 160"),
         "(j) the texture cache counts views and samplers");
  // (k)
  const std::string common = Read(shaders + "bindless_probe.glsli");
  const std::string fixed = Read(shaders + "bindless_probe_fixed.cs.glsl");
  const std::string uniform = Read(shaders + "bindless_probe_uniform.cs.glsl");
  const std::string nonuniform = Read(shaders + "bindless_probe_nonuniform.cs.glsl");
  Expect(Has(common, "uniform texture2DArray xe_heap_2d[65536];") &&
             Has(common, "xe_heap_2d[xe_probe.index]") && Has(fixed, "#define XE_PROBE_FIXED 1") &&
             Has(uniform, "#define XE_PROBE_UNIFORM 1") &&
             Has(nonuniform, "#extension GL_EXT_nonuniform_qualifier : require") &&
             uniform.find("nonuniform") == std::string::npos &&
             fixed.find("nonuniform") == std::string::npos,
         "(k) the three shaders; only the non-uniform one uses nonuniformEXT");
  Expect(Has(cp, "bytecode/vulkan_spirv/bindless_probe_uniform_cs.h\"  // fix 160"),
         "(k) the shaders built into the command processor");
}

}  // namespace

int main() {
#ifdef HAVE_PROBE
  CheckSettings();
  CheckProbe();
  CheckObjects();
#else
  std::printf("FAIL: xbox360ps5/bindless_probe.hpp is not in the tree (fix 160)\n");
  ++failures;
#endif
  CheckTree();
  if (failures) {
    std::printf("check-bindless-probe: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-bindless-probe: ok\n");
  return 0;
}
