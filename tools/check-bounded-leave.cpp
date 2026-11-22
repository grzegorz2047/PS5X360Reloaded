// SPDX-License-Identifier: MIT
// Fix 93i (tools/source-1.0.2/fixes/93i-bounded-leave.patch): leaving GTA IV with logs for a bug
// report hung on "Returning to the menu..." for minutes on 1.0.5-work-fix93g2
// (research/fix93g2-gta4/). The cache checkpoint and the flush had ended ("BOOT restarting the
// title" was written); the restart then waited in xe::ShutdownLogging for the log's writer, about
// 14,000 lines behind (the hang watch's "log 18239/3983"): a STALL report 88 s earlier had put the
// game's last system calls (1 MiB) into the log a line each, and the drive was busy with GTA IV
// copying 40 MB of .img data to its cache, so the writer managed about 100 lines a second.
//  (a) the core: TakeRecentLines(keep_on): the copy under the lock, and keeping on only when asked;
//  (b) the STALL and WAITING reports, with logs for a bug report, put the calls in a file of their
//      own (SaveStallCalls: TakeRecentLines(true), StartWriting) instead of the log; without the
//      option, as before;
//  (c) the restart gives the log's writer 3 s on a thread of its own, then restarts anyway (no
//      unbounded ShutdownLogging before LoadExec);
//  (d) the flush (fix 93g) runs on a thread of its own and is waited for 3 s at most; its line goes
//      to the boot log too (written at once: the game log's writer may be behind);
//  (e) one STALL calls file a session (each is a megabyte, written while the game may fill the drive).
// The guest's threads are not suspended before the flush: the restart destroys the emulator with
// its title open (no TerminateTitle), and suspended guest threads could hold that teardown.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string s = text.str(), out;
  for (char c : s) if (c != '\r') out += c;
  return out;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}
size_t Count(const std::string& text, const std::string& part) {
  size_t count = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++count;
  return count;
}
bool InOrder(const std::string& text, const std::vector<std::string>& parts) {
  size_t at = 0;
  for (const auto& part : parts) {
    at = text.find(part, at);
    if (at == std::string::npos) return false;
    at += part.size();
  }
  return true;
}
}  // namespace

int main() {
  const std::string header = Read(".deps/xenia-canary/src/xenia/base/logging.h");
  const std::string core = Read(".deps/xenia-canary/src/xenia/base/logging.cc");
  const std::string main = Read("src/canary_game_main.cpp");
  // (a)
  Expect(Has(header, "std::string TakeRecentLines(bool keep_on = false);"), "(a) TakeRecentLines(keep_on), off by default");
  const std::string take = Between(core, "std::string logging::TakeRecentLines(bool keep_on) {", "\n}\n");
  Expect(InOrder(take, {"std::lock_guard<std::mutex> lock(recent_mutex_);", "keep_recent_lines_ = keep_on;"}),
         "(a) kept on only when asked");
  // (b)
  const std::string save = Between(main, "void SaveStallCalls(const std::filesystem::path& folder, uint32_t title_id) {", "\n}");
  Expect(Has(save, "xe::logging::TakeRecentLines(true)") && Has(save, "xbox360ps5::bug_report::StartWriting(file, calls);") &&
             Has(save, "xbox360ps5::bug_report::RecentCallsName(title_id"),
         "(b) SaveStallCalls: the calls to a file of their own, kept on");
  Expect(Count(main, "SaveStallCalls(game_log->session.Path().parent_path(), emulator.title_id());  // fix 93i") == 2,
         "(b) both reports (STALL and WAITING) use it");
  Expect(Count(main, "if (xbox360ps5::bug_report::Active()) {\n                  SaveStallCalls(") +
                 Count(main, "if (xbox360ps5::bug_report::Active()) {\n                    SaveStallCalls(") == 2 &&
             Count(main, "xe::logging::WriteRecentLines();") == 2,
         "(b) with the option only; without it, through the log as before");
  // (c)
  const std::string restart = Between(main, "Stage(\"BOOT restarting the title\");", "sceSystemServiceLoadExec(\"/app0/eboot.bin\", nullptr);");
  Expect(InOrder(restart, {"std::thread([stopped] {", "xe::ShutdownLogging();", "}).detach();",
                           "std::chrono::seconds(3)", "while (!stopped->load(std::memory_order_acquire)"}),
         "(c) the log's writer gets 3 s on a thread of its own");
  Expect(Count(restart, "xe::ShutdownLogging();") == 1, "(c) no unbounded ShutdownLogging before the restart");
  // (d)
  const std::string flush = Between(main, "void FlushWritesBeforeRestart(const char* why) {", "\n}");
  Expect(InOrder(flush, {"std::thread([done] {", "::sync();", "}).detach();", "std::chrono::seconds(3)"}),
         "(d) the flush on a thread of its own, 3 s at most");
  Expect(Has(flush, "\"still being flushed; going on\""), "(d) and the log says when it went on without it");
  Expect(InOrder(flush, {"std::snprintf(line, sizeof(line), \"Leaving: pending disk writes %s in %lld ms (%s)\"",
                         "XELOGW(\"{}\", line);", "xbox360ps5::StageBlock(lines, 1, false);"}),
         "(d) the line in the game log and in the boot log");
  // (e)
  Expect(InOrder(save, {"static bool saved = false;", "if (saved) {", "return;", "saved = true;",
                        "xe::logging::TakeRecentLines(true)"}),
         "(e) one STALL calls file a session");
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-bounded-leave: all checks passed\n");
  return 0;
}
