// SPDX-License-Identifier: MIT
// Fix 146 (tools/source-1.0.2/fixes/146-present-limit.patch, GFX-02A): a limit on how often the
// game's new pictures are shown, on the presentation side only (research/fps-limit/DESIGN.md).
// Checked:
//  (a) the plan of every choice on the 59.94 and 119.88 Hz outputs: 30 holds a picture 2 or 4
//      refreshes, 40 3 refreshes at 120 Hz and is not applied at 60 Hz, 60 1 or 2 refreshes;
//      Automatic and No limit hold nothing; frame generation and an unknown output turn it off;
//  (b) a model of the mailbox (the game publishes pictures, the paint takes the newest) on a FIFO
//      output whose paints wake with jitter: a game at 45 to 55 FPS capped at 40 on 120 Hz shows
//      a new picture every 3 refreshes (median and p95 25 ms) at 40 a second, and the game's own
//      pictures come exactly as without the limit (the producer never waits);
//  (c) a game slower than the cap (35 FPS under 40) is shown as without the limit: the limit
//      never raises a frame rate nor holds a picture that came late;
//  (d) Dragon Age II's case: 60 FPS (the 120 Hz emulated video rate) capped at 30 on a 60 Hz
//      output shows 30 new pictures a second, 2 refreshes each, and Automatic shows them all;
//  (e) 40 on the 60 Hz output and any limit with frame generation on show every picture;
//  (f) a change during the session (40, Automatic, 30), a new output and a pause: a log line for
//      each change, the limit starts again, the first picture after a pause is shown at once, and
//      a summary covers one choice (its window starts again at a change);
//  (g) the 30 s summary: target, new pictures, game swaps, presents, vblanks, held, repeated,
//      dropped and how many by the limit, waited, median, p95 and longest; the game's own times
//      between pictures and the presents' (fix 146b);
//  (i) fix 146b, the console's cases (1.0.6-work-fix146: 30 showed 24.7 new pictures/s of GTA
//      IV's 37-43 and 15.8/s of Dragon Age II's 30): with paints that wake up to 3 ms early or
//      late, a game at the limit (30, steady or by a 60 Hz vblank) has every picture shown and
//      none dropped by the limit on both outputs; a game at 37-43 or 45-55 capped at 30 shows
//      30 a second, each picture at least 2 presents (4 at 120 Hz); a game alternating 26.7 and
//      40 ms (30 on average) loses at most a tenth;
//  (h) the assembled tree: the mailbox keeps the acquired picture while the limit holds (under
//      the consumer lock, before the exchange), the producer notes each picture, the Vulkan
//      presenter gives the display refresh and notes each paint and logs the lines, nothing in
//      the limit sleeps, fix 116's VdSwap pacing and the emulated vblank are unchanged, and the
//      launcher's option (Video, live, per game, Automatic by default) reaches the gate.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/present_limit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
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
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
// The text from `start` to the next line that starts a function at column 0 ("}\n").
std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return std::string();
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
namespace pl = xbox360ps5::present_limit;

constexpr double k60 = 59.94, k120 = 119.88;

