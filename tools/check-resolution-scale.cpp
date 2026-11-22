// SPDX-License-Identifier: MIT
// Fix 117 (tools/source-1.0.2/fixes/117-resolution-scale.patch): "Resolution scale (test)", a game
// rendered at twice its resolution (the core's draw_resolution_scale), with lines that tell on the
// console whether it works and a floor of free direct memory for the scaled resolve heaps.
//  (a) include/xbox360ps5/resolution_scale.hpp: the choices (0 1x, 1 2x, nothing else gives more
//      than 1x), the 1 GiB floor (a heap that would leave less is refused, an unknown amount is
//      allowed), the counters, and the lines' texts (none every 10 s at 1x);
//  (b) the launcher: option draw_scale, a start option a game may have of its own, 1x by default,
//      exactly the two choices; Apply sets both core scales from it; the old resolution_scale
//      key of settings.txt (written as 1 by every earlier build) is still not read, so it cannot
//      turn into 2x; the launcher's built-in presets give no game a scale;
//  (c) the core: the device line after the clamp, the sparse buffers' line, the floor asked
//      before each heap's vkAllocateMemory, the refused commits not logged one by one, the 10 s line.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "xbox360ps5/resolution_scale.hpp"

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}

namespace rs = xbox360ps5::resolution_scale;

