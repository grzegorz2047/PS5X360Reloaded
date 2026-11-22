// SPDX-License-Identifier: MIT
// Fix 83 (tools/source-1.0.2/fixes/83-wait-reg-wakeup.patch): WAIT_REG_MEM on a guest memory word
// with a wait field of 0x100 or more slept wait / 0x100 ms after each miss, so it ended up to a
// whole sleep after the guest wrote the word (1.0.5_candidate_57, Battlefield 3: 29 such waits a
// second, about 1.1 ms per sleep, 320 ms/s asleep, the GPU command thread the limit). On the
// console it now sleeps the same time in slices of 100 us and reads the word again after each.
// Checked in the assembled tree (the GPU code is not built on the host), and the slicing's
// timing modelled here:
//  (a) only memory waits are sliced, a register wait sleeps as before, other platforms as
//      before; the slices stop at the same bound, or when the worker stops;
//  (b) the word is read with the packet's endian swap and compared with its mask, reference and
//      function, as the poll above does; fix 77's timer still times the whole sleep.
#include <chrono>
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

// How long after the guest's write a wait ends: the old way rounds up to the next whole sleep,
// the sliced way to the next 100 us slice (times in microseconds).
long Late(long written_at, long sleep, bool sliced) {
  const long step = sliced ? 100 : sleep;
  const long woken = ((written_at + step - 1) / step) * step;
  return woken - written_at;
}

void CheckModel() {
  long old_total = 0, new_total = 0;
  for (long at = 1; at <= 10000; at += 7) {  // a write anywhere in a 10 ms wait
    old_total += Late(at, 1000, false);
    new_total += Late(at, 1000, true);
  }
  const double old_mean = double(old_total) / 1429, new_mean = double(new_total) / 1429;
  std::printf("   mean lateness after the write: %.0f us before, %.0f us sliced\n", old_mean, new_mean);
  Expect(old_mean > 400 && new_mean < 60, "a wait ends about 0.5 ms sooner on average");
}

void CheckTree() {
  const std::string pm4 = Read(".deps/xenia-canary/src/xenia/gpu/pm4_command_processor_implement.h");
  const size_t fn = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_WAIT_REG_MEM(");
  const size_t before = pm4.find("const uint64_t frame_cost_sleep = frame_cost_wait.BeforeSleep();  // fix 77", fn);
  const size_t ps5 = pm4.find("#if XE_PLATFORM_PS5\n          // Fix 83:", before);
  const size_t memory = pm4.find("          if (is_memory) {", ps5);
  const size_t until = pm4.find("std::chrono::milliseconds(wait / 0x100);", memory);
  const size_t slice = pm4.find("xe::threading::Sleep(std::chrono::microseconds(100));", memory);
  const size_t swap = pm4.find("static_cast<xenos::Endian>(poll_reg_addr & 0x3));", slice);
  const size_t match = pm4.find("if (MatchValueAndRef(now_value & mask, ref, wait_info)) {", swap);
  const size_t bound = pm4.find("} while (worker_running_ &&\n                     std::chrono::steady_clock::now() < until);", match);
  const size_t reg = pm4.find("          } else {\n            xe::threading::Sleep(std::chrono::milliseconds(wait / 0x100));\n          }\n#else\n"
                              "          xe::threading::Sleep(std::chrono::milliseconds(wait / 0x100));\n#endif", bound);
  const size_t after = pm4.find("frame_cost_wait.AfterSleep(frame_cost_sleep);", reg);
  const size_t next = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_REG_RMW(", fn);
  Expect(fn != std::string::npos && before != std::string::npos && ps5 != std::string::npos && memory != std::string::npos &&
             until < slice && slice != std::string::npos && swap != std::string::npos && match != std::string::npos &&
             bound != std::string::npos && reg != std::string::npos && after != std::string::npos && after < next,
         "a memory wait sleeps in 100 us slices up to the same bound, re-reading the word; a register wait and other "
         "platforms as before; fix 77's timer around it");
  const size_t poll = pm4.find("matched = MatchValueAndRef(value & mask, ref, wait_info);", fn);
  const size_t poll_swap = pm4.rfind("static_cast<xenos::Endian>(poll_reg_addr & 0x3));", poll);
  Expect(poll != std::string::npos && poll_swap != std::string::npos && poll_swap > fn && poll < match,
         "the slice reads the word as the poll above does");
}

}  // namespace

int main() {
  CheckModel();
  CheckTree();
  if (failures) {
    std::printf("check-wait-reg-wakeup: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-wait-reg-wakeup: ok\n");
  return 0;
}
