// Fix 196b: the emergency pose's hold and hint after the phone goes silent
// (include/xbox360ps5/motion_emergency.hpp, emergency_pad_source.hpp).
//
//   check-emergency-hold-hint       run from an assembled tree's root
//
// 1.0.7_candidate_2 on Kinect Sports (research/c2-107/ks-l3r3.log): the player covered the
// phone's camera, the phone sent frames without a body and then nothing for 18 s while the game
// kept reading skeletons (about 570 reads), and holding L3 + R3 showed neither the emergency pose
// nor the "No player seen" hint, and logged nothing. This replays that sequence at 30 reads a
// second with the frames the virtual sensor gets (NormalizedSnapshot: tracked bodies, then frames
// without a body, then the last frame gone stale, which Snapshot marks untracked) and the game
// loop's pad order (slots 2-4, then player 1), and checks:
//  - the hint's state 3 s after the body went, and the overlay's log line for it (HintLog);
//  - a hold straight after the loss (0.6 s later) starts the emergency pose (fix 196b: no body for
//    the hold's length, not 3 s), as does a hold after 4 s; a hold while the phone has a body does
//    not, and says why in the log (HoldReport);
//  - the pad that feeds the detector is the lowest connected slot (review L1);
//  - the tree: the pad logs the hold report and the overlay logs what its line shows.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/emergency_pad_source.hpp"

namespace m = xbox360ps5::motion;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::string ReadText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

static m::Frame Body(uint32_t sequence) {
  m::Frame f;
  f.tracked = true;
  f.session = 0x9429;
  f.sequence = sequence;
  for (int n = 0; n < 20; ++n) f.joints[size_t(n)] = {0, 0.05f * float(n), 0, 1};
  return f;
}

// The phone, as the virtual sensor's reads see it at `ms` (ms 0: the camera is covered): a body
// until then, frames without a body for 0.4 s, then nothing (the last frame, stale: untracked).
static m::Frame Phone(int ms) {
  if (ms < 0) return Body(uint32_t(20000 + ms / 33));
  m::Frame f = Body(19388);
  f.tracked = false;
  return f;
}

struct Session {
  m::EmergencyPose pose;
  m::HintLog log;
  Clock::time_point start = Clock::time_point{} + std::chrono::hours(2);
  int events_on = 0;
  int presses = 0;
  std::string last_line;
  Clock::time_point At(int ms) const { return start + milliseconds(ms); }
  // One frame of the game loop at `ms`: pads (slots 2-4 disconnected, player 1 holding or not),
  // the title's skeleton read, the overlay's line.
  void Frame(int ms, bool held, bool p1_connected = true) {
    for (uint32_t slot = 1; slot < 4; ++slot) presses += m::EmergencyPadSample(pose, slot, false, 0, At(ms));
    presses += m::EmergencyPadSample(pose, 0, p1_connected, held ? m::kEmergencyButtons : 0, At(ms));
    m::EmergencyEvent event = m::EmergencyEvent::kNone;
    pose.Apply(Phone(ms), At(ms + 2), &event);
    events_on += event == m::EmergencyEvent::kOn;
    const auto shown = m::ChooseHint(false, pose.Active(), pose.HintVisible(At(ms + 5)));
    if (const auto line = log.Note(shown)) last_line = *line;
  }
};