void CheckHeader() {
  Expect(rs::kChoices == 2, "two choices");
  Expect(rs::ScaleForChoice(0) == 1, "choice 0 is 1x");
  Expect(rs::ScaleForChoice(1) == 2, "choice 1 is 2x");
  for (int choice : {-1, 2, 3, 7, 100}) Expect(rs::ScaleForChoice(choice) == 1, "choice " + std::to_string(choice) + " is 1x");

  constexpr uint64_t kHeap = uint64_t(16) << 20;
  constexpr int64_t kGiB = int64_t(1) << 30;
  Expect(rs::kDirectFloor == uint64_t(kGiB), "the floor is 1 GiB");
  Expect(rs::MayCommit(-1, kHeap), "unknown free memory: allowed");
  Expect(rs::MayCommit(4 * kGiB, kHeap), "4 GiB free: allowed");
  Expect(rs::MayCommit(kGiB + int64_t(kHeap), kHeap), "exactly the floor left: allowed");
  Expect(!rs::MayCommit(kGiB + int64_t(kHeap) - 1, kHeap), "one byte under the floor: refused");
  Expect(!rs::MayCommit(kGiB, kHeap), "1 GiB free: a heap would leave less, refused");
  Expect(!rs::MayCommit(0, kHeap), "nothing free: refused");

  rs::Counters c;
  Expect(rs::Line(c, 3000ll << 20).empty(), "no 10 s line at 1x");
  c.scale = 2;
  c.sparse = true;
  Expect(rs::Admit(c, 3000ll << 20, kHeap), "3000 MiB free: admitted");
  rs::Allocated(c, true, kHeap);
  Expect(rs::Admit(c, 2000ll << 20, kHeap), "2000 MiB free: admitted");
  rs::Allocated(c, false, kHeap);
  Expect(!rs::Admit(c, 1000ll << 20, kHeap), "1000 MiB free: refused");
  Expect(!rs::Admit(c, 900ll << 20, kHeap), "900 MiB free: refused");
  Expect(rs::Admit(c, -1, kHeap), "unknown: admitted");
  rs::Allocated(c, true, kHeap);
  Expect(c.heaps == 2 && c.heap_bytes == 2 * kHeap, "two heaps of 16 MiB counted");
  Expect(c.failed == 1 && c.refused == 2, "one failed, two refused");
  Expect(c.lowest_free == (900ll << 20), "lowest free seen at a commit: 900 MiB");
  const std::string line = rs::Line(c, 1200ll << 20);
  Expect(line == "Resolution scale (fix 117): 2x, scaled resolve sparse, 2 heaps (32 MiB) committed, 2 refused "
                 "(under 1024 MiB free), 1 failed; direct free 1200 MiB (lowest at a commit 900 MiB)",
         "10 s line: " + line);
  c.sparse = false;
  Expect(Has(rs::Line(c, -1), "scaled resolve none,") && Has(rs::Line(c, -1), "direct free -1 MiB"),
         "10 s line without sparse buffers or a known free amount");

  rs::Device d;
  d.asked_x = d.asked_y = 2;
  d.used_x = d.used_y = 1;
  d.max_storage_buffer_range = uint64_t(1) << 32;
  d.max_image_2d = 16384;
  d.max_framebuffer_width = d.max_framebuffer_height = 16384;
  const std::string no_sparse = rs::DeviceLine(d, 6122ll << 20);
  Expect(no_sparse == "Resolution scale (fix 117): asked 2x2, in use 1x1; device sparseBinding no, sparseResidencyBuffer no, "
                      "fragmentShaderPixelInterlock no, maxStorageBufferRange 4096 MiB, maxImageDimension2D 16384, "
                      "maxFramebuffer 16384x16384; direct free 6122 MiB (the device cannot: 1x)",
         "device line without sparse: " + no_sparse);
  d.sparse_binding = d.sparse_residency_buffer = d.interlock = true;
  d.used_x = d.used_y = 2;
  const std::string sparse = rs::DeviceLine(d, -1);
  Expect(Has(sparse, "in use 2x2; device sparseBinding yes, sparseResidencyBuffer yes, fragmentShaderPixelInterlock yes") &&
             !Has(sparse, "cannot"),
         "device line with sparse: " + sparse);
  d.asked_x = d.asked_y = d.used_x = d.used_y = 1;
  Expect(!Has(rs::DeviceLine(d, -1), "cannot"), "1x asked: no 'cannot'");

  const std::string made = rs::SparseLine(rs::Sparse::kMade, 1, 128, 0, 6000ll << 20);
  Expect(made == "Resolution scale (fix 117): scaled resolve: 1 sparse buffer(s) of 2 GiB made, up to 128 heaps of 16 MiB; "
                 "direct free 6000 MiB, heaps stop below 1024 MiB free",
         "sparse line: " + made);
  Expect(Has(rs::SparseLine(rs::Sparse::kBufferFailed, 0, 0, -2, -1), "sparse buffer 0 of 2 GiB not made (VkResult -2)"),
         "sparse buffer failure line");
  Expect(Has(rs::SparseLine(rs::Sparse::kNoSupport, 0, 0, 0, -1), "no sparse binding on the device"), "no sparse line");
  Expect(Has(rs::SparseLine(rs::Sparse::kNoMemoryType, 0, 0, 0, -1), "no device-local memory type"), "memory type line");
  Expect(Has(rs::SparseLine(rs::Sparse::kOffByCvar, 0, 0, 0, -1), "tiled_shared_memory false"), "cvar line");
}

void CheckLauncher() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::string header = Read("include/xbox360ps5/launcher.hpp");
  Expect(Has(header, "int draw_scale = 0;"), "Settings::draw_scale, 1x by default");
  Expect(Has(header, "int resolution_scale = 1;"), "the old field stays (written to settings.txt as before)");
  const std::regex option(
      R"re(\{"draw_scale", &S::draw_scale, (\d+), W::(\w+), (\d+), (true|false), "([^"]*)",\s*"[^"]*",\s*\{([^}]*)\}\})re");
  std::smatch m;
  const bool found = std::regex_search(launcher, m, option);
  Expect(found, "the draw_scale option");
  if (found) {
    Expect(m[1] == "0", "in the Video sub-menu");
    Expect(m[2] == "start", "a start option (the core reads the scale as it starts)");
    Expect(m[3] == "0", "1x recommended");
    Expect(m[4] == "true", "a game may have its own");
    Expect(m[5] == "Resolution scale (test)", "label (" + m[5].str() + ")");
    Expect(m[6] == R"x("1x (720p, native)", "2x (1440p, demanding)")x", "exactly 1x and 2x (" + m[6].str() + ")");
  }
  Expect(Has(launcher, "cvars::draw_resolution_scale_x = cvars::draw_resolution_scale_y =\n"
                       "      xbox360ps5::resolution_scale::ScaleForChoice(draw_scale);"),
         "Apply sets both scales from draw_scale");
  Expect(!Has(launcher, "= resolution_scale;"), "the scales no longer come from the old field");
  Expect(Has(launcher, "// resolution_scale: not read."), "the old key is still skipped in Load");
  const size_t table = launcher.find("built_in[] = {");
  const size_t table_end = launcher.find("\n  };", table);
  Expect(table != std::string::npos && launcher.substr(table, table_end - table).find("draw_scale") == std::string::npos,
         "no built-in preset sets a scale");
  const std::string presets = Read("assets/presets.txt");
  Expect(!Has(presets, "draw_scale"), "no presets.txt entry sets a scale");
}

