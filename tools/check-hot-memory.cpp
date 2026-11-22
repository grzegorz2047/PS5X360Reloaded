// SPDX-License-Identifier: MIT
// Fix 78 (tools/source-1.0.2/fixes/78-hot-memory.patch): which memory the write watches are
// armed and lifted on. 1.0.5_candidate_50: Modern Warfare 2's view A0000000 made 6,000
// protection calls a second (216 ms/s) with 1,956 write notices a second; before changing how
// watches are armed, the blocks they are armed on, how often the guest writes them, and what the
// GPU asked them for.
//  (a) include/xbox360ps5/frame_cost.hpp: ArmKindScope (the GPU thread's kind of request, nested
//      scopes restore the outer one); ArmTimer::Range and Protected count a protection call for
//      the 64 KiB block of physical memory its request starts in, with the kind; NoteHotWrite
//      counts a write notice for its block from any thread; HotReporter gives the 5 blocks with
//      the most protection calls since its previous line, and how many blocks were armed.
//  (b) the assembled tree: EnableAccessCallbacks gives the timer its physical address,
//      TriggerCallbacks counts every write notice by block, the vertex-data, memexport and
//      texture requests set their kind, and the summary gives the line after fix 77's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
  return text.str();
}
namespace fc = xbox360ps5::frame_cost;

void Arm(uint32_t heap_base, uint32_t physical, int protections) {
  fc::ArmTimer timer(heap_base);
  timer.Range(physical);
  timer.Locked();
  for (int n = 0; n < protections; ++n)
    if (auto* arm = fc::current_arm) arm->Protected();
}

void CheckCounting() {
  fc::HotReporter reporter;
  (void)reporter.Line(1);
  Arm(0xA0000000u, 0x00120000u, 3);  // not the GPU thread: not counted
  Expect(fc::hot_protects[0x12] == 0, "counted on the GPU thread only");
  std::thread([] {
    fc::MarkGpuThread();
    Expect(fc::arm_kind == fc::kArmOther, "no kind set: other");
    {
      fc::ArmKindScope vertex(fc::kArmVertex);
      for (int n = 0; n < 40; ++n) Arm(0xA0000000u, 0x0AB30000u + (n & 0xFFFF), 1);
      {
        fc::ArmKindScope texture(fc::kArmTexture);
        Expect(fc::arm_kind == fc::kArmTexture, "a nested kind");
        Arm(0xA0000000u, 0x0AB3F000u, 2);
      }
      Expect(fc::arm_kind == fc::kArmVertex, "the outer kind again after the nested scope");
    }
    {
      fc::ArmKindScope memexport(fc::kArmMemexport);
      for (int n = 0; n < 10; ++n) Arm(0xA0000000u, 0x01000000u, 1);
    }
    for (int n = 0; n < 6; ++n) Arm(0xA0000000u, 0x02000000u, 1);  // other
    Arm(0x40000000u, 0x03000000u, 5);  // not one of the four views: not counted
    Arm(0xA0000000u, 0x04000000u, 0);  // armed, nothing protected
  }).join();
  for (int n = 0; n < 7; ++n) fc::NoteHotWrite(0x0AB30010u);  // the guest writes, from any thread
  fc::NoteHotWrite(0xEAB30000u);                                // another view's address: the same block
  fc::NoteHotWrite(0x05000000u);                                // a block not armed in this window
  Expect(fc::hot_protects[0x0AB3] == 42 && fc::hot_notices[0x0AB3] == 8, "block 0AB30000: 42 protections, 8 notices");
  Expect(fc::hot_protects[0x0300] == 0, "a virtual heap's arming is not counted");
  const std::string line = reporter.Line(2);
  std::printf("   %s\n", line.c_str());
  Expect(line.find("Frame time hot memory (64 KiB physical blocks by watch arming): "
                   "0AB30000 21 protections/s, 4 write notices/s (vertex, texture); "
                   "01000000 5 protections/s, 0 write notices/s (memexport); "
                   "02000000 3 protections/s, 0 write notices/s (other); 3 blocks armed, 2 with write notices") == 0,
         "the blocks by protection calls, their notices and kinds");
  Expect(reporter.Line(1) == "Frame time hot memory (64 KiB physical blocks by watch arming): 0 blocks armed, 0 with write notices",
         "only since the previous line");
}