static void CheckSilentPhone() {
  // (1) Covered, no hold: the hint after 3 s, logged once; nothing else.
  {
    m::emergency_pads_connected = 0;
    Session s;
    for (int ms = -3000; ms < 0; ms += 33) s.Frame(ms, false);
    CHECK(s.last_line.empty());  // nothing to say while the body is there
    int ms = 0;
    for (; ms < 2900; ms += 33) s.Frame(ms, false);
    CHECK(!s.pose.HintVisible(s.At(ms)) && s.last_line.empty());
    for (; ms < 18000; ms += 33) s.Frame(ms, false);  // ~570 reads without a body
    CHECK(s.pose.HintVisible(s.At(ms)));
    CHECK(s.last_line == "Motion panel: the emergency line shows \"No player seen. Hold L3 + R3...\" (fix 196b)");
    CHECK(s.log.lines == 1 && s.events_on == 0 && s.presses == 0);
  }
  // (2) Covered and held at once (0.6 s after the loss, for 1 s): the emergency pose starts once
  // the phone has had no body for 0.5 s; let go, it ends and the hint comes back after 3 s.
  {
    m::emergency_pads_connected = 0;
    Session s;
    for (int ms = -3000; ms < 0; ms += 33) s.Frame(ms, false);
    int ms = 0;
    for (; ms < 600; ms += 33) s.Frame(ms, false);
    for (; ms < 1600; ms += 33) s.Frame(ms, true);
    CHECK(s.events_on == 1 && s.presses == 0);
    CHECK(s.last_line == "Motion panel: the emergency line shows \"Emergency pose on...\" (fix 196b)");
    for (; ms < 5000; ms += 33) s.Frame(ms, false);
    CHECK(!s.pose.Active() && s.pose.HintVisible(s.At(ms)));
    CHECK(!s.pose.TakeHoldReport().ready);  // it started: nothing to explain
  }
  // (3) Held after 4 s of silence (as the console test plan said): on.
  {
    m::emergency_pads_connected = 0;
    Session s;
    for (int ms = -1000; ms < 0; ms += 33) s.Frame(ms, false);
    int ms = 0;
    for (; ms < 4000; ms += 33) s.Frame(ms, false);
    for (; ms < 5000; ms += 33) s.Frame(ms, true);
    CHECK(s.events_on == 1 && s.pose.Active());
  }
  // (4) Held while the phone still has a body: nothing, and the release says why.
  {
    m::emergency_pads_connected = 0;
    Session s;
    for (int ms = -3000; ms < -1900; ms += 33) s.Frame(ms, false);
    for (int ms = -1900; ms < -900; ms += 33) s.Frame(ms, true);
    s.Frame(-880, false);
    CHECK(s.events_on == 0 && s.presses == 0);
    const m::HoldReport report = s.pose.TakeHoldReport();
    CHECK(report.ready && report.reads && report.held_ms >= 990 && report.held_ms <= 1030);
    CHECK(report.since_body_ms >= 0 && report.since_body_ms < 100);
    const std::string text = m::HoldReportText(report);
    CHECK(text.find("the emergency pose did not start: the phone had a body during the hold") != std::string::npos);
    CHECK(!s.pose.TakeHoldReport().ready);  // said once
  }
  // (5) A game that reads no skeletons: the hold says so.
  {
    m::EmergencyPose pose;
    const Clock::time_point t = Clock::time_point{} + std::chrono::hours(3);
    pose.Pad(true, t);
    pose.Pad(false, t + milliseconds(800));
    const m::HoldReport report = pose.TakeHoldReport();
    CHECK(report.ready && !report.reads && report.since_body_ms == -1);
    CHECK(m::HoldReportText(report).find("the game reads no skeletons") != std::string::npos);
  }
  // The log's line names and the hint choice.
  CHECK(m::ChooseHint(true, true, true) == m::HintShown::kRecentring);
  CHECK(m::ChooseHint(false, true, true) == m::HintShown::kEmergencyOn);
  CHECK(m::ChooseHint(false, false, true) == m::HintShown::kNoPlayer);
  CHECK(m::ChooseHint(false, false, false) == m::HintShown::kNone);
  {
    m::HintLog log;
    CHECK(!log.Note(m::HintShown::kNone));  // nothing shown at the start: no line
    CHECK(log.Note(m::HintShown::kNoPlayer) && !log.Note(m::HintShown::kNoPlayer));
    const auto off = log.Note(m::HintShown::kNone);
    CHECK(off && *off == "Motion panel: the emergency line shows nothing (fix 196b)");
    for (unsigned n = 0; n < 2 * m::kHintLines; ++n) log.Note(n % 2 ? m::HintShown::kNone : m::HintShown::kRecentring);
    CHECK(log.lines == m::kHintLines);
  }
  std::printf("silent phone: the hint after 3 s, the emergency pose from a hold 0.6 s or 4 s after the loss, reasons logged\n");
}

