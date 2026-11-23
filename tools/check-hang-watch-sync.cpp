// SPDX-License-Identifier: MIT
// Fix 153 (tools/source-1.0.2/fixes/153-hang-watch-no-beat-fsync.patch): the hang watch without an
// fsync per beat. On the console (GTA IV, 1.0.6-work-fix146f and 142e) the watch's beats fell
// from 110 to 7 in 30 s with gaps up to 1.93 s, and the game's slow file operations on the console
// drive all topped out at 0.94-1.0 s; the watch synced its file 8 times a second.
// Checked:
//  (a) hang-watch-sync.txt's words: "beats" (also "every", "all", "sync"; any case, spaces and a
//      line break around) syncs every line, anything else, empty or missing, only reports;
//  (b) the lines the watch writes about it: the policy (and whether the file was there) and the
//      10 s fsync gauge;
//  (c) RawLight goes to the light sink; without one (the watch not running) to the Raw sink;
//  (d) the assembled tree: the light writer writes once and syncs only under "beats", the raw and
//      report writers still sync, the beat's two lines go in one light write, the switch is read
//      every 2 s from the app folder, the gauge every 10 s, the light sink is set with the raw
//      one; the guest threads' DbgPrint, protect and stack-switch lines are light, and so is
//      BULLYTRACE open-failed (StageLight, no fsync); the title-end line stays synced.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/hang_watch.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace hw = xbox360ps5::hang_watch;

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
  std::ostringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}

bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return std::string();
  return text.substr(at, text.find("\n}", at) - at);
}

std::string light_got, raw_got;
void LightSink(const char* text, size_t size) { light_got.assign(text, size); }
void RawSinkFake(const char* text, size_t size) { raw_got.assign(text, size); }

}  // namespace

