// SPDX-License-Identifier: MIT
// Fix 86 (tools/source-1.0.2/fixes/86-draw-lock-deferred.patch): with "Optimized video memory"
// (memory_boost) off, every draw with vertex data took the global critical region before
// requesting its vertex buffers (VulkanCommandProcessor::IssueDrawInner's hoisted lock), although
// fix 55 answers valid ranges without the lock either way and a request that needs the lock takes
// it itself. A Modern Warfare 2 run on the 1.0.5 perf builds: the GPU thread took the global lock
// about 256,000 times a second (177,000 draws) and waited 128 ms/s for it. The hoisted lock is now
// deferred whenever valid ranges are answered without the lock, whatever memory_boost says.
// Checked in the assembled tree (the GPU code is not built on the host):
//  (a) the hoisted lock is deferred on lock_free_valid alone, on the console;
//  (b) SharedMemory::RequestRange answers an all-valid range before taking the lock, without
//      asking for memory_boost, and takes the lock itself for the rest, so a deferred hoist
//      changes no answer.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

void CheckTree() {
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t hoist = vk.find("      auto shared_memory_request_range_hoisted =\n"
                               "          xbox360ps5::gpu_diag::lock_free_valid.load(std::memory_order_relaxed)\n"
                               "              ? global_critical_region::AcquireDeferred()\n"
                               "              : global_critical_region::Acquire();");
  const size_t ps5 = vk.rfind("#if XE_PLATFORM_PS5", hoist);
  const size_t loop = vk.find("for (uint32_t i = 0; i < vfetch_current_queued; ++i) {", hoist);
  Expect(hoist != std::string::npos && ps5 != std::string::npos && hoist - ps5 < 1200 && loop != std::string::npos,
         "the hoisted lock is deferred on lock_free_valid alone, on the console, before the vertex requests");
  Expect(vk.find("xbox360ps5::gpu_diag::enabled.load(std::memory_order_relaxed) &&\n"
                 "                  xbox360ps5::gpu_diag::lock_free_valid.load(") == std::string::npos,
         "no longer also asks for memory_boost");
  const std::string sm = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.cc");
  const size_t fn = sm.find("bool SharedMemory::RequestRange(uint32_t start, uint32_t length,");
  const size_t valid = sm.find("if (xbox360ps5::gpu_diag::lock_free_valid.load(std::memory_order_relaxed) &&\n"
                               "      xbox360ps5::gpu_diag::RangeBitsAllSet(",
                               fn);
  const size_t answered = sm.find("    return true;\n  }", valid);
  const size_t lock = sm.find("    auto global_lock = global_critical_region_.Acquire();", valid);
  Expect(fn != std::string::npos && valid != std::string::npos && answered < lock && lock != std::string::npos,
         "RequestRange answers an all-valid range before the lock and takes the lock itself for the rest");
}

}  // namespace

int main() {
  CheckTree();
  if (failures) {
    std::printf("check-draw-lock-deferred: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-draw-lock-deferred: ok\n");
  return 0;
}
