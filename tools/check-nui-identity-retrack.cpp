// SPDX-License-Identifier: MIT
// Fix 156w (tools/source-1.0.2/fixes/156w-nui-identity-retrack.patch; #75): a body tracked again
// after the title has its player is that player again.
//
//   check-nui-identity-retrack        run from an assembled tree's root
//
// On 1.0.7_integration1 a gap in the phone's frames dropped the body in Kinect Sports (Football)
// and Fighters Uncaged (TrainingHall); it came back as body 2, which fix 156t kept as "call
// identify" (enrollment FFFFFFFF, user FE) until the game ended, since those titles identify
// their player once and never ask again. A model of the core's skeleton read
// (xboxkrnl_pscam.cc: SkeletonIdentity, Unidentified, ForFrame, then the answers due and
// MarkIdentified), one frame every 33 ms, with three kinds of title:
//  (a) Kinect Sports / Fighters Uncaged (they ask for the first unidentified body once): body 1
//      starts unidentified, is identified; a short gap keeps body 1 (fix 169, when it is in the
//      tree) and its enrollment; after a long gap body 2 is "call identify" for at most 1.5 s
//      and then the profile's (enrollment 0, user 0) to the end, with no new request;
//  (b) Zumba (asks every frame for an unidentified player): body 1 starts unidentified until its
//      answer; body 2 after a long gap is asked about and stays unidentified until its own
//      answer, also when that takes longer than 1.5 s;
//  (c) a title that never asks (156t's 20 s fallback for the first body, unchanged), then a new
//      body is the player after 1.5 s; Reset forgets the player.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "xbox360ps5/nui_identity.hpp"
#include "xbox360ps5/virtual_skeleton.hpp"
#if __has_include("xbox360ps5/body_identity.hpp")
#define HAVE_FIX_169 1  // SkeletonIdentity keeps a body's id through a gap of up to 2 s
#else
#define HAVE_FIX_169 0
#endif

namespace ni = xbox360ps5::nui_identity;
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

enum class Title { kAsksOnce, kAsksEveryFrame, kNeverAsks };

struct Model {
  Title title;
  m::SkeletonIdentity identity;
  Clock::time_point t0 = Clock::now();
  int64_t ms = 0;
  bool asked_once = false;
  uint32_t requests = 0;
  uint32_t delay_frames = 0;  // Zumba model: the title stops asking while its answer is due
  struct Seen { uint32_t id, enrollment, user; };
  std::vector<Seen> frames;

  explicit Model(Title t) : title(t) { ni::Reset(); ni::identify_bound = true; }

  // One skeleton read: with a body or not.
  Seen Frame(bool body) {
    m::Frame pose;
    pose.session = 7;
    pose.sequence = uint32_t(ms / 33 + 1);
    pose.tracked = body;
#if HAVE_FIX_169
    const uint32_t id = identity.Update(pose, false, t0 + std::chrono::milliseconds(ms));
#else
    const uint32_t id = identity.Update(pose);
#endif
    const bool unidentified = ni::identify_bound.load() && pose.tracked && ni::Unidentified(id, ms);
    const auto shown = ni::ForFrame(true, unidentified, 0, 0);  // the binding: enrollment 0, user 0
    Seen seen{id, body ? shown.enrollment : ni::kEnrollmentUnknown, body ? shown.user_index : ni::kUserIndexNone};
    // The title reads the frame and may ask about a body that says "call identify".
    if (body && seen.enrollment == ni::kEnrollmentUnknown) {
      const bool ask = title == Title::kAsksEveryFrame || (title == Title::kAsksOnce && !asked_once);
      if (ask && ni::Identify(id, 0, 0x82000000, 0x40000000, id) == ni::kPending) {
        ++requests;
        asked_once = true;
      }
    }
    // The answers due (DeliverIdentityAnswers), each body marked identified (fix 156t).
    for (const auto& r : ni::TakeDue()) ni::MarkIdentified(r.tracking_id);
    frames.push_back(seen);
    ms += 33;
    return seen;
  }
  void Run(bool body, int64_t for_ms) {
    for (const int64_t end = ms + for_ms; ms < end;) Frame(body);
  }
  // How long (ms) the frames since `from` said "call identify" for a body.
  int64_t UnidentifiedMs(size_t from) const {
    int64_t n = 0;
    for (size_t i = from; i < frames.size(); ++i)
      if (frames[i].id && frames[i].enrollment == ni::kEnrollmentUnknown) n += 33;
    return n;
  }
};

