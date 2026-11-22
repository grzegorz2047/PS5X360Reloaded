// SPDX-License-Identifier: MIT
// Fix 76 (tools/source-1.0.2/fixes/76-alloc-failure-lines.patch): on the v1.0.4 draft Fable II
// (4D5307F1) stopped at start. Its game thread asks NtAllocateVirtualMemory for a 5 MiB reserve
// of 64 KiB pages; when that fails it asks again at every 64 KiB from 0x40000000 until one is
// free (about 3,900 tries with 245 MiB of the heap taken). Fix 47 wrote a hang-watch line, a
// write and an fsync, for every failed allocation: 3,856 lines in 61 s, and the game never got
// past it. Now only the first kFailureLines failures get a line, then one says the rest are only
// counted; a big allocation still gets its line, and the beat still has every failure.
// At 16 s the frame watch's capture also logged every system call: Fable II's idle threads asked
// for the time about 20,000 times a second, 33,000 lines in under 2 s, and the game's own lines
// waited over a minute for the log writer. The capture now takes at most kCaptureDebugLines.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/hang_watch.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
namespace hw = xbox360ps5::hang_watch;
std::vector<std::string> raw;

void CheckLines() {
  hw::raw_sink.store(+[](const char* text, size_t size) { raw.emplace_back(text, size); });
  // Fable II's search: the reserve anywhere, then at every 64 KiB from 0x40000000.
  hw::GuestAllocation("NtAllocateVirtualMemory", false, 0, 0x540000, 0x20002000, false);
  for (uint32_t address = 0x40000000; address < 0x4F500000; address += 0x10000)
    hw::GuestAllocation("NtAllocateVirtualMemory", false, address, 0x540000, 0x20002000, false);
  const uint64_t tries = 1 + (0x4F500000 - 0x40000000) / 0x10000;
  Expect(hw::guest_memory.failures == tries, "every failure counted (" + std::to_string(hw::guest_memory.failures) + ")");
  Expect(raw.size() == hw::kFailureLines + 1, "a line for the first " + std::to_string(hw::kFailureLines) +
                                                   " failures and one more, not one each (" + std::to_string(raw.size()) + ")");
  Expect(!raw.empty() && raw.front().find("GUEST NtAllocateVirtualMemory FAILED: 00000000, 5505024 bytes (5 MiB), type 20002000") == 0,
         "the first failure's line");
  Expect(raw.size() > hw::kFailureLines &&
             raw[hw::kFailureLines] == "GUEST failed allocations: 16 lines written, the rest only counted in the beat",
         "the line that says no more follow");
  // The reserve found room: a small success, no line.
  Expect(hw::GuestAllocation("NtAllocateVirtualMemory", false, 0x4F500000, 0x540000, 0x20002000, true).empty(),
         "a small allocation: no line");
  const size_t before = raw.size();
  hw::GuestAllocation("MmAllocatePhysicalMemoryEx", true, 0xA0000000, 96u << 20, 0x20000000, true);
  Expect(raw.size() == before + 1, "a big allocation still gets its line");
  hw::BeatInput in;
  Expect(hw::HeartbeatLine(in).find(", " + std::to_string(tries) + " failed") != std::string::npos,
         "the beat has every failure");
  hw::raw_sink.store(nullptr);
}

void CheckCaptureLines() {
  raw.clear();
  hw::raw_sink.store(+[](const char* text, size_t size) { raw.emplace_back(text, size); });
  Expect(hw::capture_debug_lines == -1 && hw::CaptureDebugLeft(), "no capture: debug lines by the log level alone");
  for (int n = 0; n < 5000; ++n) Expect(hw::TakeCaptureDebugLine(), "no capture: every debug line");
  Expect(raw.empty(), "no capture: no line about it");
  hw::capture_debug_lines.store(hw::kCaptureDebugLines);
  int taken = 0;
  for (int n = 0; n < 20000; ++n) taken += hw::TakeCaptureDebugLine() ? 1 : 0;
  Expect(taken == hw::kCaptureDebugLines, "a capture: " + std::to_string(hw::kCaptureDebugLines) + " debug lines (" +
                                              std::to_string(taken) + ")");
  Expect(!hw::CaptureDebugLeft(), "then none, already before a line is formatted");
  Expect(raw.size() == 1 && raw[0] == "STALL capture: 1000 system-call lines logged, no more until the next capture",
         "one line says the capture's lines are spent");
  hw::capture_debug_lines.store(-1);
  Expect(hw::CaptureDebugLeft() && hw::TakeCaptureDebugLine(), "after the capture: by the log level again");
  hw::raw_sink.store(nullptr);
}

void CheckTree() {
  const std::string code = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc");
  Expect(code.find("if (!ok || (alloc_type & X_MEM_COMMIT)) {\n    xbox360ps5::hang_watch::GuestAllocation(") != std::string::npos ||
             code.find("if (!ok || (alloc_type & X_MEM_COMMIT)) {\r\n    xbox360ps5::hang_watch::GuestAllocation(") != std::string::npos,
         "NtAllocateVirtualMemory still hands every failure to GuestAllocation");
  const std::string log = Read(".deps/xenia-canary/src/xenia/base/logging.cc");
  const size_t goes = log.find("bool GoesToLog(LogLevel log_level, uint32_t log_mask) {");
  const size_t left = log.find("!xbox360ps5::hang_watch::CaptureDebugLeft()", goes);
  Expect(goes != std::string::npos && left != std::string::npos && left - goes < 300,
         "GoesToLog drops debug lines once the capture's are spent");
  size_t at = 0, takes = 0;
  while ((at = log.find("!xbox360ps5::hang_watch::TakeCaptureDebugLine()", at)) != std::string::npos) {
    const size_t append = log.find("logger_->AppendLine(", at);
    if (append != std::string::npos && append - at < 200) ++takes;
    ++at;
  }
  Expect(takes == 2, "both AppendLogLine take a capture line right before appending");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t store = main_code.find("xbox360ps5::hang_watch::capture_debug_lines.store(xbox360ps5::hang_watch::kCaptureDebugLines);");
  const size_t level = main_code.find("cvars::log_level = 3;  // Every system call, for a moment.", store);
  const size_t finish = main_code.find("case xbox360ps5::FrameWatch::finish_capture:", level);
  const size_t reset = main_code.find("xbox360ps5::hang_watch::capture_debug_lines.store(-1);  // fix 76", finish);
  const size_t over = main_code.find("XELOGW(\"STALL over: frames again\");", finish);
  const size_t reset2 = main_code.find("xbox360ps5::hang_watch::capture_debug_lines.store(-1);  // fix 76", over);
  Expect(store != std::string::npos && level != std::string::npos && level - store < 200 && reset != std::string::npos &&
             over != std::string::npos && reset < over && reset2 != std::string::npos,
         "the capture sets the budget with the log level; its end and recovery clear it");
}

}  // namespace

int main() {
  CheckLines();
  CheckCaptureLines();
  CheckTree();
  if (failures) {
    std::printf("check-alloc-failure-lines: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-alloc-failure-lines: ok\n");
  return 0;
}
