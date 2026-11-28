// Fix 196: fix 107's L3 + R3 detector hears player 1's pad only
// (include/xbox360ps5/emergency_pad_source.hpp).
//
//   check-emergency-pad-source       run from an assembled tree's root
//
// Replays the game loop's order (slots 2-4 submitted before player 1's pad every frame, 60 Hz):
// fed from every slot as before, one brief press made a re-centre request a frame and a hold never
// started the emergency pose; fed through EmergencyPadSample, a brief press is one request, a hold
// on player 1's pad starts the emergency pose after kHoldToStart, and a hold on another slot does
// nothing. Then the tree's DualSenseInput::Submit.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/emergency_pad_source.hpp"

namespace m = xbox360ps5::motion;
using Clock = std::chrono::steady_clock;

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

constexpr uint32_t kBoth = m::kEmergencyButtons, kFrameMs = 16;

struct Run {
  int presses = 0;           // re-centre requests
  bool emergency = false;    // the emergency pose started
  int emergency_events = 0;  // kOn events
};

// `frames` frames of the game loop with `held_slot` holding L3 + R3 for the first `hold_frames`
// (then all let go); `connected[n]` says which slots have a pad. `old_way` feeds every slot to
// EmergencyPose::Pad as fix 107 did. Each frame the title reads a skeleton without a body (Apply).
static Run Loop(uint32_t held_slot, int hold_frames, int frames, const bool (&connected)[4], bool old_way) {
  m::EmergencyPose pose;
  const Clock::time_point start = Clock::time_point{} + std::chrono::hours(1);
  Run run;
  for (int f = 0; f < frames; ++f) {
    const Clock::time_point now = start + std::chrono::milliseconds(f * kFrameMs);
    auto submit = [&](uint32_t slot) {
      const bool down = slot == held_slot && f < hold_frames;
      const uint32_t buttons = connected[slot] && down ? kBoth : 0;
      const bool press = old_way ? pose.Pad(connected[slot] && (buttons & kBoth) == kBoth, now)
                                 : m::EmergencyPadSample(pose, slot, connected[slot], buttons, now);
      run.presses += press;
    };
    for (uint32_t slot = 1; slot < 4; ++slot) submit(slot);  // poll_extra_players
    submit(0);                                                // player 1's pad (input->Submit)
    m::EmergencyEvent event = m::EmergencyEvent::kNone;
    m::Frame none;
    pose.Apply(none, now + std::chrono::milliseconds(5), &event);  // the title's skeleton read
    if (event == m::EmergencyEvent::kOn) {
      run.emergency = true;
      ++run.emergency_events;
    }
  }
  return run;
}

int main() {
  const bool only_p1[4] = {true, false, false, false};
  const bool four[4] = {true, true, true, true};
  const bool mixed[4] = {true, false, true, false};
  // 1.0.7_candidate_1 (fix 107 fed from every slot): a 10-frame press was 10 requests (Nike+'s
  // seq 1-10) and a 2 s hold never started the emergency pose.
  const Run old_press = Loop(0, 10, 60, only_p1, true);
  CHECK(old_press.presses == 10);
  const Run old_hold = Loop(0, 125, 160, only_p1, true);
  CHECK(!old_hold.emergency && old_hold.presses > 100);
  std::printf("fed from every slot: a 10-frame press made %d requests; a 2 s hold %d and no emergency pose\n",
              old_press.presses, old_hold.presses);
  // Fix 196, slots interleaving connected and disconnected pads: one brief press is one request,
  // and the hold is never reset.
  for (const auto* pads : {&only_p1, &four, &mixed}) {
    const Run press = Loop(0, 10, 60, *pads, false);
    CHECK(press.presses == 1 && !press.emergency);
    const Run quick = Loop(0, 1, 30, *pads, false);
    CHECK(quick.presses == 1);
    // A hold on player 1's pad starts the emergency pose once kHoldToStart has passed, once, and
    // is no re-centre request.
    const Run hold = Loop(0, 125, 160, *pads, false);
    CHECK(hold.emergency && hold.emergency_events == 1 && hold.presses == 0);
    // Just under the threshold: not yet the pose; a release then is a re-centre request.
    const int under = int(m::kHoldToStart.count() / kFrameMs) - 1;
    const Run short_hold = Loop(0, under, 60, *pads, false);
    CHECK(!short_hold.emergency && short_hold.presses == 1);
    // A hold on another slot does nothing (player 1 not pressing).
    for (uint32_t slot = 1; slot < 4; ++slot) {
      const Run other = Loop(slot, 125, 160, *pads, false);
      CHECK(!other.emergency && other.presses == 0);
      const Run other_press = Loop(slot, 10, 60, *pads, false);
      CHECK(other_press.presses == 0);
    }
  }
  CHECK(m::FeedsEmergency(0) && !m::FeedsEmergency(1) && !m::FeedsEmergency(3) && !m::FeedsEmergency(255));
  // A disconnected player 1 pad holds nothing, whatever its buttons say.
  {
    m::EmergencyPose pose;
    const Clock::time_point t = Clock::time_point{} + std::chrono::hours(1);
    CHECK(!m::EmergencyPadSample(pose, 0, false, kBoth, t));
    CHECK(!m::EmergencyPadSample(pose, 0, false, 0, t + std::chrono::milliseconds(100)));
  }
  std::printf("player 1 only: one request a press, the hold starts the emergency pose, other slots ignored\n");

  // The tree: Submit feeds the detector through EmergencyPadSample with its own slot, before the
  // pad's lock, and no longer calls Pad itself.
  const std::string pad = ReadText("platform/ps5/dualsense_input.cpp");
  if (pad.empty()) {
    std::printf("no assembled tree here; the tree not checked\n");
  } else {
    CHECK(pad.find("#include \"xbox360ps5/emergency_pad_source.hpp\"") != std::string::npos);
    const size_t submit = pad.find("void DualSenseInput::Submit");
    const size_t call = pad.find("motion::EmergencyPadSample(motion::emergency_pose, user_, sample.connected, sample.buttons,", submit);
    CHECK(submit != std::string::npos && call != std::string::npos);
    CHECK(call < pad.find("std::lock_guard<std::mutex> lock(mutex_);", submit));
    CHECK(pad.find("motion::emergency_pose.Pad(") == std::string::npos);
    CHECK(pad.find("\"Virtual Kinect: re-centre requested from the pad (seq {}) (fix 107)\"") != std::string::npos);
    std::printf("tree: DualSenseInput::Submit feeds the detector through EmergencyPadSample(user_)\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-emergency-pad-source: all passed\n");
  return 0;
}