// (a)
void CheckPlans() {
  struct Row {
    int choice;
    double hz;
    bool framegen;
    bool applied;
    uint32_t refreshes;
  };
  const Row rows[] = {
      {pl::kAutomatic, k60, false, false, 0}, {pl::kAutomatic, k120, false, false, 0},
      {pl::kNoLimit, k60, false, false, 0},   {pl::kNoLimit, k120, false, false, 0},
      {pl::k30, k60, false, true, 2},         {pl::k30, k120, false, true, 4},
      {pl::k40, k60, false, false, 0},        {pl::k40, k120, false, true, 3},
      {pl::k60, k60, false, true, 1},         {pl::k60, k120, false, true, 2},
      {pl::k30, k60, true, false, 0},         {pl::k40, k120, true, false, 0},
      {pl::k60, 0, false, false, 0},          {pl::k30, 1000, false, false, 0},
  };
  for (const Row& row : rows) {
    pl::Output out;
    out.hz = row.hz;
    out.fifo = true;
    out.framegen = row.framegen;
    const pl::Plan plan = pl::MakePlan(row.choice, out);
    char what[160];
    std::snprintf(what, sizeof(what), "(a) %s at %.2f Hz%s: applied %d, %u refreshes (%s)",
                  pl::ChoiceText(row.choice), row.hz, row.framegen ? " with frame generation" : "",
                  int(plan.applied), plan.refreshes, plan.reason);
    Expect(plan.applied == row.applied && plan.refreshes == row.refreshes, what);
    if (plan.applied) {
      const double expected = row.refreshes * 1e9 / row.hz;
      Expect(std::fabs(double(plan.period_ns) - expected) < 2, std::string(what) + ": period");
      Expect(std::fabs(double(plan.slack_ns) - 0.5e9 / row.hz) < 2, std::string(what) + ": slack");
    }
  }
  pl::Output out;
  out.hz = k60;
  Expect(Has(pl::MakePlan(pl::k40, out).reason, "40 FPS needs the 120 Hz output"),
         "(a) 40 at 60 Hz says it needs the 120 Hz output");
  Expect(pl::TargetFps(pl::kAutomatic) == 0 && pl::TargetFps(pl::kNoLimit) == 0 &&
             pl::TargetFps(pl::k30) == 30 && pl::TargetFps(pl::k40) == 40 && pl::TargetFps(pl::k60) == 60,
         "(a) the five choices");
}

// (b) to (g): the mailbox of Presenter::RefreshGuestOutput and ConsumeGuestOutput, modelled.
struct Result {
  uint64_t produced = 0;  // the game's pictures
  uint64_t shown = 0;     // new pictures taken by a paint
  uint64_t paints = 0;
  std::vector<int64_t> produced_at;  // when the game published each picture
  std::vector<int64_t> intervals;    // between new pictures shown, ns
  std::vector<uint64_t> presents;    // presents each shown picture had before the next
  std::vector<std::string> lines;
};
struct Game {
  // The game's time between pictures, ns, for the picture n (its own pace: unaffected by the
  // presentation, which is what the model checks).
  std::function<int64_t(uint64_t, std::mt19937&)> frame;
};
Result Run(pl::Gate& gate, double output_hz, int64_t seconds, const Game& game, uint32_t seed,
           bool framegen = false, int64_t start = 1000000000, int64_t early = 0,
           int64_t late = 700000) {
  Result result;
  std::mt19937 random(seed);
  // A paint wakes up to 0.7 ms late (fix 146b's cases: up to 3 ms early or late).
  std::uniform_int_distribution<int64_t> jitter(-early, late);
  const double refresh = 1e9 / output_hz;
  gate.SetOutputHz(output_hz);
  int64_t next_frame = start;
  uint64_t ready = 0, acquired = 0;  // picture numbers; 0: nothing
  int64_t last_shown = 0;
  uint64_t last_shown_paint = 0;
  const int64_t end = start + seconds * 1000000000;
  uint64_t vblanks = 0;
  for (uint64_t refresh_index = 0;; ++refresh_index) {
    const int64_t vblank = start + int64_t(double(refresh_index) * refresh);
    if (vblank >= end) break;
    // The game publishes every picture due before this paint (it never waits for the screen).
    while (next_frame <= vblank) {
      ready = ++result.produced;
      result.produced_at.push_back(next_frame);
      gate.NoteRefresh(next_frame);
      next_frame += game.frame(result.produced, random);
    }
    vblanks = uint64_t((vblank - start) / 8333333);  // a 120 Hz emulated vblank, for the line
    const int64_t now = vblank + jitter(random);
    std::string lines[2];
    const int count = gate.NotePaint(now, true, framegen, vblanks, lines);
    for (int i = 0; i < count; ++i) result.lines.push_back(lines[i]);
    ++result.paints;
    if (ready != acquired && gate.Take(now, acquired != 0)) {
      acquired = ready;
      ++result.shown;
      if (last_shown) {
        result.intervals.push_back(now - last_shown);
        result.presents.push_back(result.paints - last_shown_paint);
      }
      last_shown = now;
      last_shown_paint = result.paints;
    }
  }
  return result;
}
double Percentile(std::vector<int64_t> values, double share) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const size_t index = std::min(values.size() - 1, size_t(std::ceil(share * double(values.size()))) - 1);
  return double(values[index]) / 1e6;
}
Game Uneven(double low_fps, double high_fps) {
  return {[low_fps, high_fps](uint64_t, std::mt19937& random) {
    std::uniform_real_distribution<double> fps(low_fps, high_fps);
    return int64_t(1e9 / fps(random));
  }};
}
Game Steady(double fps) {
  return {[fps](uint64_t, std::mt19937&) { return int64_t(1e9 / fps); }};
}

