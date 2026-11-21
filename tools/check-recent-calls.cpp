// SPDX-License-Identifier: MIT
// Fix 93e (tools/source-1.0.2/fixes/93e-recent-calls-file.patch): leaving a game with logs for a
// bug report no longer hangs. GTA IV (545407F2) on 1.0.5-work-fix93d stood on "Returning to the
// menu..." with its dots still and its music playing: the game's last system calls (about a
// megabyte kept in memory, 4.8 million calls a second) went through the log a line each on the UI
// thread as the game loop ended. They are now taken in one copy (TakeRecentLines, which also stops
// keeping them), written to a file of their own (recent-calls-<title>-<UTC>.txt) in one write on a
// thread of their own, and waited for at most 3 s while the notice is drawn; the game's sound is
// muted with the notice.
//  (a) bug_report.hpp: RecentCallsName; StartWriting writes the text whole, says when it is done
//      and whether it worked, and survives the caller letting go of it; Files() takes the file
//      when it was written in the session;
//  (b) leaving_notice.hpp: PaintUntil paints until the work is done, or stops at its limit;
//  (c) the way out with a megabyte of system calls like GTA IV's (XamInputGetKeystrokeEx lines):
//      written and waited for in well under the limit, and a writing that never ends costs the
//      limit only;
//  (d) the assembled tree: TakeRecentLines in the core (one copy under the lock, no more kept, a cut
//      first line dropped); the way out mutes the game, takes the calls and starts the file before
//      its first wait, waits at most 3 s before the cache checkpoint, and no longer sends them
//      through the log.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/bug_report.hpp"
#include "xbox360ps5/leaving_notice.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace {
namespace fs = std::filesystem;
namespace br = xbox360ps5::bug_report;
namespace notice = xbox360ps5::leaving_notice;
using Clock = std::chrono::steady_clock;

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
double Seconds(Clock::time_point since) { return std::chrono::duration<double>(Clock::now() - since).count(); }
// A megabyte of what GTA IV's last system calls looked like.
std::string GtaCalls() {
  std::string text;
  while (text.size() < (1u << 20)) text += "d> F8000008 XamInputGetKeystrokeEx(7018FA24(00000000), 40000002, 7018FA60)\n";
  return text;
}

void CheckWriting(const fs::path& folder) {
  Expect(br::RecentCallsName(0x545407F2, 1795291872) == "recent-calls-545407F2-20261121-201112-UTC.txt",
         "(a) RecentCallsName: " + br::RecentCallsName(0x545407F2, 1795291872));
  const auto text = std::make_shared<const std::string>(GtaCalls());
  const fs::path file = folder / br::RecentCallsName(0x545407F2, int64_t(std::time(nullptr)));
  // (c) the way out: start the file, then paint until it is written.
  const auto start = Clock::now();
  auto writing = br::StartWriting(file, text);
  int frames = 0;
  const bool done = notice::PaintUntil(writing->done, std::chrono::seconds(3), [&] { ++frames; });
  const double took = Seconds(start);
  Expect(done && writing->ok.load(), "(a) a megabyte of system calls is written");
  Expect(took < 1.0, "(c) and waited for in well under the 3 s limit: " + std::to_string(took) + " s");
  Expect(Read(file) == *text, "(a) whole, in one file");
  // A file that cannot be made: done, not written, no wait beyond it.
  auto failed = br::StartWriting(folder / "no-such-folder" / "recent-calls-x.txt", text);
  Expect(notice::PaintUntil(failed->done, std::chrono::seconds(3), [] {}) && !failed->ok.load(),
         "(a) a file that cannot be made: done, not written");
  // The caller may let go before the thread ends (the title restarts).
  {
    auto dropped = br::StartWriting(folder / "recent-calls-dropped.txt", text);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  Expect(Read(folder / "recent-calls-dropped.txt") == *text, "(a) the writing goes on after the caller let go of it");
  // (a) Files() takes the session's file, not an older one.
  br::State state = br::Begin("GTA IV", "545407F2", int64_t(std::time(nullptr)) - 60);
  const fs::path log = folder / "Game-GTA IV-5E824984-20261121-201112-UTC-0.log";
  { std::ofstream(log) << "log\n"; }
  state.logs.push_back(log.string());
  const fs::path old = folder / "recent-calls-545407F2-20201121-101112-UTC.txt";
  { std::ofstream(old) << "old\n"; }
  fs::last_write_time(old, fs::last_write_time(old) - std::chrono::hours(5));
  bool has_new = false, has_old = false;
  for (const auto& entry : br::Files(state, {})) {
    has_new = has_new || entry.path.filename() == file.filename();
    has_old = has_old || entry.path.filename() == old.filename();
  }
  Expect(has_new && !has_old, "(a) the report takes the session's file of system calls only");
}

void CheckPaintUntil() {
  std::atomic<bool> done{false};
  int frames = 0;
  const auto start = Clock::now();
  std::thread worker([&done] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    done.store(true);
  });
  Expect(notice::PaintUntil(done, std::chrono::seconds(3), [&] { ++frames; }), "(b) PaintUntil: done");
  worker.join();
  Expect(Seconds(start) < 1.0 && frames >= 5, "(b) frames painted meanwhile: " + std::to_string(frames));
  std::atomic<bool> never{false};
  const auto capped = Clock::now();
  int capped_frames = 0;
  Expect(!notice::PaintUntil(never, std::chrono::milliseconds(400), [&] { ++capped_frames; }), "(c) a wait that never ends stops");
  const double took = Seconds(capped);
  Expect(took >= 0.39 && took < 0.8, "(c) at its limit: " + std::to_string(took) + " s");
  Expect(capped_frames >= 10, "(c) painting all the while: " + std::to_string(capped_frames));
  std::atomic<bool> already{true};
  int none = 0;
  Expect(notice::PaintUntil(already, std::chrono::seconds(3), [&] { ++none; }) && none == 0, "(b) nothing to wait for: no frame");
}

