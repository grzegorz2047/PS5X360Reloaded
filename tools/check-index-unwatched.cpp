// SPDX-License-Identifier: MIT
// Fix 102 (tools/source-1.0.2/fixes/102-index-unwatched.patch): with "Optimized video memory"
// (memory_boost) on, a 4 KiB unit the guest keeps writing is left unwatched (fix 36) when every
// request for it allows that; a request that does not allow it pins the unit watched for good
// (SharedMemory::PinWatched). Index buffers read from the shared memory asked without allowing it,
// although a draw uses them only as it uses vertex data. Grand Theft Auto IV (545407F2) writes its
// dynamic indices and vertices every frame in the 0xC0000000 view: 1,300 to 2,100 protections a
// second there with as many write notices (1.0.5-work-fix93b/93e), the watch arming alone 50 to
// 90 ms/s of the GPU command thread, and the guest's write faults on top. Checked:
//  (a) a model of the units' life (heat from write faults, pinning, unwatched): a unit the guest
//      writes every frame and two draws read, as vertex and as index data, costs a protection and
//      a fault each frame while the index request pins it, and stops costing them once it may be
//      left unwatched; the GPU copy still equals the guest's bytes at every draw;
//  (b) include/xbox360ps5/frame_cost.hpp: index buffers are their own kind in fix 78's line, and
//      the kinds still fit the per-block byte of kind bits;
//  (c) the assembled tree: the primitive processor's index request allows unwatched units on the
//      console, with its kind set, only for indices read from the shared memory at the draw (the
//      processed-index cache keeps its own watch); converted indices are not requested there.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
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
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
namespace fc = xbox360ps5::frame_cost;

// One unit, as shared_memory.cc keeps it: kHeatToUnwatch write faults leave it unwatched unless it
// is pinned; a request for a watched unit that is not valid uploads and protects it.
struct Unit {
  static constexpr int kHeatToUnwatch = 3;
  uint64_t guest = 0, gpu = 0, hashed = 0;
  bool valid = false, protected_ = false, pinned = false, unwatched = false, seen = false;
  int heat = 0, protections = 0, faults = 0, uploads = 0, stale_draws = 0;
  void GuestWrite(uint64_t value) {
    guest = value;
    if (protected_) {  // the write faults: invalidated, opened, heated
      ++faults;
      protected_ = valid = false;
      if (!pinned && ++heat >= kHeatToUnwatch) unwatched = true;
    }
  }
  void Request(bool allow_unwatched) {
    if (!allow_unwatched) {
      pinned = true;
      unwatched = false;
      heat = 0;
    }
    if (unwatched && allow_unwatched) {  // FilterUnwatched: hash, upload when changed
      if (!seen || hashed != guest) {
        gpu = hashed = guest;
        seen = true;
        ++uploads;
      }
    } else if (!valid) {  // MakeRangeValid: protect, then copy
      protected_ = valid = true;
      ++protections;
      gpu = guest;
      ++uploads;
    }
  }
  void Draw(bool allow_index_unwatched) {
    Request(true);                   // vertex data
    if (gpu != guest) ++stale_draws;
    Request(allow_index_unwatched);  // the indices next to it
    if (gpu != guest) ++stale_draws;
  }
};

void CheckModel() {
  for (const bool fixed : {false, true}) {
    Unit unit;
    for (uint64_t frame = 1; frame <= 60; ++frame) {
      unit.GuestWrite(frame);  // the game refills its dynamic buffer
      unit.Draw(fixed);
      unit.Draw(fixed);
    }
    std::printf("   %s: %d protections, %d faults, %d uploads in 60 frames\n", fixed ? "fix 102" : "before",
                unit.protections, unit.faults, unit.uploads);
    Expect(unit.stale_draws == 0, "every draw reads what the guest wrote");
    if (!fixed) {
      Expect(unit.pinned && !unit.unwatched && unit.protections == 60 && unit.faults == 59,
             "pinned by the index request: a protection and a fault every frame");
    } else {
      Expect(!unit.pinned && unit.unwatched && unit.protections <= Unit::kHeatToUnwatch + 1 &&
                 unit.faults == Unit::kHeatToUnwatch && unit.uploads == 60,
             "left unwatched after a few faults: one upload a frame, no protection");
    }
  }
  // A texture next to it still pins the unit (fix 102 changes the index request only).
  Unit shared;
  for (uint64_t frame = 1; frame <= 10; ++frame) {
    shared.GuestWrite(frame);
    shared.Draw(true);
    shared.Request(false);  // a texture
  }
  Expect(shared.pinned && shared.protections == 10, "a texture request still pins its units");
}

