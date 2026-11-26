// SPDX-License-Identifier: MIT
// Fix 183 (tools/source-1.0.2/fixes/183-kinect-ghost.patch; two-player Kinect probe,
// research/design-v108/kinect-2-players.md step 1): the "ghost player". With the switch file
// kinect-ghost.txt in the data root on, slot 1 of every skeleton frame carries a copy of
// the player's pose from delay-s earlier, offset-m to the side, under its own tracking ID, as a
// guest. Without the file nothing changes.
//
//   check-kinect-ghost        run from an assembled tree's root
//
//  (a) the switch: off without the file or without "on"; offset and delay parsed and clamped;
//  (b) the delay buffer: the newest pose at least `delay` old, nothing before that, the ring;
//  (c) the offset: every joint the phone sent, not the absent ones;
//  (d) the frame: slot 1 with the ghost's ID, state and guest pair, slot 0 and the header as
//      WriteSkeletonFrame wrote them; an untracked ghost leaves slot 1 empty;
//  (e) the ghost's identity: -1/FE until answered, or 20 s without being asked; -2/FE after;
//      -1/FE in a title whose identify is not answered here; the player's 156t state untouched
//      by a ghost request;
//  (f) the tree: the switch read at binding, slot 1 only with it on, the ghost's identify and
//      guest answer, the 156s line with both bodies, the every-call diag, the reset.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/kinect_ghost.hpp"
#include "xbox360ps5/nui_identity.hpp"
#include "xbox360ps5/virtual_skeleton.hpp"

namespace kg = xbox360ps5::kinect_ghost;
namespace mo = xbox360ps5::motion;
namespace ni = xbox360ps5::nui_identity;

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
  std::string out;
  for (char c : s.str())
    if (c != '\r') out += c;
  return out;
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}
static uint32_t LoadBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

static mo::Frame Pose(float marker) {
  mo::Frame f;
  f.tracked = true;
  f.session = 7;
  for (uint32_t n = 0; n < mo::kJointCount; ++n) {
    f.joints[n].x = marker + 0.01f * float(n);
    f.joints[n].y = 0.1f * float(n % 5);
    f.joints[n].z = 0.0f;
    f.joints[n].confidence = 0.9f;
  }
  return f;
}

static void CheckSwitch() {
  CHECK(!kg::Parse("").on);
  CHECK(!kg::Parse("offset-m 1.5\ndelay-s 2\n").on);  // no "on"
  kg::Config c = kg::Parse("on\n");
  CHECK(c.on && c.offset_m == kg::kDefaultOffset && c.delay_s == kg::kDefaultDelay);
  c = kg::Parse("# the ghost\non   # yes\noffset-m 0.8\r\ndelay-s 1.5\nnoise 9\n");
  CHECK(c.on && c.offset_m == 0.8f && c.delay_s == 1.5f);
  c = kg::Parse("on\noffset-m 9\ndelay-s -3\n");
  CHECK(c.offset_m == kg::kOffsetMax && c.delay_s == kg::kDelayMin);
  c = kg::Parse("on\noffset-m\ndelay-s x\n");  // no value: defaults
  CHECK(c.offset_m == kg::kDefaultOffset && c.delay_s == kg::kDefaultDelay);
  CHECK(!kg::Load("no-such-dir/kinect-ghost.txt").on);
  {
    std::ofstream out("build-kinect-ghost-test.txt", std::ios::binary);
    out << "on\noffset-m 1.25\n";
  }
  c = kg::Load("build-kinect-ghost-test.txt");
  CHECK(c.on && c.offset_m == 1.25f && c.delay_s == kg::kDefaultDelay);
  std::remove("build-kinect-ghost-test.txt");
  CHECK(std::string(kg::kSwitchPath) == XBOX360PS5_DATA_ROOT "/kinect-ghost.txt");
}

static void CheckDelay() {
  kg::DelayBuffer buffer;
  CHECK(!buffer.Delayed(1000, 3000).tracked);  // nothing yet
  for (int64_t t = 0; t <= 5000; t += 33) buffer.Push(Pose(float(t)), t);
  mo::Frame d = buffer.Delayed(5000, 3000);
  CHECK(d.tracked && d.joints[0].x == 1980.0f);  // the newest at or before 2000 ms
  CHECK(!buffer.Delayed(2000, 3000).tracked);     // not 3 s of history yet at 2 s
  CHECK(buffer.Delayed(5000, 0).joints[0].x == 4983.0f);
  // An untracked pose (the player gone) comes back untracked, 3 s later.
  buffer.Push(mo::Frame{}, 5016);
  CHECK(!buffer.Delayed(8016, 3000).tracked);
  // The ring: at most kDelayFrames poses, the oldest dropped.
  kg::DelayBuffer ring;
  for (int64_t n = 0; n < int64_t(kg::kDelayFrames) + 100; ++n) ring.Push(Pose(float(n)), n * 33);
  CHECK(ring.size() == kg::kDelayFrames);
  CHECK(!ring.Delayed(99 * 33, 0).tracked);  // pose 99 was dropped
  CHECK(ring.Delayed(100 * 33, 0).joints[0].x == 100.0f);
}

