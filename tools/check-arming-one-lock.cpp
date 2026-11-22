// SPDX-License-Identifier: MIT
// Fix 84 (tools/source-1.0.2/fixes/84-arming-one-lock.patch): Memory::
// EnablePhysicalMemoryAccessCallbacks (the last step of MakeRangeValid) arms a write watch on the
// four views of physical memory, and each view's PhysicalHeap::EnableAccessCallbacks took the
// global critical region itself; guest threads took it between the views' calls, so the later
// views waited for it again (1.0.5_candidate_50, Modern Warfare 2: view C0000000 20.9 of its
// 21.6 ms/s waiting for the lock, protecting nothing). The region is now taken once for all
// four. Checked in the assembled tree (the core is not built on the host):
//  (a) EnablePhysicalMemoryAccessCallbacks takes the region before the first view and keeps it
//      for all four, and on the console counts the wait in view A0000000's lock time;
//  (b) each view still takes the region itself (it is called from elsewhere too), which is the
//      same thread's again: the console's global mutex is recursive.
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
  const std::string memory = Read(".deps/xenia-canary/src/xenia/memory.cc");
  const size_t fn = memory.find("void Memory::EnablePhysicalMemoryAccessCallbacks(");
  const size_t lock = memory.find("  auto global_lock = global_critical_region::Acquire();", fn);
  const size_t counted = memory.find("xbox360ps5::frame_cost::alias_lock_ticks[0],", lock);
  size_t at = lock, views = 0;
  for (const char* view : {"heaps_.vA0000000.EnableAccessCallbacks(", "heaps_.vC0000000.EnableAccessCallbacks(",
                           "heaps_.vE0000000.EnableAccessCallbacks(", "heaps_.v7F000000.EnableAccessCallbacks("}) {
    const size_t next = memory.find(view, at);
    if (next == std::string::npos) break;
    at = next;
    ++views;
  }
  const size_t end = memory.find("\n}\n", fn);
  Expect(fn != std::string::npos && lock != std::string::npos && counted != std::string::npos && counted < at && views == 4 &&
             at < end,
         "the region taken once before the four views, its wait counted for view A0000000");
  Expect(memory.find("#if XE_PLATFORM_PS5\n  const uint64_t lock_start = xbox360ps5::frame_cost::Ticks();\n#endif", fn) < lock,
         "the timing only on the console");
  const size_t heap = memory.find("void PhysicalHeap::EnableAccessCallbacks(uint32_t physical_address,");
  Expect(heap != std::string::npos &&
             memory.find("auto global_lock = global_critical_region_.Acquire();", heap) != std::string::npos,
         "each view still takes the region itself");
  const std::string mutex = Read(".deps/xenia-canary/src/xenia/base/mutex.h");
  const size_t ps5 = mutex.find("#elif XE_PLATFORM_PS5");
  const size_t global = mutex.find("class alignas(64) xe_global_mutex {", ps5);
  const size_t recursion = mutex.find("uint32_t recursion_count_{0};", global);
  const size_t other = mutex.find("using global_mutex_type = std::recursive_mutex;");
  Expect(ps5 != std::string::npos && global != std::string::npos && recursion != std::string::npos &&
             recursion - global < 400 && other != std::string::npos,
         "the global mutex is recursive on the console (and elsewhere)");
}

}  // namespace

int main() {
  CheckTree();
  if (failures) {
    std::printf("check-arming-one-lock: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-arming-one-lock: ok\n");
  return 0;
}