int main() {
  // (a)
  Expect(hw::ParseSyncSwitch("beats") == hw::kSyncEveryLine, "(a) beats");
  Expect(hw::ParseSyncSwitch("  Beats\r\n") == hw::kSyncEveryLine, "(a) Beats with spaces and CRLF");
  Expect(hw::ParseSyncSwitch("every\n") == hw::kSyncEveryLine && hw::ParseSyncSwitch("ALL") == hw::kSyncEveryLine &&
             hw::ParseSyncSwitch("sync") == hw::kSyncEveryLine,
         "(a) every, all, sync");
  Expect(hw::ParseSyncSwitch("reports") == hw::kSyncReports && hw::ParseSyncSwitch("") == hw::kSyncReports &&
             hw::ParseSyncSwitch("beat") == hw::kSyncReports && hw::ParseSyncSwitch("x beats") == hw::kSyncReports,
         "(a) reports, empty, a near miss and a later word are reports only");
  Expect(hw::sync_policy.load() == hw::kSyncReports, "(a) reports only before the switch is read");

  // (b)
  Expect(hw::SyncPolicyLine(hw::kSyncEveryLine, true) ==
             "HANG watch sync (fix 153): every line synced (hang-watch-sync.txt says beats)",
         "(b) every line: " + hw::SyncPolicyLine(hw::kSyncEveryLine, true));
  Expect(hw::SyncPolicyLine(hw::kSyncReports, false) ==
             "HANG watch sync (fix 153): only reports synced (no hang-watch-sync.txt: the default)",
         "(b) the default");
  Expect(hw::SyncPolicyLine(hw::kSyncReports, true) ==
             "HANG watch sync (fix 153): only reports synced (hang-watch-sync.txt says reports)",
         "(b) reports from the file");
  Expect(hw::FsyncGaugeLine(70.25, 12.34, hw::kSyncReports) ==
             "HANG fsync gauge +70.25 s (fix 153): 12.3 ms for one fsync of this file (only reports synced)",
         "(b) the gauge: " + hw::FsyncGaugeLine(70.25, 12.34, hw::kSyncReports));

  // (c)
  hw::raw_sink.store(&RawSinkFake);
  hw::RawLight("a guest line");
  Expect(raw_got == "a guest line" && light_got.empty(), "(c) without a light sink, a Raw line");
  raw_got.clear();
  hw::raw_light_sink.store(&LightSink);
  hw::RawLight("a beat");
  Expect(light_got == "a beat" && raw_got.empty(), "(c) with one, the light sink");
  hw::Raw("a report");
  Expect(raw_got == "a report", "(c) Raw stays the synced sink");

  // (d)
  const std::string main_text = Read("src/canary_game_main.cpp");
  const std::string light = Body(main_text, "void HangWatchRawLight(const char* text, size_t size) {");
  Expect(Has(light, "(void)!::write(fd, line.data(), line.size());") &&
             Has(light, "if (xbox360ps5::hang_watch::sync_policy.load(std::memory_order_relaxed) ==\n"
                        "        xbox360ps5::hang_watch::kSyncEveryLine)\n      ::fsync(fd);") &&
             !Has(light, "lock") && !Has(light, "XELOG"),
         "(d) the light writer writes once and syncs only under \"beats\"");
  const std::string raw = Body(main_text, "void HangWatchRaw(const char* text, size_t size) {");
  const std::string batch = Body(main_text, "void HangWatchRawBatch(const std::vector<std::string>& lines) {");
  Expect(Has(raw, "::fsync(fd);") && Has(batch, "::fsync(fd);"), "(d) raw lines and reports still sync");
  Expect(Has(main_text, "EmitLight(hw::HeartbeatLine(in) + \"\\n[X360] \" +") &&
             !Has(main_text, "Emit(hw::HeartbeatLine(in));"),
         "(d) the beat's two lines in one light write");
  Expect(Has(main_text, "XBOX360PS5_DATA_ROOT \"/hang-watch-sync.txt\"") && Has(main_text, "next_switch = now + 2000;") &&
             Has(main_text, "ReadSyncSwitch(false);") && Has(main_text, "ReadSyncSwitch(true);"),
         "(d) hang-watch-sync.txt read at the start and every 2 s");
  Expect(Has(main_text, "next_gauge = now + 10000;") && Has(main_text, "hw::FsyncGaugeLine("),
         "(d) the fsync gauge every 10 s");
  Expect(Has(main_text, "raw_light_sink.store(&HangWatchRawLight, std::memory_order_release);"),
         "(d) the light sink is set with the raw one");
  const std::string kernel = ".deps/xenia-canary/src/xenia/kernel/";
  Expect(Has(Read(kernel + "xboxkrnl/xboxkrnl_strings.cc"), "xbox360ps5::hang_watch::RawLight(text);"),
         "(d) DbgPrint lines are light");
  Expect(Has(Read(kernel + "xboxkrnl/xboxkrnl_memory.cc"), "xbox360ps5::hang_watch::RawLight(xbox360ps5::hang_watch::ProtectLine("),
         "(d) protect lines are light");
  Expect(Has(Read(kernel + "xboxkrnl/xboxkrnl_threading.cc"),
             "xbox360ps5::hang_watch::RawLight(xbox360ps5::hang_watch::StackSwitchLine("),
         "(d) stack-switch lines are light");
  const std::string io = Read(kernel + "xboxkrnl/xboxkrnl_io.cc");
  const size_t open_failed = io.find("BULLYTRACE open-failed status");
  Expect(open_failed != std::string::npos && io.find("xbox360ps5::StageLight(text);", open_failed) - open_failed < 400,
         "(d) BULLYTRACE open-failed is a light Stage line");
  const std::string stage_light = Body(Read("platform/ps5/crash_report.cpp"), "void StageLight(const char* text) {");
  Expect(!stage_light.empty() && !Has(stage_light, "fsync") && Has(stage_light, "write(report_file, buffer, size)") &&
             Has(stage_light, "write(mirror, buffer, size)"),
         "(d) StageLight writes the report file and its mirror without an fsync");
  Expect(Has(Read(kernel + "xam/xam_info.cc"), "  xbox360ps5::hang_watch::Raw(text);\n}"),
         "(d) the title-end line stays synced");

  if (failures) {
    std::printf("check-hang-watch-sync: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-hang-watch-sync: ok\n");
  return 0;
}