static void CheckOffset() {
  mo::Frame p = Pose(0.0f);
  p.joints[3].confidence = 0.0f;  // absent
  const mo::Frame s = kg::Shifted(p, 1.1f);
  CHECK(s.joints[0].x == p.joints[0].x + 1.1f && s.joints[1].x == p.joints[1].x + 1.1f);
  CHECK(s.joints[3].x == p.joints[3].x);
  CHECK(s.joints[5].y == p.joints[5].y && s.joints[5].z == p.joints[5].z);
}

static void CheckFrame() {
  std::vector<uint8_t> plain(mo::kSkeletonFrameBytes), with(mo::kSkeletonFrameBytes);
  const mo::Frame player = Pose(0.0f);
  mo::WriteSkeletonFrame(plain.data(), player, 42, 1386, 1, 0, 0);
  mo::WriteSkeletonFrame(with.data(), player, 42, 1386, 1, 0, 0);
  mo::WriteSkeletonBody(with.data(), kg::kGhostSlot, kg::Shifted(Pose(0.5f), 1.1f),
                        kg::kGhostTrackingId, kg::kGuestEnrollment, kg::kUserIndexNone);
  const size_t slot1 = mo::kSkeletonDataOffset + kg::kGhostSlot * mo::kSkeletonDataBytes;
  // Header and slot 0 untouched.
  CHECK(std::memcmp(plain.data(), with.data(), slot1) == 0);
  CHECK(std::memcmp(plain.data() + slot1 + mo::kSkeletonDataBytes,
                    with.data() + slot1 + mo::kSkeletonDataBytes,
                    mo::kSkeletonFrameBytes - slot1 - mo::kSkeletonDataBytes) == 0);
  const uint8_t* b = with.data() + slot1;
  CHECK(LoadBE(b + 0x00) == mo::kSkeletonTracked && LoadBE(b + 0x04) == kg::kGhostTrackingId);
  CHECK(LoadBE(b + 0x08) == kg::kGuestEnrollment && LoadBE(b + 0x0C) == kg::kUserIndexNone);
  CHECK(LoadBE(b + 0x160) == mo::kJointTracked);
  // The ghost stands 1.1 m to the side of the player: sensor x = -phone x.
  float px, py, pz, gx, gy, gz;
  std::memcpy(&px, &plain[mo::kSkeletonDataOffset + 0x10], 4);
  uint32_t bits = LoadBE(with.data() + mo::kSkeletonDataOffset + 0x10);
  std::memcpy(&px, &bits, 4);
  bits = LoadBE(b + 0x10);
  std::memcpy(&gx, &bits, 4);
  (void)py, (void)pz, (void)gy, (void)gz;
  CHECK(gx < px - 1.0f && gx > px - 2.0f);
  // Slot 0 keeps its tracking ID 1, slot 1 the ghost's; every frame the same.
  CHECK(LoadBE(with.data() + mo::kSkeletonDataOffset + 4) == 1);
  // An untracked ghost: slot 1 empty, "unknown" as WriteSkeletonFrame leaves it.
  std::vector<uint8_t> gone = plain;
  mo::WriteSkeletonBody(gone.data(), kg::kGhostSlot, mo::Frame{}, kg::kGhostTrackingId,
                        kg::kGuestEnrollment, kg::kUserIndexNone);
  CHECK(gone == plain);
  // The -1/FE pair (call identify) is written as given too.
  std::vector<uint8_t> call = plain;
  mo::WriteSkeletonBody(call.data(), kg::kGhostSlot, Pose(0.5f), kg::kGhostTrackingId,
                        kg::kEnrollmentCallIdentify, kg::kUserIndexNone);
  CHECK(LoadBE(call.data() + slot1 + 0x08) == 0xFFFFFFFF);
}

