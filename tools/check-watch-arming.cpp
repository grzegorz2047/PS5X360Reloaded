// SPDX-License-Identifier: MIT
// Fix 74 (tools/source-1.0.2/fixes/74-watch-arming.patch): what arming a write watch costs, by
// view of physical memory (0xA0000000, 0xC0000000, 0xE0000000, 0x7F000000). MakeRangeValid's last
// step arms all four views; fix 62's "marking valid" was 248 ms/s in Modern Warfare 2 on
// 1.0.4_candidate_31. Measured before deciding whether to protect fewer views.
//  (a) include/xbox360ps5/frame_cost.hpp: ArmTimer counts on the GPU thread only and for the four
//      views only; calls, protection calls (through current_arm while it runs), the time in the
//      call and the part until the lock was taken; nested timers restore the outer one;
//      write notices are counted per view from any thread; ArmReporter gives each view's rates
//      since its previous line;
//  (b) the assembled tree: PhysicalHeap::EnableAccessCallbacks times itself after its early
//      return, notes the lock right after taking it, and EnableAccessCallbacksInner notes each
//      protection call before making it; the line in the summary after fix 73's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
void Spin(uint64_t ns) {
  const uint64_t end = fc::NowNs() + ns;
  while (fc::NowNs() < end) {
  }
}

void CheckTimer() {
  Expect(fc::AliasOf(0xA0000000u) == 0 && fc::AliasOf(0xC0000000u) == 1 && fc::AliasOf(0xE0000000u) == 2 &&
             fc::AliasOf(0x7F000000u) == 3 && fc::AliasOf(0x40000000u) == fc::kAliases,
         "the four views, nothing else");
  fc::ArmReporter reporter;
  (void)reporter.Line(1);
  {
    fc::ArmTimer not_gpu(0xA0000000u);
    Expect(fc::current_arm == nullptr, "not on the GPU thread: not current");
  }
  Expect(fc::alias_calls[0] == 0, "counted on the GPU thread only");
  const uint64_t start = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    for (int n = 0; n < 3; ++n) {
      fc::ArmTimer timer(0xA0000000u);
      Spin(2000000);  // 2 ms waiting for the lock
      timer.Locked();
      for (int p = 0; p < 2; ++p) {
        if (auto* arm = fc::current_arm) arm->Protected();
      }
      {
        fc::ArmTimer other(0x40000000u);  // a virtual heap: not counted, the outer one stays current
        Expect(fc::current_arm == &timer, "an uncounted heap leaves the current arming as it was");
      }
      Spin(1000000);  // 1 ms protecting
    }
    {
      fc::ArmTimer e(0xE0000000u);
      if (auto* arm = fc::current_arm) arm->Protected();
    }
    Expect(fc::current_arm == nullptr, "none current after the last");
  }).join();
  const double seconds = double(fc::NowNs() - start) / 1e9;
  const std::string line = reporter.Line(seconds);
  std::printf("   %s\n", line.c_str());
  Expect(fc::alias_calls[0] == 3 && fc::alias_protects[0] == 6, "view A0000000: 3 calls, 6 protections");
  Expect(fc::alias_calls[2] == 1 && fc::alias_protects[2] == 1, "view E0000000: 1 call, 1 protection");
  Expect(fc::alias_calls[1] == 0 && fc::alias_calls[3] == 0, "the others: none");
  // Write notices come from any thread.
  for (int n = 0; n < 3; ++n) fc::NoteWriteNotice(0xC0000000u);
  fc::NoteWriteNotice(0x40000000u);
  Expect(fc::alias_write_notices[1] == 3 && fc::alias_write_notices[0] == 0, "write notices counted per view, from any thread");
  const std::string notices = reporter.Line(1);
  Expect(notices.find("C0000000 0 calls/s, 0 protections/s, 0.0 ms/s (lock 0.0 ms/s), 3 write notices/s") != std::string::npos,
         "the line gives the write notices");
  const auto value = [&](const std::string& after) {
    const size_t at = line.find(after);
    return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + after.size());
  };
  const size_t a0 = line.find("A0000000 ");
  const std::string a0_part = line.substr(a0, line.find(';', a0) - a0);
  const auto in_a0 = [&](const std::string& after) {
    const size_t at = a0_part.find(after);
    return at == std::string::npos ? -1.0 : std::atof(a0_part.c_str() + at + after.size());
  };
  const double total = in_a0("protections/s, ") * seconds, lock = in_a0("(lock ") * seconds;
  Expect(total > 7.5 && total < 12, "A0000000: about 9 ms in all (" + std::to_string(total) + ")");
  Expect(lock > 5.5 && lock < 8, "A0000000: about 6 ms until the lock (" + std::to_string(lock) + ")");
  Expect(value("Frame time watch arming (GPU thread) by view: A0000000 ") >= 0 && line.find("7F000000 0 calls/s") != std::string::npos,
         "the line names every view");
}

void CheckTree() {
  const std::string code = Read(".deps/xenia-canary/src/xenia/memory.cc");
  const size_t fn = code.find("void PhysicalHeap::EnableAccessCallbacks(uint32_t physical_address,");
  const size_t early = code.find("if (!enable_invalidation_notifications && !enable_data_providers) {", fn);
  const size_t timer = code.find("xbox360ps5::frame_cost::ArmTimer arm_timer(heap_base_);  // fix 74", early);
  const size_t lock = code.find("auto global_lock = global_critical_region_.Acquire();", timer);
  const size_t locked = code.find("arm_timer.Locked();  // fix 74", lock);
  const size_t inner = code.find("EnableAccessCallbacksInner<true, true>(system_page_first,", locked);
  Expect(fn != std::string::npos && early < timer && timer < lock && lock < locked && locked < inner && inner != std::string::npos,
         "EnableAccessCallbacks times itself after its early return and notes the lock right after taking it");
  const size_t body = code.find("XE_NOINLINE void PhysicalHeap::EnableAccessCallbacksInner(");
  size_t at = body, notes = 0;
  while ((at = code.find("if (auto* arm = xbox360ps5::frame_cost::current_arm) arm->Protected();  // fix 74", at)) !=
         std::string::npos) {
    const size_t protect = code.find("xe::memory::Protect(", at);
    if (protect != std::string::npos && protect - at < 200) ++notes;
    ++at;
  }
  Expect(body != std::string::npos && notes == 2, "both protection calls of the inner loop are noted before they are made");
  const size_t trigger = code.find("bool PhysicalHeap::TriggerCallbacks(");
  const size_t notice = code.find("if (is_write) xbox360ps5::frame_cost::NoteWriteNotice(heap_base_);  // fix 74", trigger);
  Expect(trigger != std::string::npos && notice != std::string::npos && notice - trigger < 400,
         "every write notice is counted per view");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t draws = main_code.find("XELOGW(\"{}\", frame_cost_draws.Line(double(summary_samples)));");
  const size_t arming = main_code.find("XELOGW(\"{}\", frame_cost_arming.Line(double(summary_samples)));");
  Expect(draws != std::string::npos && arming != std::string::npos && draws < arming, "the summary gives the line after fix 73's");
}

}  // namespace

int main() {
  CheckTimer();
  CheckTree();
  if (failures) {
    std::printf("check-watch-arming: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-watch-arming: ok\n");
  return 0;
}
