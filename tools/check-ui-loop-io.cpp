// SPDX-License-Identifier: MIT
// Fix 152 (tools/source-1.0.2/fixes/152-ui-loop-no-log-flush.patch): the game loop's every-second
// work makes no file-system call on the UI thread.
//
// On the console (1.0.6-work-fix146f, GTA IV at 60 Hz) the screen stood still 0.13-0.78 s about
// once in 30 s, more often as the session went on, and fix 146f's present-stall lines put all of it
// in the loop's "every second" part, right after the 30 s summary. There, with video-memory
// optimizations on (GTA IV's own preset sets them, and they turn gpu_diag on), the loop called
// xe::FlushLog(): a 10 ms sleep and a flush of the log file on the UI thread, behind the log
// writer's own writes to the console drive, which the game's slow deletes and renames kept busy.
// Checked, on the assembled tree:
//  (a) the every-second block of the game loop has no FlushLog() and no other file-system call
//      (std::ofstream, std::filesystem, fopen, fflush, fsync, rename, remove);
//  (b) in the whole game loop, FlushLog() is only on the paths that restart the process
//      (followed by sceSystemServiceLoadExec);
//  (c) the lines still reach the file: the core logger's writer thread flushes after each batch
//      (flush_log, on by default) and the game does not turn flush_log off.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
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
  std::ostringstream text;
  text << file.rdbuf();
  std::string data = text.str();
  // CRLF files read as LF.
  std::string out;
  out.reserve(data.size());
  for (char c : data)
    if (c != '\r') out += c;
  return out;
}

bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
  return n;
}

}  // namespace

int main() {
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(!main_text.empty(), "src/canary_game_main.cpp in the tree");
  const size_t start = main_text.find("auto hang_watch_thread = std::make_unique<HangWatchThread>(");
  const size_t loop = main_text.find("while (!context.HasQuitFromUIThread()) {", start);
  const size_t end = main_text.find("hang_watch_thread.reset();", loop);
  Expect(start != std::string::npos && loop != std::string::npos && end != std::string::npos && loop < end,
         "the game loop is found");
  const std::string game_loop = main_text.substr(loop, end - loop);

  // (a)
  const size_t second = game_loop.find("if (std::chrono::steady_clock::now() - sample_at >= std::chrono::seconds(1)) {");
  Expect(second != std::string::npos, "(a) the every-second block is found");
  const std::string every_second = second == std::string::npos ? std::string() : game_loop.substr(second);
  Expect(!Has(every_second, "FlushLog();"), "(a) the every-second block does not flush the log on the UI thread");
  for (const char* io : {"std::ofstream", "std::filesystem::", "fopen(", "fflush(", "fsync(", "std::rename(",
                         "std::remove("})
    Expect(!Has(every_second, io), std::string("(a) the every-second block makes no file-system call: ") + io);
  Expect(Has(every_second, "Fix 152: no xe::FlushLog() here."), "(a) the reason stays next to the summary");

  // (b)
  size_t flushes = 0, on_restart = 0;
  for (size_t at = game_loop.find("FlushLog();"); at != std::string::npos;
       at = game_loop.find("FlushLog();", at + 1)) {
    ++flushes;
    const size_t exec = game_loop.find("sceSystemServiceLoadExec(", at);
    if (exec != std::string::npos && exec - at < 400) ++on_restart;
  }
  Expect(flushes == on_restart,
         "(b) FlushLog() in the game loop only before a restart: " + std::to_string(flushes) + " calls, " +
             std::to_string(on_restart) + " before sceSystemServiceLoadExec");

  // (c)
  const std::string logging = Read(".deps/xenia-canary/src/xenia/base/logging.cc");
  Expect(Has(logging, "DEFINE_bool(flush_log, true,"), "(c) flush_log is on by default");
  const size_t writer = logging.find("void WriteThread() {");
  const size_t flush = logging.find("if (cvars::flush_log) {", writer);
  Expect(writer != std::string::npos && flush != std::string::npos &&
             logging.find("FlushAllSinks();", flush) - flush < 80,
         "(c) the log writer thread flushes after each batch");
  Expect(!Has(main_text, "cvars::flush_log = false"), "(c) the game keeps flush_log on");
  Expect(Count(main_text, "xe::FlushLog();") >= 1, "(c) FlushLog() is still used on the exit and restart paths");

  if (failures) {
    std::printf("check-ui-loop-io: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-ui-loop-io: ok\n");
  return 0;
}