static void CheckIdentity() {
  kg::Identity id;
  auto p = kg::ForFrame(false, id, 0);
  CHECK(p.enrollment == 0xFFFFFFFF && p.user_index == 0xFE);  // not bound: no identify step
  id.first_seen_ms = 1000;
  p = kg::ForFrame(true, id, 1000 + kg::kGhostAskWithinMs - 1);
  CHECK(p.enrollment == 0xFFFFFFFF);  // call identify
  p = kg::ForFrame(true, id, 1000 + kg::kGhostAskWithinMs);
  CHECK(p.enrollment == kg::kGuestEnrollment && p.user_index == 0xFE);  // never asked: a guest
  id.asked = true;
  p = kg::ForFrame(true, id, 1000 + 10 * kg::kGhostAskWithinMs);
  CHECK(p.enrollment == 0xFFFFFFFF);  // asked, not answered yet
  id.answered = true;
  p = kg::ForFrame(true, id, 1001);
  CHECK(p.enrollment == kg::kGuestEnrollment);
  CHECK(kg::ForFrame(true, kg::Identity{}, 1u << 30).enrollment == 0xFFFFFFFF);  // never seen
  // A ghost request does not count as "the title asked" for the player (156t's fallback).
  ni::Reset();
  CHECK(ni::Identify(kg::kGhostTrackingId, 0, 0x82331FE8, 0, kg::kGhostTrackingId, 0, false) ==
        ni::kPending);
  CHECK(ni::Unidentified(1, 0));
  CHECK(!ni::Unidentified(1, ni::kAskWithinMs));  // the player still falls back after 20 s
  ni::Reset();
  CHECK(ni::Identify(1, 0, 0x82331FE8, 0, 1) == ni::kPending);  // the player: as before
  CHECK(ni::Unidentified(1, 0));
  CHECK(ni::Unidentified(1, 10 * ni::kAskWithinMs));  // asked: no fallback
  ni::Reset();
}

static void CheckTree() {
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  const std::string shim = ReadText(".deps/xenia-canary/src/xenia/kernel/util/shim_utils.h");
  if (cam.empty() || shim.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_pscam.cc, shim_utils.h)\n");
    ++failures;
    return;
  }
  CHECK(Has(cam, "std::atomic<bool> g_ghost_on{false};"));
  CHECK(Has(cam, "g_ghost = xbox360ps5::kinect_ghost::Load();"));
  CHECK(Has(cam, "g_ghost_on = g_ghost.on;"));
  // Slot 1 only with the switch on, after slot 0 is written.
  const size_t slot0 = cam.find("      tracking_id, identity.enrollment, identity.user_index);");
  const size_t slot1 = cam.find("  if (g_ghost_on.load()) WriteGhost(memory->TranslateVirtual(frame_ptr), pose, bound_identify);");
  CHECK(slot0 != std::string::npos && slot1 != std::string::npos && slot0 < slot1);
  CHECK(Has(cam, "xbox360ps5::motion::WriteSkeletonBody(frame, kg::kGhostSlot, ghost, kg::kGhostTrackingId,"));
  // The ghost's identify: accepted, its own state, a guest's answer.
  CHECK(Has(cam, "xbox360ps5::nui_identity::AllowedFlags(layout), false);  // not the player's request"));
  // The player's identify call as it was (check-nui-identity reads it too).
  CHECK(Has(cam, "      xbox360ps5::nui_identity::AllowedFlags(layout));  // fix 156q"));
  const size_t ghost_call = cam.find("if (ghost) {\n    const uint32_t result = xbox360ps5::nui_identity::Identify(");
  const size_t player_call = cam.find("      xbox360ps5::nui_identity::AllowedFlags(layout));  // fix 156q");
  CHECK(ghost_call != std::string::npos && ghost_call < player_call);
  CHECK(Has(cam, "g_ghost_identity.asked = true;"));
  CHECK(Has(cam, "guest.enrollment = xbox360ps5::kinect_ghost::kGuestEnrollment;"));
  CHECK(Has(cam, "g_ghost_identity.answered = true;"));
  CHECK(Has(cam, "if (!IsGuestRangeMapped(memory, r.callback, 4, false) || GhostAnswered(r, message)) continue;"));
  CHECK(Has(cam, "if (!g_ghost_on.load() || r.tracking_id != xbox360ps5::kinect_ghost::kGhostTrackingId)\n    return false;"));
  CHECK(Has(cam, "our bodies \"\n           \"{:08X} {:08X} (fix 156s, 183)\""));
  CHECK(Has(cam, "xbox360ps5::kinect_diag::every_call = g_ghost.on;"));
  CHECK(Has(shim, "if (kd::EveryCall(name)) {  // fix 183: with the ghost on"));
  CHECK(Has(cam, "    g_ghost_identity = {};\n  }\n  g_ghost_live = false;\n"));
}

int main() {
  CheckSwitch();
  CheckDelay();
  CheckOffset();
  CheckFrame();
  CheckIdentity();
  CheckTree();
  if (failures) {
    std::printf("check-kinect-ghost: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-ghost: ok\n");
  return 0;
}