void CheckTree() {
  const std::string header = Read(".deps/xenia-canary/src/xenia/base/logging.h");
  const std::string core = Read(".deps/xenia-canary/src/xenia/base/logging.cc");
  // Fix 93i adds `keep_on` (a STALL report keeps them on); the way out's call still stops them.
  Expect(Has(header, "std::string TakeRecentLines();") || Has(header, "std::string TakeRecentLines(bool keep_on = false);"),
         "(d) the core declares TakeRecentLines");
  const std::string take = Between(core, "std::string logging::TakeRecentLines(", "\n}\n");
  Expect(Has(take, "std::lock_guard<std::mutex> lock(recent_mutex_);") && (Has(take, "keep_recent_lines_ = false;") || Has(take, "keep_recent_lines_ = keep_on;")) &&
             Has(take, "xe::threading::SuspensionDeferral deferral;") && Has(take, "if (cut) {") &&
             !Has(take, "logger_"),
         "(d) one copy under the lock, no more kept, a cut first line dropped, nothing through the log");
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string way_out = Between(main, "hang_watch_thread.reset();  // fix 47", "if (!restart && emulator.is_title_open())");
  Expect(!way_out.empty() && !Has(way_out, "WriteRecentLines()"), "(d) the way out no longer sends them through the log");
  const size_t mute = way_out.find("quiet.mute = 1;");
  const size_t taken = way_out.find("xe::logging::TakeRecentLines()");
  const size_t started = way_out.find("recent_calls = xbox360ps5::bug_report::StartWriting(file, calls);");
  const size_t first_wait = way_out.find("xe::ui::controller_keyboard::Cancel();");
  const size_t waited = way_out.find("leaving::PaintUntil(recent_calls->done, std::chrono::seconds(3), paint_notice);");
  const size_t checkpoint = way_out.find("CheckpointExitCache(exit_cache)");
  Expect(mute != std::string::npos && taken != std::string::npos && mute < taken, "(d) the game is muted first");
  Expect(started != std::string::npos && started < first_wait, "(d) the file is started before the first wait");
  Expect(waited != std::string::npos && waited < checkpoint, "(d) waited for (3 s at most) before the cache checkpoint");
  Expect(Has(way_out, "RecentCallsName(emulator.title_id(), int64_t(std::time(nullptr)))") &&
             Has(way_out, "game_log->session.Path().parent_path() /"),
         "(d) beside the game log, named by the title");
}
}  // namespace

int main() {
  const fs::path folder = fs::temp_directory_path() /
      ("check-recent-calls-" + std::to_string(Clock::now().time_since_epoch().count()));
  fs::create_directories(folder);
  CheckWriting(folder);
  CheckPaintUntil();
  CheckTree();
  std::error_code error;
  fs::remove_all(folder, error);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-recent-calls: all checks passed\n");
  return 0;
}