void CheckGtaCap40() {
  // (b) GTA IV, 45 to 55 FPS, 40 on the 120 Hz output.
  pl::Gate gate;
  gate.SetChoice(pl::k40);
  const Result capped = Run(gate, k120, 40, Uneven(45, 55), 7);
  pl::Gate free_gate;
  free_gate.SetChoice(pl::kAutomatic);
  const Result free = Run(free_gate, k120, 40, Uneven(45, 55), 7);
  const double rate = double(capped.shown) / 40.0;
  Expect(rate > 39.0 && rate < 40.1, "(b) 40 on 120 Hz: about 40 new pictures a second, got " + std::to_string(rate));
  const double median = Percentile(capped.intervals, 0.5), p95 = Percentile(capped.intervals, 0.95);
  Expect(std::fabs(median - 25.03) < 1.0 && p95 < 26.0,
         "(b) 40 on 120 Hz: median " + std::to_string(median) + " ms, p95 " + std::to_string(p95) + " ms (3 refreshes)");
  uint64_t not_three = 0;
  for (int64_t interval : capped.intervals) {
    const double refreshes = double(interval) / (1e9 / k120);
    if (std::fabs(refreshes - 3) > 0.2) ++not_three;
  }
  Expect(not_three * 100 < capped.intervals.size(),
         "(b) 40 on 120 Hz: under 1% of pictures not 3 refreshes, got " + std::to_string(not_three));
  Expect(capped.produced == free.produced && capped.produced_at == free.produced_at,
         "(b) the game's pictures come at the same times with and without the limit");
  const double free_rate = double(free.shown) / 40.0;
  Expect(free_rate > 44.0, "(b) Automatic shows the game's pictures (" + std::to_string(free_rate) + "/s)");
  Expect(gate.held() > 0 && free_gate.held() == 0, "(b) only the limit holds pictures");
}

void CheckSlowerThanCap() {
  // (c) 35 FPS under a 40 cap: as without it.
  pl::Gate gate;
  gate.SetChoice(pl::k40);
  const Result capped = Run(gate, k120, 20, Steady(35), 3);
  pl::Gate free_gate;
  const Result free = Run(free_gate, k120, 20, Steady(35), 3);
  Expect(capped.shown == free.shown && capped.shown + 2 >= capped.produced,
         "(c) 35 FPS under 40: every picture shown (" + std::to_string(capped.shown) + " of " +
             std::to_string(capped.produced) + ", without the limit " + std::to_string(free.shown) + ")");
  Expect(gate.held() == 0, "(c) 35 FPS under 40: no picture held, got " + std::to_string(gate.held()));
}

