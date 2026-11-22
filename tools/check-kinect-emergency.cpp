// SPDX-License-Identifier: MIT
// Fix 107 (Kinect Sports, issue 15): the emergency pose out of a Kinect game's "player lost" pause.
//
//   check-kinect-emergency        run from an assembled tree's root
//
//  (a) the trigger: L3 + R3 held for 0.5 s while no body has been published for 3 s; not with a
//      recent body, not for a short press, not before the title reads skeletons; the hint shows
//      only then and not while the pose is on;
//  (b) the body: the last one published (a standing body at 2.5 m when there was none), tracked,
//      under the old tracking id (SkeletonIdentity's resume) and the same player binding;
//  (c) the hand-over: the phone's body ends it, in the old session even when the phone's session
//      is new: no new tracking id, so no identity notification; letting go ends it; 10 s ends it
//      and it does not start again until the buttons are let go; Reset forgets the last body;
//  (d) the tree: the skeleton read, the depth camera, the pad and the overlay use it, the log
//      lines, and the overlay's four texts in all 16 language files (the argument);
//  (e) the same pair pressed briefly asks the phone to re-centre (a hold does not); each request
//      has a number and a 3 s countdown, rides on the answers to the page's frames and on the
//      status while it waits (10 s past its countdown at most), and the page's report
//      acknowledges it once; the server's wiring and log lines.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/motion_emergency.hpp"
#include "xbox360ps5/virtual_nui.hpp"
#include "xbox360ps5/phone_latency.hpp"
#include "xbox360ps5/phone_qr_panel.hpp"

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

static std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

static m::Frame Body(uint32_t session, uint32_t sequence, float hand_y) {
  m::Frame f = m::StandingBody(session);
  f.sequence = sequence;
  f.joints[11].y = hand_y;  // the right hand, to tell bodies apart
  return f;
}
static m::Frame NoBody(uint32_t session) {
  m::Frame f;
  f.session = session;
  return f;
}