void CheckCore() {
  const std::string processor = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string texture_cache = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_texture_cache.cc");
  const std::string render_targets = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc");
  const std::string game_main = Read("src/canary_game_main.cpp");
  const size_t clamp = processor.find("ClampDrawResolutionScaleToMaxSupported(");
  const size_t device_line = processor.find("xbox360ps5::resolution_scale::DeviceLine(");
  const size_t rt_cache = processor.find("render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(");
  Expect(clamp != std::string::npos && device_line > clamp && device_line < rt_cache,
         "the device line comes after the clamp, before the render target cache");
  Expect(Has(processor, "xbox360ps5::resolution_scale::counters.scale.store(\n"
                        "        std::max(draw_resolution_scale_x, draw_resolution_scale_y),"),
         "the scale in use is recorded for the 10 s line");
  for (const char* outcome : {"Sparse::kMade", "Sparse::kNoSupport", "Sparse::kBufferFailed", "Sparse::kNoMemoryType",
                              "Sparse::kOffByCvar"})
    Expect(Has(texture_cache, std::string("xbox360ps5::resolution_scale::") + outcome), std::string("sparse line ") + outcome);
  // The sparse heaps: the floor asked just before vkAllocateMemory, its outcome counted after.
  const size_t allocate = texture_cache.find("dfn.vkAllocateMemory(device, &allocate_info, nullptr, &heap)");
  const size_t admit = allocate == std::string::npos ? std::string::npos
                                                     : texture_cache.rfind("xbox360ps5::resolution_scale::Admit(", allocate);
  const size_t heap_check = texture_cache.rfind("if (scaled_resolve_heaps_[heap_index] == VK_NULL_HANDLE) {", allocate);
  const size_t allocated = texture_cache.find("xbox360ps5::resolution_scale::Allocated(", allocate);
  Expect(allocate != std::string::npos && admit != std::string::npos && heap_check != std::string::npos &&
             admit > heap_check && allocated != std::string::npos && allocated - allocate < 200,
         "the floor is asked before each heap's allocation, the outcome counted after");
  // The fallback buffers (no sparse buffers while scaled): the same floor before vmaCreateBuffer.
  const size_t vma = texture_cache.find("vmaCreateBuffer(\n            vma_allocator_, &buffer_create_info");
  const size_t fallback_admit =
      vma == std::string::npos ? std::string::npos : texture_cache.rfind("xbox360ps5::resolution_scale::Admit(", vma);
  Expect(vma != std::string::npos && fallback_admit != std::string::npos && vma - fallback_admit < 800 &&
             texture_cache.find("xbox360ps5::resolution_scale::Allocated(", vma) != std::string::npos,
         "the fallback buffers keep the floor");
  Expect(Has(render_targets, "Fix 117") && Has(render_targets, "failed_commits < 8"),
         "a refused commit is logged a few times, not at every resolve");
  Expect(Has(game_main, "xbox360ps5::resolution_scale::Line("), "the 10 s line");
}

}  // namespace

int main() {
  CheckHeader();
  CheckLauncher();
  CheckCore();
  if (failures) {
    std::printf("check-resolution-scale: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-resolution-scale: ok\n");
  return 0;
}