void CheckDragonAge() {
  // (d) 60 FPS at the 120 Hz emulated rate, 30 on the 60 Hz output.
  pl::Gate gate;
  gate.SetChoice(pl::k30);
  const Result capped = Run(gate, k60, 30, Steady(60), 11);
  const double rate = double(capped.shown) / 30.0;
  Expect(rate > 29.5 && rate < 30.1, "(d) 30 on 60 Hz: 30 new pictures a second, got " + std::to_string(rate));
  const double median = Percentile(capped.intervals, 0.5), p95 = Percentile(capped.intervals, 0.95);
  Expect(std::fabs(median - 33.37) < 1.0 && p95 < 34.5,
         "(d) 30 on 60 Hz: 2 refreshes each, median " + std::to_string(median) + " p95 " + std::to_string(p95));
  pl::Gate free_gate;
  const Result free = Run(free_gate, k60, 30, Steady(60), 11);
  Expect(double(free.shown) / 30.0 > 58.0, "(d) Automatic: every picture of the 60 FPS game");
  Expect(capped.produced_at == free.produced_at, "(d) the game's pace is unchanged by the limit");
}

void CheckNotApplied() {
  // (e)
  pl::Gate gate;
  gate.SetChoice(pl::k40);
  const Result at60 = Run(gate, k60, 10, Uneven(45, 55), 5);
  Expect(gate.held() == 0 && !gate.plan().applied, "(e) 40 on the 60 Hz output: not applied");
  Expect(!at60.lines.empty() && Has(at60.lines[0], "not applied") && Has(at60.lines[0], "120 Hz output"),
         "(e) 40 on 60 Hz: the line says why: " + (at60.lines.empty() ? std::string() : at60.lines[0]));
  pl::Gate fg;
  fg.SetChoice(pl::k30);
  const Result with_fg = Run(fg, k120, 10, Steady(60), 5, true);
  Expect(fg.held() == 0 && !with_fg.lines.empty() && Has(with_fg.lines[0], "frame generation"),
         "(e) frame generation on: not applied, and said so");
}

void CheckChanges() {
  // (f) one session: 40, then Automatic, then 30; a pause; a new output.
  pl::Gate gate;
  gate.SetChoice(pl::k40);
  Result first = Run(gate, k120, 5, Steady(55), 1, false, 1000000000);
  Expect(first.lines.size() == 1 && Has(first.lines[0], "40 FPS chosen; applied") &&
             Has(first.lines[0], "3 refreshes of 119.88 Hz (FIFO)"),
         "(f) the first line: " + (first.lines.empty() ? std::string() : first.lines[0]));
  gate.SetChoice(pl::kAutomatic);
  Result second = Run(gate, k120, 5, Steady(55), 2, false, 7000000000);
  Expect(second.lines.size() == 1 && Has(second.lines[0], "Automatic chosen; not applied"),
         "(f) back to Automatic: " + (second.lines.empty() ? std::string() : second.lines[0]));
  Expect(double(second.shown) / 5.0 > 53.0, "(f) Automatic shows every picture again");
  gate.SetChoice(pl::k30);
  // A pause: the game shows nothing for 2 s, then 60 FPS.
  Result third = Run(gate, k120, 5, Steady(60), 3, false, 14000000000);
  Expect(third.lines.size() == 1 && Has(third.lines[0], "30 FPS chosen; applied") &&
             Has(third.lines[0], "4 refreshes"),
         "(f) then 30: " + (third.lines.empty() ? std::string() : third.lines[0]));
  const double rate = double(third.shown) / 5.0;
  Expect(rate > 29.0 && rate < 30.2, "(f) 30 on 120 Hz: about 30 a second, got " + std::to_string(rate));
  // After a pause longer than kPauseNs the first new picture is taken at once.
  const int64_t after = 25000000000;
  gate.NoteRefresh(after);
  Expect(gate.Take(after + 1000000, true), "(f) after a pause the first picture is shown at once");
  Expect(!gate.Take(after + 9000000, true), "(f) and the next one waits for its refreshes");
  // A new output (the surface made again at 60 Hz): the line, and 30 is 2 refreshes now.
  gate.SetOutputHz(k60);
  std::string lines[2];
  const int count = gate.NotePaint(after + 20000000, true, false, 0, lines);
  Expect(count == 1 && Has(lines[0], "2 refreshes of 59.94 Hz"), "(f) a new output: " + lines[0]);
  // An unknown choice is Automatic.
  gate.SetChoice(9);
  Expect(gate.choice() == pl::kAutomatic, "(f) an unknown choice is Automatic");

  // Fix 146b: a summary covers one choice. 20 s at 40, then 30: the first summary comes 30 s
  // after the change and counts 30 a second (not a mix of 40 and 30).
  pl::Gate mixed;
  mixed.SetChoice(pl::k40);
  const Result at40 = Run(mixed, k120, 20, Steady(55), 5, false, 1000000000);
  mixed.SetChoice(pl::k30);
  const Result at30 = Run(mixed, k120, 35, Steady(55), 6, false, 21000000000);
  std::string summary;
  for (const std::string& line : at30.lines)
    if (Has(line, "new pictures/s")) summary = line;
  Expect(!summary.empty() && Has(summary, "target 30 FPS") &&
             (Has(summary, "; 30.0 new pictures/s") || Has(summary, "; 29.9 new pictures/s")),
         "(f) the summary after a change covers the new choice only: " + summary);
  for (const std::string& line : at40.lines)
    Expect(!Has(line, "new pictures/s"), "(f) no summary within 20 s of the choice: " + line);
}