int main(int argc, char** argv) {
  const Clock::time_point t0 = Clock::now();
  auto at = [&](int ms) { return t0 + milliseconds(ms); };

  // (a) and (b): a player tracked, then lost; the title keeps reading skeletons (every 33 ms).
  {
    m::EmergencyPose e;
    m::SkeletonIdentity identity;
    m::EmergencyEvent ev;
    CHECK(!e.HintVisible(at(0)));  // no skeleton read yet: no hint
    m::Frame out = e.Apply(Body(7, 1, .9f), at(0), &ev);
    const uint32_t id = identity.Update(out, ev == m::EmergencyEvent::kOn);
    CHECK(out.tracked && id != 0 && ev == m::EmergencyEvent::kNone);
    const auto bound = m::PublishNuiFrame(out, id, 0b0001);
    CHECK(bound.user_index == 0 && bound.tracking_id == id);
    for (int b = 33; b <= 990; b += 33) out = e.Apply(Body(7, uint32_t(1 + b), .9f), at(b), &ev), identity.Update(out);
    CHECK(identity.Update(out) == id);
    // Lost at 1 s.
    int t = 1000;
    for (; t < 3500; t += 33) out = e.Apply(NoBody(7), at(t), &ev), identity.Update(out);
    CHECK(!out.tracked && ev == m::EmergencyEvent::kNone);
    CHECK(!e.HintVisible(at(t)));  // 2.5 s without a body: not yet
    for (; t < 4100; t += 33) e.Apply(NoBody(7), at(t), &ev);
    CHECK(e.HintVisible(at(t)));   // over 3 s
    // A short press does nothing.
    e.Pad(true, at(t));
    out = e.Apply(NoBody(7), at(t + 300), &ev);
    CHECK(!out.tracked && ev == m::EmergencyEvent::kNone);
    e.Pad(false, at(t + 330));
    out = e.Apply(NoBody(7), at(t + 600), &ev);
    CHECK(!out.tracked);
    // Held for 0.5 s: on, with the last body, tracked, under the old tracking id and binding.
    t += 1000;
    e.Pad(true, at(t));
    out = e.Apply(NoBody(7), at(t + 400), &ev);
    CHECK(!out.tracked && ev == m::EmergencyEvent::kNone);
    out = e.Apply(NoBody(7), at(t + 520), &ev);
    CHECK(ev == m::EmergencyEvent::kOn && out.tracked && out.session == 7 && out.joints[11].y == .9f);
    CHECK(e.Active() && !e.HintVisible(at(t + 520)));
    const uint32_t resumed = identity.Update(out, ev == m::EmergencyEvent::kOn);
    CHECK(resumed == id);
    const auto rebound = m::PublishNuiFrame(out, resumed, 0b0001);
    CHECK(rebound.user_index == bound.user_index && rebound.tracking_id == bound.tracking_id);
    // Kept while held; the depth camera's view has it too.
    for (int n = 1; n < 30; ++n) {
      out = e.Apply(NoBody(7), at(t + 520 + n * 33), &ev);
      CHECK(out.tracked && ev == m::EmergencyEvent::kNone && identity.Update(out) == id);
    }
    CHECK(e.Overlay(NoBody(7)).tracked && e.Overlay(NoBody(7)).joints[11].y == .9f);
    CHECK(e.Overlay(Body(7, 9, .1f)).joints[11].y == .1f);  // a real body is never covered
    // (c) The phone's body comes back in a NEW session (the page was reloaded): it takes over in
    // the old one, so the title keeps its tracking id: no identity notification.
    t += 2000;
    out = e.Apply(Body(99, 1, .3f), at(t), &ev);
    CHECK(ev == m::EmergencyEvent::kOffBody && out.tracked && out.session == 7 && out.joints[11].y == .3f);
    CHECK(identity.Update(out) == id);
    out = e.Apply(Body(99, 2, .4f), at(t + 33), &ev);
    CHECK(out.session == 7 && identity.Update(out) == id && !e.Active());
    // The body lost again, then back: that is a new body for the title (a new tracking id), as
    // without fix 107.
    out = e.Apply(NoBody(99), at(t + 66), &ev);
    identity.Update(out);
    out = e.Apply(Body(99, 3, .4f), at(t + 99), &ev);
    CHECK(out.session == 99 && identity.Update(out) != id);
  }
  // (c) Letting go ends it; the 10 s limit ends it and it waits for the buttons to be let go.
  {
    m::EmergencyPose e;
    m::EmergencyEvent ev;
    e.Apply(Body(3, 1, .5f), at(0), &ev);
    for (int t = 33; t < 4000; t += 33) e.Apply(NoBody(3), at(t), &ev);
    e.Pad(true, at(4000));
    e.Apply(NoBody(3), at(4600), &ev);
    CHECK(ev == m::EmergencyEvent::kOn);
    e.Pad(false, at(5000));
    m::Frame out = e.Apply(NoBody(3), at(5033), &ev);
    CHECK(ev == m::EmergencyEvent::kOffReleased && !out.tracked && !e.Active());
    CHECK(e.HintVisible(at(5033)));  // the hint again
    e.Pad(true, at(6000));
    e.Apply(NoBody(3), at(6600), &ev);
    CHECK(ev == m::EmergencyEvent::kOn);
    int t = 6633, events = 0;
    for (; t < 6600 + 10000 + 100; t += 33) {
      out = e.Apply(NoBody(3), at(t), &ev);
      if (ev != m::EmergencyEvent::kNone) { ++events; break; }
    }
    CHECK(events == 1 && ev == m::EmergencyEvent::kOffLimit && !out.tracked);
    CHECK(t >= 6600 + 10000 && t < 6600 + 10000 + 40);
    for (int n = 0; n < 60; ++n) {  // still held: not again
      e.Apply(NoBody(3), at(t + n * 33), &ev);
      CHECK(ev == m::EmergencyEvent::kNone && !e.Active());
    }
    e.Pad(false, at(t + 2000));
    e.Apply(NoBody(3), at(t + 2033), &ev);
    e.Pad(true, at(t + 2100));
    e.Apply(NoBody(3), at(t + 2700), &ev);
    CHECK(ev == m::EmergencyEvent::kOn);  // let go and held again: on
  }
  // (a) Not with a recent body; (b) a standing body at 2.5 m when there was none; Reset.
  {
    m::EmergencyPose e;
    m::EmergencyEvent ev;
    e.Apply(Body(5, 1, .5f), at(0), &ev);
    e.Pad(true, at(100));
    m::Frame out = e.Apply(NoBody(5), at(1000), &ev);
    CHECK(ev == m::EmergencyEvent::kNone && !out.tracked);  // the body went 1 s ago
    e.Reset();
    e.Pad(false, at(1100));
    e.Pad(true, at(1200));
    out = e.Apply(NoBody(5), at(1800), &ev);
    CHECK(ev == m::EmergencyEvent::kOn && out.tracked);
    CHECK(out.joints[0].x == 0 && out.joints[0].y == 0 && out.joints[0].z == 0);  // hips at the sensor's 2.5 m
    CHECK(out.joints[2].y > .4f && out.joints[4].x > 0 && out.joints[8].x < 0 && out.joints[14].y < -.8f);
    for (const auto& joint : out.joints) CHECK(joint.confidence == 1.0f);
    CHECK(std::string(m::EmergencyEventName(m::EmergencyEvent::kOffLimit)) == "off (10 s limit)");
  }
  CHECK(m::kEmergencyButtons == (0x02 | 0x04));

  // (f) The motion panel's text lines: one each, so the distance cannot run into the status line in
  // any language ("Motion: tracking (research)Odległość 2.7 m"); the hint under the distance while
  // it shows, all of it above the skeleton's clip (185).
  for (const bool tracked : {true, false}) {
    const auto lines = m::PanelStatusLines(tracked);
    CHECK(lines.skeleton - lines.status >= m::kStatusLineHeight - 2);
    CHECK(lines.distance - lines.skeleton >= m::kStatusLineHeight);
    if (tracked) CHECK(lines.hint - lines.distance >= m::kStatusLineHeight);
    else CHECK(lines.hint == lines.distance);
    CHECK(lines.hint + m::kStatusLineHeight <= 186 + m::kStatusLineHeight && lines.status >= 100);
  }

  // (e) A brief press asks for a re-centre; a hold, or a press that started the emergency body, does not.
  {
    m::EmergencyPose e;
    m::EmergencyEvent ev;
    CHECK(!e.Pad(true, at(0)) && e.Pad(false, at(200)));     // pressed and let go within 0.5 s
    CHECK(!e.Pad(true, at(1000)) && !e.Pad(false, at(1600)));  // held 0.6 s: not a press
    CHECK(!e.Pad(false, at(1700)));                            // nothing held
    e.Apply(Body(4, 1, .5f), at(2000), &ev);
    for (int t = 2033; t < 5500; t += 33) e.Apply(NoBody(4), at(t), &ev);
    e.Pad(true, at(5500));
    e.Apply(NoBody(4), at(6100), &ev);
    CHECK(ev == m::EmergencyEvent::kOn);
    CHECK(!e.Pad(false, at(6200)));  // the end of the hold that started it: no re-centre
  }
  {
    // The request: numbered, a 3 s countdown, in the frames' answers and the status until the
    // page says it ran it, at most 10 s past the countdown.
    const int64_t t = m::SteadyMs();  // the status reads the clock itself
    CHECK(!m::RecentrePending(t) && m::RecentreJson(t).empty());
    const uint32_t first = m::RequestRecentre(t);
    CHECK(first == 1 && m::RecentrePending(t) && m::RecentreRemainingMs(t + 1000) == 2000);
    CHECK(m::RecentreJson(t + 1000) == ",\"recentre\":1,\"recentre_in_ms\":2000");
    CHECK(m::RecentreRemainingMs(t + 5000) == 0 && m::RecentrePending(t + 5000));
    CHECK(m::Status().find("\"recentre\":1,\"recentre_in_ms\":") != std::string::npos);
    CHECK(!m::AckRecentre(0) && !m::AckRecentre(2));  // nothing, or a number not asked yet
    CHECK(m::AckRecentre(1) && !m::AckRecentre(1));    // once
    CHECK(!m::RecentrePending(t + 1000) && m::RecentreJson(t + 1000).empty());
    CHECK(m::Status().find("\"recentre\"") == std::string::npos);
    const uint32_t second = m::RequestRecentre(t + 20000);
    CHECK(second == 2 && m::RecentrePending(t + 20000 + 3000 + 10000) && !m::RecentrePending(t + 20000 + 3000 + 10001));
    // The page's report: "recentred":N.
    CHECK(xbox360ps5::phone_latency::Recentred("{\"fps\":25,\"recentred\":2}") == 2);
    CHECK(xbox360ps5::phone_latency::Recentred("{\"fps\":25,\"recentred\":0}") == 0);
    CHECK(xbox360ps5::phone_latency::Recentred("{\"fps\":25}") == 0 && xbox360ps5::phone_latency::Recentred("") == 0);
  }

  // (d) The tree.
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  const std::string pad = ReadText("platform/ps5/dualsense_input.cpp");
  const std::string depth = ReadText("include/xbox360ps5/virtual_depth_camera.hpp");
  const std::string game = ReadText("src/canary_game_main.cpp");
  if (cam.empty() || pad.empty() || game.empty()) {
    std::printf("no assembled tree here; the tree not checked\n");
  } else {
    const size_t apply = cam.find("xbox360ps5::motion::emergency_pose.Apply(\n      xbox360ps5::motion::NormalizedSnapshot(), clock::now(), &emergency);");
    const size_t update = cam.find("source.identity.Update(pose, emergency == xbox360ps5::motion::EmergencyEvent::kOn);");
    CHECK(apply != std::string::npos && update != std::string::npos && apply < update);
    CHECK(cam.find("\"Virtual Kinect: emergency pose {} (fix 107)\"") != std::string::npos);
    CHECK(cam.find("xbox360ps5::motion::emergency_pose.Reset();  // fix 107") != std::string::npos);
    CHECK(cam.find("emergency_pose.Overlay(xbox360ps5::motion::NormalizedSnapshot());") != std::string::npos);
    CHECK(depth.find("Render(emergency_pose.Overlay(NormalizedSnapshot()),depth_);") != std::string::npos);
    const size_t pad_call = pad.find("motion::emergency_pose.Pad(sample.connected && (sample.buttons & motion::kEmergencyButtons) ==");
    CHECK(pad_call != std::string::npos && pad_call < pad.find("std::lock_guard<std::mutex> lock(mutex_);", pad.find("void DualSenseInput::Submit")));
    CHECK(game.find("DrawEmergencyHint(scale, now);  // fix 107") != std::string::npos);
    CHECK(game.find("const bool recentring = xbox360ps5::motion::RecentrePending(now_ms);  // fix 107") != std::string::npos);
    // (f) The panel draws its lines where PanelStatusLines says, the distance no longer at the
    // status line's right end.
    CHECK(game.find("const auto lines = xbox360ps5::motion::PanelStatusLines(phone.tracked);") != std::string::npos);
    CHECK(game.find("ImVec2((1570 + shift_) * scale, lines.distance * scale),") != std::string::npos);
    CHECK(game.find("DrawPositionHint(draw, scale, pose, lines.hint);") != std::string::npos);
    CHECK(game.find("(1885 + shift_) * scale - size.x, 117 * scale)") == std::string::npos);
    // The pad's press, the server's answers and the acknowledgement, with their log lines.
    CHECK(pad.find("\"Virtual Kinect: re-centre requested from the pad (seq {}) (fix 107)\"") != std::string::npos);
    CHECK(pad.find("motion::enabled.load() && motion::game_running.load()) {") != std::string::npos);
    const std::string tls = ReadText("port/phone_camera_tls.cpp");
    CHECK(tls.find("\"{\\\"ok\\\":true\" + motion::RecentreJson(motion::SteadyMs()) + \"}\"") != std::string::npos);
    CHECK(tls.find("if (const uint32_t seq = phone_latency::Recentred(body); motion::AckRecentre(seq))") != std::string::npos);
    CHECK(tls.find("\"Virtual Kinect: re-centre requested from the pad (seq {}); phone acknowledged (fix 107)\"") != std::string::npos);
    // Fix 106's reasons line now under the 1.5 s limit with the stages line.
    CHECK(tls.find("if (phone_latency::Due(phone_latency::NowMs())) {") != std::string::npos);
    for (const char* key : {"No player seen. Hold L3 + R3 to bring the last pose back, or press them briefly to re-centre the phone camera from here. Stand 2.5-3 m from the phone with your hips in view.",
                            "Emergency pose on: the game sees the last pose. Let go of L3 + R3 to stop; a body from the phone takes over.",
                            "Re-centring the phone camera in %d... Stand still at your spot.", "Re-centring the phone camera... Stand still at your spot."})
      CHECK(game.find(std::string("Tr(\"") + key + "\")") != std::string::npos);
    std::printf("tree: the skeleton read, depth camera, pad and overlay use the emergency pose\n");
  }
  // The overlay's two texts in every language file (the argument: tools/source-1.0.2/english/lang),
  // as rekey.py makes them from extra.txt: keys and values with "\s" for spaces.
  if (argc < 2) {
    std::printf("no language folder given; the texts not checked\n");
  } else {
    auto escaped = [](std::string text) {
      std::string out;
      for (const char c : text) out += c == ' ' ? std::string("\\s") : std::string(1, c);
      return out;
    };
    const std::string keys[] = {
        escaped("No player seen. Hold L3 + R3 to bring the last pose back, or press them briefly to re-centre the phone camera from here. Stand 2.5-3 m from the phone with your hips in view.") + " = ",
        escaped("Emergency pose on: the game sees the last pose. Let go of L3 + R3 to stop; a body from the phone takes over.") + " = ",
        escaped("Re-centring the phone camera in %d... Stand still at your spot.") + " = ",
        escaped("Re-centring the phone camera... Stand still at your spot.") + " = "};
    int files = 0;
    for (const char* code : {"de", "en", "es-419", "es", "fr", "it", "ja", "ko", "nl", "pl", "pt-BR", "pt", "ru", "tr",
                             "zh-Hans", "zh-Hant"}) {
      const std::string lang = ReadText((std::string(argv[1]) + "/" + code + ".txt").c_str());
      bool all = true;
      for (const auto& key : keys) all = all && lang.find(key) != std::string::npos;
      if (!all) std::printf("missing in %s.txt\n", code);
      files += all;
    }
    CHECK(files == 16);
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-emergency: all passed\n");
  return 0;
}