static void CheckLowestSlot() {
  // Review L1: only another user's pad connected (slot 3): its press re-centres and its hold works.
  m::emergency_pads_connected = 0;
  CHECK(m::EmergencySlot(0) == 0 && m::EmergencySlot(0b1000) == 3 && m::EmergencySlot(0b1010) == 1 &&
        m::EmergencySlot(0b1011) == 0);
  m::EmergencyPose pose;
  const Clock::time_point start = Clock::time_point{} + std::chrono::hours(4);
  int presses = 0, on = 0;
  auto frame = [&](int f, uint32_t held_slot, bool down, const bool (&connected)[4]) {
    const Clock::time_point now = start + milliseconds(f * 16);
    for (uint32_t slot : {1u, 2u, 3u, 0u})
      presses += m::EmergencyPadSample(pose, slot, connected[slot],
                                       connected[slot] && down && slot == held_slot ? m::kEmergencyButtons : 0, now);
    m::EmergencyEvent event{};
    m::Frame none;
    pose.Apply(none, now + milliseconds(2), &event);
    on += event == m::EmergencyEvent::kOn;
  };
  const bool only3[4] = {false, false, false, true};
  int f = 0;
  for (; f < 10; ++f) frame(f, 3, false, only3);
  for (; f < 20; ++f) frame(f, 3, true, only3);  // a brief press
  for (; f < 40; ++f) frame(f, 3, false, only3);
  CHECK(presses == 1 && on == 0);
  for (; f < 120; ++f) frame(f, 3, true, only3);  // a hold
  CHECK(on == 1 && presses == 1);
  for (; f < 140; ++f) frame(f, 3, false, only3);
  // Player 1's pad comes back: it feeds again, slot 3 no longer does.
  const bool both[4] = {true, false, false, true};
  for (; f < 150; ++f) frame(f, 3, false, both);
  for (; f < 160; ++f) frame(f, 3, true, both);
  for (; f < 180; ++f) frame(f, 3, false, both);
  CHECK(presses == 1);
  for (; f < 190; ++f) frame(f, 0, true, both);
  for (; f < 200; ++f) frame(f, 0, false, both);
  CHECK(presses == 2);
  std::printf("lowest connected slot: another user's lone pad re-centres and holds; player 1's wins when back\n");
}

static void CheckTree() {
  const std::string pad = ReadText("platform/ps5/dualsense_input.cpp");
  const std::string game = ReadText("src/canary_game_main.cpp");
  if (pad.empty() || game.empty()) {
    std::printf("no assembled tree here; the tree not checked\n");
    return;
  }
  const size_t sample = pad.find("motion::EmergencyPadSample(motion::emergency_pose, user_, sample.connected, sample.buttons,");
  const size_t report = pad.find("motion::emergency_pose.TakeHoldReport(); report.ready && motion::enabled.load())");
  CHECK(sample != std::string::npos && report != std::string::npos && sample < report);
  CHECK(pad.find("XELOGW(\"Virtual Kinect: {} (fix 196b)\", motion::HoldReportText(report));") != std::string::npos);
  const size_t hint = game.find("void DrawEmergencyHint(");
  const size_t note = game.find("xbox360ps5::motion::hint_log.Note(xbox360ps5::motion::ChooseHint(recentring, on, visible))", hint);
  const size_t ret = game.find("if (!recentring && !on && !visible) return;", hint);
  CHECK(hint != std::string::npos && note != std::string::npos && ret != std::string::npos && note < ret);
  CHECK(game.find("DrawEmergencyHint(scale, now);  // fix 107, also with the panel off") != std::string::npos);
  std::printf("tree: the pad logs a hold that started nothing, the overlay logs what its line shows\n");
}

int main() {
  CheckSilentPhone();
  CheckLowestSlot();
  CheckTree();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-emergency-hold-hint: all passed\n");
  return 0;
}