static void AsksOnce(const char* name, int64_t short_gap_ms) {
  std::printf("  %s\n", name);
  Model t(Title::kAsksOnce);
  t.Run(true, 2000);
  const auto first = t.frames.front();
  CHECK(first.id == 1 && first.enrollment == ni::kEnrollmentUnknown && first.user == ni::kUserIndexNone);
  CHECK(t.requests == 1);
  CHECK(t.frames.back().id == 1 && t.frames.back().enrollment == 0 && t.frames.back().user == 0);
  // A short gap (the logs: the phone's last frame 516 / 557 ms before the body was dropped).
  t.Run(false, short_gap_ms);
  size_t mark = t.frames.size();
  t.Run(true, 3000);
#if HAVE_FIX_169
  CHECK(t.frames[mark].id == 1);  // fix 169: the same body
  CHECK(t.UnidentifiedMs(mark) == 0);
#else
  CHECK(t.UnidentifiedMs(mark) <= ni::kReaskWithinMs);
#endif
  CHECK(t.frames.back().enrollment == 0 && t.frames.back().user == 0);
  // A long gap: body 2. 156t kept it unidentified to the end; 156w gives it the profile after 1.5 s.
  t.Run(false, 4000);
  mark = t.frames.size();
  t.Run(true, 70000);
  CHECK(t.frames[mark].id != 1 && t.frames[mark].id != 0);
  CHECK(t.UnidentifiedMs(mark) > 0 && t.UnidentifiedMs(mark) <= ni::kReaskWithinMs + 33);
  CHECK(t.frames.back().enrollment == 0 && t.frames.back().user == 0);
  CHECK(t.requests == 1);  // the title did not ask again, and did not need to
  // And a third body after another long gap: the same.
  t.Run(false, 5000);
  mark = t.frames.size();
  t.Run(true, 3000);
  CHECK(t.UnidentifiedMs(mark) <= ni::kReaskWithinMs + 33 && t.frames.back().enrollment == 0);
}

static void Zumba() {
  std::printf("  Zumba Fitness (asks every frame)\n");
  Model z(Title::kAsksEveryFrame);
  z.Run(true, 3000);
  CHECK(z.frames.front().enrollment == ni::kEnrollmentUnknown);  // the unidentified start
  CHECK(z.requests >= 1);
  const int64_t start = z.UnidentifiedMs(0);
  CHECK(start >= int64_t(ni::kAnswerDelayFrames) * 33 - 33 && start < 1000);  // until its answer
  CHECK(z.frames.back().enrollment == 0);
  // Body 2 after a long gap: Zumba asks, and waits for its own answer.
  z.Run(false, 4000);
  const size_t mark = z.frames.size();
  const uint32_t before = z.requests;
  z.Run(true, 3000);
  CHECK(z.frames[mark].id != 1 && z.frames[mark].enrollment == ni::kEnrollmentUnknown);
  CHECK(z.requests > before);  // it asked about body 2 itself
  CHECK(z.frames.back().enrollment == 0);
  // A request still on its way holds "call identify" past 1.5 s (an answer due later).
  ni::Reset();
  ni::MarkIdentified(1);  // the title has its player
  CHECK(ni::Unidentified(5, 0));
  CHECK(ni::Identify(5, 0, 0x82000000, 0x40000000, 5) == ni::kPending);
  CHECK(ni::Unidentified(5, 2000));  // asked about: not yet the player
  for (uint32_t n = 0; n < ni::kAnswerDelayFrames; ++n)
    for (const auto& r : ni::TakeDue()) ni::MarkIdentified(r.tracking_id);
  CHECK(!ni::Unidentified(5, 2100));  // answered
}

static void NeverAsks() {
  std::printf("  a title that never asks\n");
  Model d(Title::kNeverAsks);
  d.Run(true, 25000);
  const int64_t first = d.UnidentifiedMs(0);
  CHECK(first >= ni::kAskWithinMs - 33 && first <= ni::kAskWithinMs + 66);  // 156t's fallback
  CHECK(d.frames.back().enrollment == 0);
  d.Run(false, 4000);
  const size_t mark = d.frames.size();
  d.Run(true, 3000);
  CHECK(d.UnidentifiedMs(mark) <= ni::kReaskWithinMs + 33 && d.frames.back().enrollment == 0);
  // Reset (a new binding): the next title's first body starts unidentified again.
  ni::Reset();
  CHECK(ni::Unidentified(9, 100000));
  CHECK(ni::Unidentified(9, 100000 + ni::kReaskWithinMs + 1000));
  CHECK(!ni::Unidentified(0, 0));
}

int main() {
  AsksOnce("Kinect Sports (asks once; a 516 ms gap, then a long one)", 600);
  AsksOnce("Fighters Uncaged (asks once; a 557 ms gap, then a long one)", 700);
  Zumba();
  NeverAsks();
  if (failures) {
    std::printf("check-nui-identity-retrack: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-nui-identity-retrack: ok\n");
  return 0;
}