void CheckReport() {
  // (g) 40 s of GTA IV at 40: one summary after 30 s, with every field.
  pl::Gate gate;
  gate.SetChoice(pl::k40);
  const Result result = Run(gate, k120, 40, Uneven(45, 55), 9);
  std::string report;
  for (const std::string& line : result.lines)
    if (Has(line, "new pictures/s")) report = line;
  Expect(!report.empty(), "(g) a summary line within 40 s");
  for (const char* field : {"Presentation limit (fix 146): target 40 FPS (3 refreshes of 119.88 Hz)",
                            "new pictures/s of", "game swaps/s", "presents/s", "vblanks/s",
                            "held ", "repeated ", "dropped ", "/s by the limit)", "waited ",
                            "ms a picture", "interval median 25.", "p95 25.", "longest ",
                            "game interval median ", "present interval median "})
    Expect(Has(report, field), std::string("(g) the summary has \"") + field + "\": " + report);
  // 40.0 new pictures of about 50 game swaps; about 120 presents.
  Expect(Has(report, ": target 40 FPS") && (Has(report, "; 40.0 new pictures/s") || Has(report, "; 39.9 new pictures/s")),
         "(g) 40 new pictures a second: " + report);
  Expect(Has(report, "119.") || Has(report, "120.0 presents/s"), "(g) about 120 presents a second: " + report);
  pl::Gate automatic;
  const Result plain = Run(automatic, k60, 31, Steady(30), 4);
  Expect(!plain.lines.empty() && Has(plain.lines.back(), "Automatic, not applied") &&
             Has(plain.lines.back(), "30.0 game swaps/s"),
         "(g) Automatic: the summary is written too (no first line): " +
             (plain.lines.empty() ? std::string() : plain.lines.back()));
  Expect(plain.lines.size() == 1, "(g) Automatic from the start: no plan line, only the summary");
}