void CheckTop() {
  fc::HotReporter reporter;
  (void)reporter.Line(1);
  std::thread([] {
    fc::MarkGpuThread();
    for (uint32_t b = 0; b < 8; ++b)
      for (uint32_t n = 0; n <= b; ++n) Arm(0xA0000000u, 0x10000000u + (b << 16), 1);
  }).join();
  const std::string line = reporter.Line(1);
  std::printf("   %s\n", line.c_str());
  Expect(line.find(": 10070000 8 protections/s") != std::string::npos && line.find("; 10030000 4 protections/s") != std::string::npos &&
             line.find("10020000") == std::string::npos && line.find("8 blocks armed") != std::string::npos,
         "the 5 blocks with the most calls, largest first; all 8 counted");
}

void CheckTree() {
  const std::string code = Read(".deps/xenia-canary/src/xenia/memory.cc");
  const size_t timer = code.find("xbox360ps5::frame_cost::ArmTimer arm_timer(heap_base_);  // fix 74");
  const size_t range = code.find("arm_timer.Range(physical_address);  // fix 78", timer);
  Expect(timer != std::string::npos && range != std::string::npos && range - timer < 120,
         "EnableAccessCallbacks gives the timer its physical address");
  const size_t notice = code.find("if (is_write) xbox360ps5::frame_cost::NoteWriteNotice(heap_base_);  // fix 74");
  const size_t hot = code.find("xbox360ps5::frame_cost::NoteHotWrite(GetPhysicalAddress(virtual_address));  // fix 78", notice);
  Expect(notice != std::string::npos && hot != std::string::npos && hot - notice < 200, "TriggerCallbacks counts every write notice by block");
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t vertex = vk.find("xbox360ps5::frame_cost::ArmKindScope frame_cost_kind(xbox360ps5::frame_cost::kArmVertex);  // fix 78");
  const size_t vfetch = vk.find("shared_memory_->RequestRange(vfetch_addresses[i] << 2,", vertex);
  const size_t mx = vk.find("xbox360ps5::frame_cost::ArmKindScope frame_cost_kind(xbox360ps5::frame_cost::kArmMemexport);  // fix 78");
  const size_t mx_request = vk.find("shared_memory_->RequestRange(memexport_range_base_bytes,", mx);
  Expect(vertex != std::string::npos && vfetch != std::string::npos && vfetch - vertex < 200 && mx != std::string::npos &&
             mx_request != std::string::npos && mx_request - mx < 300,
         "vertex-data and memexport requests set their kind");
  const std::string tex = Read(".deps/xenia-canary/src/xenia/gpu/texture_cache.cc");
  const size_t request = tex.find("void TextureCache::RequestTextures(uint32_t used_texture_mask) {");
  const size_t kind = tex.find("xbox360ps5::frame_cost::ArmKindScope frame_cost_kind(xbox360ps5::frame_cost::kArmTexture);  // fix 78", request);
  Expect(request != std::string::npos && kind != std::string::npos && kind - request < 300, "texture requests set their kind");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t waits = main_code.find("XELOGW(\"{}\", frame_cost_wait_reg.Line(double(summary_samples)));");
  const size_t hot_line = main_code.find("XELOGW(\"{}\", frame_cost_hot.Line(double(summary_samples)));");
  Expect(waits != std::string::npos && hot_line != std::string::npos && waits < hot_line, "the summary gives the line after fix 77's");
}

}  // namespace

int main() {
  CheckCounting();
  CheckTop();
  CheckTree();
  if (failures) {
    std::printf("check-hot-memory: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-hot-memory: ok\n");
  return 0;
}