void CheckKinds() {
  Expect(fc::kArmKinds <= 8, "the kinds fit a block's byte of kind bits");
  Expect(std::string(fc::ArmKindName(fc::kArmIndex)) == "index" &&
             std::string(fc::ArmKindName(fc::kArmTexture)) == "texture" &&
             std::string(fc::ArmKindName(fc::kArmOther)) == "other",
         "index buffers are their own kind, the others keep their names");
  fc::HotReporter reporter;
  (void)reporter.Line(1);
  std::thread([] {
    fc::MarkGpuThread();
    fc::ArmKindScope index(fc::kArmIndex);
    fc::ArmTimer timer(0xC0000000u);
    timer.Range(0x0FAF0000u);
    timer.Locked();
    for (int n = 0; n < 4; ++n)
      if (auto* arm = fc::current_arm) arm->Protected();
  }).join();
  const std::string line = reporter.Line(1);
  std::printf("   %s\n", line.c_str());
  Expect(line.find("0FAF0000 4 protections/s, 0 write notices/s (index)") != std::string::npos,
         "fix 78's line names the index kind");
}

void CheckTree() {
  const std::string pp = Read(".deps/xenia-canary/src/xenia/gpu/primitive_processor.cc");
  Expect(pp.find("#if XE_PLATFORM_PS5\n#include \"xbox360ps5/frame_cost.hpp\"  // fix 102") != std::string::npos,
         "the primitive processor sees the kinds on the console");
  const size_t dma = pp.find("if (cacheable.index_buffer_type == ProcessedIndexBufferType::kGuestDMA ||\n"
                             "      cacheable.index_buffer_type ==\n"
                             "          ProcessedIndexBufferType::kHostBuiltinForDMA) {");
  const size_t kind = pp.find("xbox360ps5::frame_cost::kArmIndex);  // fix 102", dma);
  const size_t allow = pp.find("constexpr bool kIndicesAllowUnwatched = true;  // fix 102", dma);
  const size_t other = pp.find("#else\n    constexpr bool kIndicesAllowUnwatched = false;\n#endif", allow);
  const size_t request = pp.find("if (!shared_memory_.RequestRange(guest_index_base,\n"
                                 "                                     guest_index_buffer_needed_bytes,\n"
                                 "                                     kIndicesAllowUnwatched)) {",
                                 dma);
  Expect(dma != std::string::npos && kind != std::string::npos && allow != std::string::npos && other != std::string::npos &&
             request != std::string::npos && dma < kind && kind < allow && allow < other && other < request &&
             request - dma < 1600,
         "only the shared-memory index request allows unwatched units, on the console, with its kind set");
  Expect(pp.find("RequestRange(", request + 40) == std::string::npos, "no other shared-memory request in the primitive processor");
  const std::string sm = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.cc");
  Expect(sm.find("if (allow_unwatched && UnwatchedPagesEnabled()) {") != std::string::npos &&
             sm.find("(written_by_gpu || !request_allows_unwatched_)) {\n      PinWatched(valid_page_first, valid_page_last);") !=
                 std::string::npos,
         "shared memory still leaves units unwatched only with the option on, and pins the other requests' units");
}

}  // namespace

int main() {
  CheckModel();
  CheckKinds();
  CheckTree();
  if (failures) {
    std::printf("check-index-unwatched: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-index-unwatched: ok\n");
  return 0;
}