// (i) fix 146b.
Game ByVblank60(double jitter_ms) {
  // 30 by two vblanks of an emulated 60.00 Hz, each picture published up to `jitter_ms` later.
  return {[jitter_ms](uint64_t, std::mt19937& random) {
    std::uniform_real_distribution<double> jitter(-jitter_ms, jitter_ms);
    return int64_t(2e9 / 60.0 + jitter(random) * 1e6);
  }};
}
Game Alternating() {
  return {[](uint64_t n, std::mt19937&) { return int64_t(n % 2 ? 26.7e6 : 40e6); }};
}
void CheckConsoleCases() {
  constexpr int64_t kEarly = 3000000, kLate = 3000000;
  for (double output : {k60, k120}) {
    const std::string at = output < 100 ? " at 60 Hz" : " at 120 Hz";
    const uint32_t refreshes = output < 100 ? 2 : 4;
    struct AtLimit {
      const char* name;
      Game game;
    } at_limit[] = {{"steady 30", Steady(30)}, {"30 by a 60 Hz vblank", ByVblank60(1.0)}};
    for (const AtLimit& row : at_limit) {
      pl::Gate gate;
      gate.SetChoice(pl::k30);
      const Result result = Run(gate, output, 40, row.game, 21, false, 1000000000, kEarly, kLate);
      // 30.00 is a hair faster than 2 refreshes of 59.94 Hz (29.97 a second): over 40 s the
      // limit may drop the 1.2 pictures that difference makes, no more.
      const double most = 40.0 * output / refreshes;
      const uint64_t over = result.produced > uint64_t(most) ? result.produced - uint64_t(most) : 0;
      Expect(result.shown + 2 + over >= result.produced && gate.limit_dropped() <= over + 1,
             std::string("(i) ") + row.name + " under 30" + at + ": every picture shown (" +
                 std::to_string(result.shown) + " of " + std::to_string(result.produced) +
                 ", dropped by the limit " + std::to_string(gate.limit_dropped()) + ")");
    }
    struct Faster {
      const char* name;
      Game game;
    } faster[] = {{"37-43", Uneven(37, 43)}, {"45-55", Uneven(45, 55)}, {"60", Steady(60)}};
    for (const Faster& row : faster) {
      pl::Gate gate;
      gate.SetChoice(pl::k30);
      const Result result = Run(gate, output, 40, row.game, 23, false, 1000000000, kEarly, kLate);
      const double rate = double(result.shown) / 40.0;
      Expect(rate > 29.5 && rate < 30.1,
             std::string("(i) ") + row.name + " capped at 30" + at + ": 30 a second, got " +
                 std::to_string(rate));
      uint64_t short_ones = 0;
      for (uint64_t presents : result.presents)
        if (presents < refreshes) ++short_ones;
      Expect(short_ones == 0, std::string("(i) ") + row.name + " capped at 30" + at +
                                  ": every picture on screen for " + std::to_string(refreshes) +
                                  " presents or more, " + std::to_string(short_ones) + " shorter");
      Expect(gate.limit_dropped() > 0 && gate.limit_dropped() <= result.produced - result.shown,
             std::string("(i) ") + row.name + " capped at 30" + at + ": the limit's drops are counted");
    }
    pl::Gate gate;
    gate.SetChoice(pl::k30);
    const Result uneven = Run(gate, output, 40, Alternating(), 25, false, 1000000000, kEarly, kLate);
    Expect(uneven.shown * 10 >= uneven.produced * 9,
           "(i) 26.7 and 40 ms in turn under 30" + at + ": at most a tenth not shown, " +
               std::to_string(uneven.shown) + " of " + std::to_string(uneven.produced));
  }
}

// (h) the assembled tree.
void CheckTree() {
  const std::string header = Read("include/xbox360ps5/present_limit.hpp");
  Expect(!header.empty(), "(h) include/xbox360ps5/present_limit.hpp in the tree");
  for (const char* sleep : {"sleep(", "Sleep(", "sleep_for", "wait_for", "wait_until", "this_thread",
                            "condition_variable"})
    Expect(!Has(header, sleep), std::string("(h) nothing in the limit sleeps (") + sleep + ")");
  Expect(Has(header, "return presents >= plan_.refreshes ||") && !Has(header, "deadline_"),
         "(h) fix 146b: a picture is held for its presents, with no time grid");

  const std::string presenter = Read(".deps/xenia-canary/src/xenia/ui/presenter.cc");
  const std::string consume = Body(presenter, "std::unique_lock<std::mutex> Presenter::ConsumeGuestOutput(");
  const size_t lock_at = consume.find("guest_output_mailbox_consumer_mutex_");
  const size_t take_at = consume.find("present_limit::Instance().Take(");
  const size_t exchange_at = consume.find("compare_exchange_weak");
  Expect(lock_at != std::string::npos && take_at != std::string::npos && exchange_at != std::string::npos &&
             lock_at < take_at && take_at < exchange_at,
         "(h) ConsumeGuestOutput asks the limit under the consumer lock, before the exchange");
  Expect(Has(consume, "desired_acquired_and_ready = old_acquired_and_ready;"),
         "(h) a held picture stays the acquired one (the exchange is skipped)");
  Expect(Has(consume, "guest_output_properties_[old_acquired_and_ready & 3].IsActive()"),
         "(h) a blank picture is replaced at once");
  const std::string refresh = Body(presenter, "bool Presenter::RefreshGuestOutput(");
  const size_t publish_at = refresh.find("guest_output_mailbox_acquired_and_ready_.compare_exchange_weak");
  const size_t note_at = refresh.find("present_limit::Instance().NoteRefresh(");
  Expect(publish_at != std::string::npos && note_at != std::string::npos && publish_at < note_at,
         "(h) RefreshGuestOutput notes each picture after it is published");
  Expect(!Has(refresh, "Take("), "(h) the producer never asks the limit (it never waits)");

  const std::string vulkan = Read(".deps/xenia-canary/src/xenia/ui/vulkan/vulkan_presenter.cc");
  Expect(Has(vulkan, "present_limit::Instance().SetOutputHz(double(selected_refresh) / 1000.0)"),
         "(h) the KHR_display surface gives the limit its refresh");
  const std::string paint = Body(vulkan, "Presenter::PaintResult VulkanPresenter::PaintAndPresentImpl(");
  Expect(Has(paint, "present_limit::Instance().NotePaint(") && Has(paint, "paint_context_.swapchain_is_fifo") &&
             Has(paint, "ps5x360::framegen::source_ticks") && Has(paint, "XELOGW(\"{}\""),
         "(h) each paint notes the output, frame generation and vblanks, and logs the lines");

  const std::string video = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc");
  Expect(Has(video, "Performance swap pacing (fix 116)") && Has(video, "pacer.OnSwap(now)") &&
             !Has(video, "present_limit"),
         "(h) fix 116's VdSwap pacing unchanged, and the limit is not in VdSwap");
  const std::string graphics = Read(".deps/xenia-canary/src/xenia/gpu/graphics_system.cc");
  Expect(Has(graphics, "ps5x360::framegen::Tick(double(cvars::framerate_limit ? cvars::framerate_limit : 60));") &&
             !Has(graphics, "present_limit"),
         "(h) the emulated vblank (framerate_limit) unchanged");

  const std::string launcher = Read("src/launcher.cpp");
  Expect(Has(launcher, "{\"present_limit\", &S::present_limit, 0, W::live, 0, true, \"Frame rate limit (screen)\","),
         "(h) the option: Video, live (guide and web page), Automatic recommended, per game");
  Expect(Has(launcher, "{\"Automatic\", \"No limit\", \"30\", \"40\", \"60\"}"), "(h) its five choices, in Choice's order");
  Expect(Has(launcher, "xbox360ps5::present_limit::Instance().SetChoice(present_limit);"),
         "(h) Settings::Apply gives the choice to the limit (also from the guide)");
  Expect(Has(launcher, "key == \"present_limit\" && (value < 0 || value > 4)"),
         "(h) an unknown saved value is Automatic, not the nearest choice");
  const size_t clock_row = launcher.find("{\"video_clock\", &S::video_clock");
  const size_t vsync_row = launcher.find("{\"vsync\", &S::vsync");
  const size_t limit_row = launcher.find("{\"present_limit\", &S::present_limit");
  Expect(clock_row < vsync_row && vsync_row < limit_row, "(h) the option follows Emulated video rate and VSync");
  const std::string settings = Read("include/xbox360ps5/launcher.hpp");
  Expect(Has(settings, "int present_limit = 0;"), "(h) Settings::present_limit, Automatic by default");
}

}  // namespace

int main() {
  CheckPlans();
  CheckGtaCap40();
  CheckSlowerThanCap();
  CheckDragonAge();
  CheckNotApplied();
  CheckChanges();
  CheckReport();
  CheckConsoleCases();
  CheckTree();
  if (failures) {
    std::printf("check-present-limit: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-present-limit: ok\n");
  return 0;
}
