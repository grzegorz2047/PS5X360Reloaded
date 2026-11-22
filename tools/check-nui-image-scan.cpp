// Fix 57: the virtual Kinect finds what it needs in a title's executable by the shape of
// its code (include/xbox360ps5/nui_image_scan.hpp), not by addresses traced in one image.
//
//   check-nui-image-scan                     synthetic code, XEX headers, and (run from an
//                                            assembled tree's root) the tree's core files
//   check-nui-image-scan --image <decoded.bin> <base> <callback> [<default.xex>]
//                                            a real executable as loaded (xex_decode.py's
//                                            output); prints what is found, fails when the
//                                            callback, skeleton call or Kinect flag is not
//
// No game code is part of this check; the real-image mode reads files given on the
// command line only.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/nui_image_scan.hpp"
#include "xbox360ps5/virtual_skeleton.hpp"
#include "xbox360ps5/body_scale.hpp"
#include "xbox360ps5/kinect_hand_view.hpp"

namespace m = xbox360ps5::motion;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

// A little PowerPC assembler for the synthetic images.
struct Image {
  uint32_t base = 0x82000000;
  std::vector<uint8_t> bytes = std::vector<uint8_t>(0x4000, 0);
  void Put(uint32_t at, uint32_t w) {
    const size_t o = at - base;
    bytes[o] = uint8_t(w >> 24); bytes[o + 1] = uint8_t(w >> 16);
    bytes[o + 2] = uint8_t(w >> 8); bytes[o + 3] = uint8_t(w);
  }
  m::CodeView View() const { return {bytes.data(), bytes.size(), base}; }
};
uint32_t D(unsigned op, unsigned rd, unsigned ra, int32_t imm) {
  return op << 26 | rd << 21 | ra << 16 | (uint32_t(imm) & 0xFFFF);
}
uint32_t Lis(unsigned rd, uint16_t imm) { return D(15, rd, 0, int16_t(imm)); }
uint32_t Addi(unsigned rd, unsigned ra, int32_t imm) { return D(14, rd, ra, imm); }
uint32_t Bl(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC) | 1; }
uint32_t B(uint32_t from, uint32_t to) { return 18u << 26 | ((to - from) & 0x03FFFFFC); }
uint32_t Mr(unsigned ra, unsigned rs) { return 31u << 26 | rs << 21 | ra << 16 | rs << 11 | 444u << 1; }
uint32_t Lwz(unsigned rd, unsigned ra, int32_t d) { return D(32, rd, ra, d); }
uint32_t Stw(unsigned rs, unsigned ra, int32_t d) { return D(36, rs, ra, d); }

// The identity callback as the Kinect SDK compiles it (see nui_image_scan.hpp), at
// 82001000; its table at 82003000; the 2C00F case at 82001100 calls a refresh at 82002000.
Image IdentityImage(bool refresh_reads_buffer, uint32_t max = 0x15, bool case_is_call = true) {
  Image image;
  const uint32_t cb = 0x82001000, table = 0x82003000, cases = 0x82001080;
  uint32_t at = cb;
  for (uint32_t w : {m::kMflrR12, Stw(12, 1, -8), 0x9421FF40u /* stwu */, Mr(31, 4),
                     D(15, 11, 3, -3), Addi(11, 11, 0x3FFE),
                     D(10, 6 << 2, 11, int32_t(max)) /* cmplwi cr6,r11,max */,
                     0x41990100u /* bgt */, Lis(12, 0x8200), Addi(12, 12, 0x3000),
                     0x7C0C58AEu /* lbzx r0,r12,r11 */, 0x5400103Au /* slwi r0,r0,2 */,
                     Lis(12, 0x8200), 0x60000000u /* nop */, Addi(12, 12, 0x1080),
                     0x7D8C0214u /* add */, 0x7D8903A6u /* mtctr */, m::kBctr}) {
    image.Put(at, w);
    at += 4;
  }
  // Index of 2C00F: 2C00F - 30000 + 3FFE = 0xD -> case 8 (82001080 + 0x20).
  for (uint32_t i = 0; i < 0x16; ++i) image.bytes[table - image.base + i] = 0;
  image.bytes[table - image.base + 0xD] = 8;
  const uint32_t event = cases + 8 * 4;
  image.Put(cases, Addi(3, 0, 0));
  image.Put(cases + 4, m::kBlr);
  const uint32_t refresh = 0x82002000, helper = 0x82002800;
  image.Put(event, case_is_call ? Bl(event, refresh) : Lwz(3, 31, 0));
  image.Put(event + 4, B(event + 4, cases));
  // __savegprlr: stores relative to r1, then blr.
  image.Put(helper, 0xF9C1FF68u /* std r14,-0x98(r1) */);
  image.Put(helper + 4, Stw(12, 1, -8));
  image.Put(helper + 8, m::kBlr);
  at = refresh;
  for (uint32_t w : {m::kMflrR12, Bl(refresh + 4, helper), 0x9421FF50u,
                     refresh_reads_buffer ? Lwz(5, 4, 0) : Addi(3, 1, 0x50),
                     Bl(refresh + 16, 0x82002400), Addi(4, 1, 0x54), m::kBlr}) {
    image.Put(at, w);
    at += 4;
  }
  image.Put(0x82002400, m::kBlr);
  return image;
}

void CheckIdentitySynthetic() {
  const Image good = IdentityImage(false);
  auto result = m::CheckIdentityCallback(good.View(), 0x82001000);
  CHECK(result.ok);
  CHECK(result.event_case == 0x820010A0);
  CHECK(result.refresh == 0x82002000);
  // A refresh that reads the buffer (r4) is not given a null one.
  result = m::CheckIdentityCallback(IdentityImage(true).View(), 0x82001000);
  CHECK(!result.ok);
  CHECK(std::strstr(result.why, "buffer") != nullptr);
  // The switch's range must hold the identity-changed event.
  CHECK(!m::CheckIdentityCallback(IdentityImage(false, 0x0C).View(), 0x82001000).ok);
  // The case must be a single call.
  CHECK(!m::CheckIdentityCallback(IdentityImage(false, 0x15, false).View(), 0x82001000).ok);
  // Not a function start, misaligned, outside the image.
  CHECK(!m::CheckIdentityCallback(good.View(), 0x82001004).ok);
  CHECK(!m::CheckIdentityCallback(good.View(), 0x82001002).ok);
  CHECK(!m::CheckIdentityCallback(good.View(), 0x81000000).ok);
  CHECK(!m::CheckIdentityCallback(good.View(), 0x82003FFC).ok);
  // A function without the switch.
  Image plain;
  plain.Put(0x82000100, m::kMflrR12);
  plain.Put(0x82000104, m::kBlr);
  CHECK(!m::CheckIdentityCallback(plain.View(), 0x82000100).ok);
}

void CheckRegisterUse() {
  CHECK(m::ReadsRegister(Lwz(5, 4, 0), 4));
  CHECK(!m::ReadsRegister(Lwz(4, 5, 0), 4));
  CHECK(m::WritesRegister(Lwz(4, 5, 0), 4));
  CHECK(m::ReadsRegister(Mr(30, 4), 4));      // mr r30,r4 keeps the buffer
  CHECK(!m::ReadsRegister(Mr(4, 3), 4));      // mr r4,r3 replaces it
  CHECK(m::WritesRegister(Mr(4, 3), 4));
  CHECK(m::ReadsRegister(Stw(4, 1, 0x50), 4));
  CHECK(!m::ReadsRegister(Addi(4, 0, 1), 4));  // li r4,1
  CHECK(m::WritesRegister(Addi(4, 0, 1), 4));
  CHECK(m::ReadsRegister(Addi(3, 4, 8), 4));
  CHECK(m::ReadsRegister(0x7C632214u /* add r3,r3,r4 */, 4));
}

void CheckXexHeader() {
  std::vector<uint8_t> h(0x60, 0);
  auto put = [&h](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) h[at + i] = uint8_t(v >> (24 - 8 * i)); };
  put(0, 0x58455832);
  put(0x14, 2);
  put(0x18, 0x00040006); put(0x1C, 0x40);
  put(0x20, 0x00030100); put(0x24, 0x40);
  CHECK(m::XexHeaderRequiresKinect(h.data(), h.size()));
  put(0x24, 0x80);  // supported only (optional Kinect)
  CHECK(!m::XexHeaderRequiresKinect(h.data(), h.size()));
  put(0x24, 0xC0);
  CHECK(m::XexHeaderRequiresKinect(h.data(), h.size()));
  put(0x20, 0x00030000);  // the other system flags word, not the 32 one
  CHECK(!m::XexHeaderRequiresKinect(h.data(), h.size()));
  put(0x20, 0x00030100); put(0x14, 1);  // header count excludes it
  CHECK(!m::XexHeaderRequiresKinect(h.data(), h.size()));
  put(0, 0x58455831);
  CHECK(!m::XexHeaderRequiresKinect(h.data(), h.size()));
  CHECK(!m::XexHeaderRequiresKinect(h.data(), 20));
  CHECK(m::SkeletalTrackingRequired(true, 0x40) && !m::SkeletalTrackingRequired(false, 0x40));
}

void CheckHandSeamAndCalls() {
  Image image;
  image.Put(0x82000200, Lis(11, 0x820E));
  image.Put(0x82000204, 0xC01E2710u);  // lfs f0,0x2710(r30)
  image.Put(0x82000300, Lis(11, 0x820E));
  image.Put(0x82000304, 0xC01E2714u);  // another offset: not the seam
  auto seam = m::FindHandSeam(image.View());
  CHECK(seam.matches == 1 && seam.site == 0x82000200);
  CHECK(seam.r11 == 0xFFFFFFFF820E0000ull);
  {
    // The thresholds after the seam, as DBZ forms them: addi r29,r11,0x2C10 (82002C10 in
    // this image of 82000000-82003FFF; the real one is 0x820E6C10), then lfs from it.
    Image t;
    auto put_float = [&t](uint32_t at, float value) {
      uint32_t bits;
      std::memcpy(&bits, &value, 4);
      t.Put(at, bits);
    };
    t.Put(0x82000200, Lis(11, 0x8200));
    t.Put(0x82000204, 0xC01E2710u);
    t.Put(0x82000208, Mr(5, 26));
    t.Put(0x8200020C, Addi(29, 11, 0x2C10));
    t.Put(0x82000210, D(48, 11, 29, 0x9C));   // lfs f11,0x9C(r29): reach
    t.Put(0x82000214, D(48, 12, 29, 0x610));  // lfs f12,0x610(r29): -direction
    t.Put(0x8200021C, D(48, 13, 29, 0x60C));  // lfs f13,0x60C(r29): +direction
    put_float(0x82002C10 + 0x9C, 0.4f);
    put_float(0x82002C10 + 0x610, -0.7071f);
    put_float(0x82002C10 + 0x60C, 0.7071f);
    const auto tseam = m::FindHandSeam(t.View());
    CHECK(tseam.site == 0x82000200);
    const auto th = m::FindHandThresholds(t.View(), tseam);
    CHECK(th.found && th.reach == 0.4f && th.direction == 0.7071f);
    // Values that make no sense are not taken (the DBZ defaults stay).
    put_float(0x82002C10 + 0x9C, 40.0f);
    CHECK(!m::FindHandThresholds(t.View(), tseam).found);
    CHECK(!m::FindHandThresholds(t.View(), m::HandSeam{}).found);
  }
  image.Put(0x82000400, Lis(11, 0x7FFF));
  image.Put(0x82000404, 0xC1BE2710u);  // lfs f13,0x2710(r30): a second seam
  seam = m::FindHandSeam(image.View());
  CHECK(seam.matches == 2 && seam.site == 0);
  image.Put(0x82000500, Bl(0x82000500, 0x82000800));
  image.Put(0x82000600, B(0x82000600, 0x82000800));  // a jump, not a call
  const auto calls = m::CallsOf(image.View(), 0x82000800);
  CHECK(calls.size() == 1 && calls[0] == 0x82000500);
}

// body_scale.hpp: the phone's body brought to an adult's size, arms rebuilt from the shoulder.
m::Frame Body(float size, bool bent) {
  // A standing body facing the sensor, hips at the origin, `size` times an adult's (0.52 m torso).
  m::Frame pose;
  pose.tracked = true;
  pose.session = 7;
  pose.sequence = 1;
  for (auto& j : pose.joints) j = {0, 0, 0, 0.9f};
  auto at = [&](int n, float x, float y, float z) { pose.joints[size_t(n)] = {x * size, y * size, z * size, 0.9f}; };
  at(0, 0, 0, 0); at(1, 0, 0.26f, 0); at(2, 0, 0.52f, 0); at(3, 0, 0.75f, 0);
  // Left arm straight out to the side (+X picture), right arm bent (forearm up, hand by the shoulder).
  at(4, 0.18f, 0.5f, 0); at(5, 0.48f, 0.5f, 0); at(6, 0.74f, 0.5f, 0); at(7, 0.82f, 0.5f, 0);
  at(8, -0.18f, 0.5f, 0);
  if (bent) { at(9, -0.18f, 0.2f, 0.05f); at(10, -0.2f, 0.45f, 0.12f); at(11, -0.2f, 0.53f, 0.12f); }
  else { at(9, -0.48f, 0.5f, 0); at(10, -0.74f, 0.5f, 0); at(11, -0.82f, 0.5f, 0); }
  return pose;
}

void CheckBodyScale() {
  // A body the phone made 0.6 of an adult: scaled x1/0.6 around the hips, arms back to adult length.
  m::BodyScaleState state;
  const m::Frame small = Body(0.6f, false);
  for (int n = 0; n < 60; ++n) m::ObserveBody(state, small);
  CHECK(std::fabs(state.torso - 0.312f) < 1e-3f);
  float factor = 0;
  const m::Frame out = m::NormalizeBody(state, small, &factor);
  CHECK(std::fabs(factor - 1 / 0.6f) < 1e-3f);
  CHECK(out.joints[0].x == 0 && out.joints[0].y == 0 && out.joints[0].z == 0);  // the hips stay
  CHECK(std::fabs(m::Distance(out.joints[0], out.joints[2]) - m::kTorsoMetres) < 1e-3f);
  const float arm = m::Distance(out.joints[4], out.joints[7]);
  CHECK(arm > 0.6f && arm < 0.7f);  // an adult's straight arm (0.64 m)
  CHECK(m::PhoneArmLength(small, false) < 0.4f && m::PhoneArmLength(out, false) > 0.6f);
  // Directions are kept: the straight arm stays horizontal.
  CHECK(std::fabs(out.joints[7].y - out.joints[4].y) < 1e-4f && out.joints[7].x > out.joints[4].x);
  // A bent arm keeps its bend; its bones get adult lengths (at least 90 %).
  m::BodyScaleState bent_state;
  const m::Frame bent = Body(0.6f, true);
  for (int n = 0; n < 60; ++n) m::ObserveBody(bent_state, bent);
  const m::Frame bent_out = m::NormalizeBody(bent_state, bent);
  CHECK(m::Distance(bent_out.joints[8], bent_out.joints[9]) >= 0.9f * m::kUpperArmMetres - 1e-3f);
  CHECK(m::Distance(bent_out.joints[9], bent_out.joints[10]) >= 0.9f * m::kForearmMetres - 1e-3f);
  CHECK(bent_out.joints[10].y > bent_out.joints[9].y);  // the forearm still points up
  // Depth only as far as the picture-plane length leaves room for (1.0.5_candidate_1). The
  // right hand "on its shoulder" with both bones seen at full length but put 0.4 m towards
  // the camera (1.0.4_candidate_28's monocular error) comes back to the shoulder...
  {
    m::Frame adult_pose = Body(1.0f, false);
    adult_pose.joints[9] = {-0.18f, 0.2f, 0, 0.9f};                // elbow 0.30 below the shoulder
    adult_pose.joints[10] = {-0.2f, 0.46f, 0.4f, 0.9f};             // wrist up by the shoulder, 0.4 in front
    adult_pose.joints[11] = {-0.2f, 0.54f, 0.42f, 0.9f};
    m::BodyScaleState s1;
    m::Frame plain = Body(1.0f, true);  // the bones' lengths, learnt from frames without the error
    for (int n = 0; n < 60; ++n) m::ObserveBody(s1, plain);
    const m::Frame fixed = m::NormalizeBody(s1, adult_pose);
    CHECK(std::fabs(fixed.joints[11].z - fixed.joints[8].z) < 0.15f);
    CHECK(m::Distance(fixed.joints[11], fixed.joints[8]) < 0.2f);
    // ...while a punch at the camera (the forearm looks short in the picture) keeps its depth.
    m::Frame punch = Body(1.0f, false);
    punch.joints[9] = {-0.2f, 0.42f, 0.25f, 0.9f};   // the upper arm forward and a little down
    punch.joints[10] = {-0.2f, 0.45f, 0.5f, 0.9f};   // the forearm straight at the camera
    punch.joints[11] = {-0.2f, 0.45f, 0.58f, 0.9f};
    m::BodyScaleState s2;
    for (int n = 0; n < 60; ++n) m::ObserveBody(s2, punch);
    const m::Frame kept = m::NormalizeBody(s2, punch);
    CHECK(kept.joints[11].z - kept.joints[8].z > 0.45f);
    // 1.0.5_candidate_1's recording "hands on the shoulders": the hand by its shoulder in the
    // picture, the elbow 0.15 m forward and the forearm another 0.2 m forward (the phone cannot
    // tell which way a forearm points). The bent arm's forearm turns back: the hand ends by
    // the shoulder, in depth too.
    m::Frame charge = Body(1.0f, false);
    charge.joints[9] = {-0.22f, 0.32f, 0.15f, 0.9f};   // elbow forward and down
    charge.joints[10] = {-0.19f, 0.45f, 0.33f, 0.9f};  // the wrist forward again (the error)
    charge.joints[11] = {-0.18f, 0.53f, 0.36f, 0.9f};  // the hand by the shoulder in the picture
    m::BodyScaleState s3;
    for (int n = 0; n < 60; ++n) m::ObserveBody(s3, charge);
    const m::Frame back = m::NormalizeBody(s3, charge);
    CHECK(m::Distance(back.joints[11], back.joints[8]) < 0.15f);
    CHECK(back.joints[9].z > back.joints[8].z + 0.1f);  // the elbow stays forward
  }
  // Depth re-centred on 2.5 m (1.0.5_candidate_3): a page calibration that put the player at
  // its 3.8 m limit (hips at Z -1.30) is brought back over a few seconds; a quick step forward
  // still shows.
  {
    m::Frame far = Body(1.0f, false);
    for (auto& j : far.joints) j.z -= 1.3f;
    m::BodyScaleState sd;
    for (int n = 0; n < 900; ++n) m::ObserveBody(sd, far);
    CHECK(std::fabs(m::NormalizeBody(sd, far).joints[0].z) < 0.01f);
    CHECK(m::PositionHint(m::NormalizeBody(sd, far)) == m::Positioning::kOk);
    // A real step towards the sensor (0.3 m in a frame at most) still shows at once.
    m::Frame step = far;
    for (auto& j : step.joints) j.z += 0.3f;
    m::ObserveBody(sd, step);
    const float now_z = m::NormalizeBody(sd, step).joints[0].z;
    CHECK(now_z > 0.299f && now_z < 0.301f);  // whole (1.0.5_candidate_10: the average waits)
    // 1.0.5_candidate_10: a dodge back (0.35 m for a second) and back again: the average waits,
    // all of it reaches the title, and nothing of it stays.
    {
      m::BodyScaleState dodge = sd;
      for (int n = 0; n < 10; ++n) m::ObserveBody(dodge, far);  // back from the step first
      const float before = dodge.hip_z;
      m::Frame back_step = far;
      for (auto& j : back_step.joints) j.z -= 0.35f;
      float deepest = 0;
      for (int n = 0; n < 30; ++n) {
        m::ObserveBody(dodge, back_step);
        deepest = std::min(deepest, m::NormalizeBody(dodge, back_step).joints[0].z);
      }
      CHECK(deepest < -0.349f && deepest > -0.351f);
      CHECK(dodge.hip_z == before);
      for (int n = 0; n < 30; ++n) m::ObserveBody(dodge, far);
      CHECK(std::fabs(m::NormalizeBody(dodge, far).joints[0].z) < 0.01f && dodge.away_frames == 0);
      // ...but an offset that lasts (the player stays 0.38 m further back) is taken after 2 s.
      m::Frame stays = far;
      for (auto& j : stays.joints) j.z -= 0.38f;
      for (int n = 0; n < m::kAwayFrames; ++n) m::ObserveBody(dodge, stays);
      CHECK(dodge.hip_z == before);
      for (int n = 0; n < 900; ++n) m::ObserveBody(dodge, stays);
      CHECK(std::fabs(m::NormalizeBody(dodge, stays).joints[0].z) < 0.05f);
      CHECK(dodge.jumps == sd.jumps);  // a step, not a jump
    }
    // A jump no body can make (the page's "Recenter" says 2.5 m now): the new depth at once.
    m::Frame recalibrated = far;
    for (auto& j : recalibrated.joints) j.z += 1.3f;
    m::ObserveBody(sd, recalibrated);
    CHECK(std::fabs(m::NormalizeBody(sd, recalibrated).joints[0].z) < 0.01f);
    // ...and is counted for the log (1.0.5_candidate_6): one jump, from 3.5 m (after the step)
    // to 2.5 m. The real step before it was not one.
    CHECK(sd.jumps == 1 && std::fabs(sd.jump_from - (-1.0f)) < 1e-4f && std::fabs(sd.jump_to) < 1e-4f);
  }
  // An adult stays an adult; a huge or tiny reading is clamped (x0.8 .. x2).
  m::BodyScaleState adult;
  m::ObserveBody(adult, Body(1.0f, false));
  m::NormalizeBody(adult, Body(1.0f, false), &factor);
  CHECK(std::fabs(factor - 1) < 1e-3f);
  m::BodyScaleState tiny;
  m::ObserveBody(tiny, Body(0.2f, false));
  m::NormalizeBody(tiny, Body(0.2f, false), &factor);
  CHECK(factor == m::kLargestBodyScale);
  // No body, or no torso yet: the frame as it came.
  m::Frame lost = small;
  lost.tracked = false;
  CHECK(m::NormalizeBody(state, lost).joints[7].x == small.joints[7].x);
  m::BodyScaleState empty;
  CHECK(m::NormalizeBody(empty, small).joints[7].x == small.joints[7].x);
  // A new phone session starts its averages again.
  m::Frame other = Body(1.0f, false);
  other.session = 8;
  m::ObserveBody(state, other);
  CHECK(state.session == 8 && std::fabs(state.torso - 0.52f) < 1e-3f);
  // Unseen joints are not moved by the averages (their bones are not measured).
  m::BodyScaleState unseen;
  m::Frame hidden = small;
  hidden.joints[11].confidence = 0;
  m::ObserveBody(unseen, hidden);
  CHECK(unseen.bones[1][2] == 0);
}

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// Run from an assembled tree's root: the core uses the scan, not the traced addresses.
void CheckTree() {
  const char* kApp = ".deps/xenia-canary/src/xenia/kernel/xam/apps/xam_app.cc";
  const char* kNui = ".deps/xenia-canary/src/xenia/kernel/xam/xam_nui.cc";
  const char* kCam = ".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc";
  const std::string app = ReadText(kApp), nui = ReadText(kNui), cam = ReadText(kCam);
  if (app.empty() || nui.empty() || cam.empty()) {
    std::printf("no assembled tree here; core files not checked\n");
    return;
  }
  auto count = [](const std::string& text, const char* what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
  };
  for (const std::string* text : {&app, &nui, &cam}) {
    CHECK(text->find("0x8249D248") == std::string::npos);
    CHECK(text->find("0x821B51D0") == std::string::npos);
    CHECK(text->find("kVirtualNuiTitle") == std::string::npos);
    CHECK(text->find("title_id() != 0x4E4D084E") == std::string::npos);
  }
  // 821B4E24 only as the 1.0.2 image's fallback when the seam pattern misses it.
  CHECK(app.find("0x821B4E24") == std::string::npos && nui.find("0x821B4E24") == std::string::npos);
  CHECK(count(cam, "0x821B4E24") <= 3);
  CHECK(cam.find("0x821B4E24") == std::string::npos || cam.find("FindHandSeam") < cam.find("0x821B4E24"));
  CHECK(app.find("CheckIdentityCallback") != std::string::npos);
  CHECK(app.find("KinectRequiredTitle") != std::string::npos);
  CHECK(nui.find("KinectRequiredTitle") != std::string::npos);
  CHECK(cam.find("FindHandSeam") != std::string::npos);
  CHECK(cam.find("CallsOf") != std::string::npos);
  // The 1.0.2 hash stays only for that fallback and the host-only hand probe.
  CHECK(count(cam, "0x8FDD4F4AB147612A") <= 2);
  CHECK(app.find("0x8FDD4F4AB147612A") == std::string::npos);  // named in a comment only
  // The PsCam entry asks the executable, not the title ID.
  const size_t entry = cam.find("PsCamDeviceRequest_entry");
  CHECK(entry != std::string::npos && cam.find("KinectRequiredTitle", entry) != std::string::npos);
  // The title side: the library asks before a Kinect game starts with Camera motion off,
  // and a start that did not ask shows a notice when the game asks for its camera.
  const std::string launcher = ReadText("src/launcher.cpp"), game_main = ReadText("src/canary_game_main.cpp"),
                    covers = ReadText("src/covers.cpp");
  CHECK(launcher.find("GameRequiresKinect(launch_.path)") != std::string::npos);
  CHECK(launcher.find("own[\"motion_phone\"] = 1") != std::string::npos);
  CHECK(game_main.find("kinect_needed = &KinectNeededNotice") != std::string::npos);
  CHECK(covers.find("XexHeaderRequiresKinect") != std::string::npos);
  CHECK(cam.find("ReportKinectNeeded") != std::string::npos);
  // The title's skeleton frames and the fallback get the normalised body (fix 107: through the
  // emergency pose, which passes it on unless L3 + R3 brought the last body back).
  CHECK(cam.find("const xbox360ps5::motion::Frame pose = xbox360ps5::motion::NormalizedSnapshot();") != std::string::npos ||
        cam.find("const xbox360ps5::motion::Frame pose = xbox360ps5::motion::emergency_pose.Apply(\n"
                 "      xbox360ps5::motion::NormalizedSnapshot(), clock::now(), &emergency);") != std::string::npos);
  CHECK(cam.find("auto pose = xbox360ps5::motion::NormalizedSnapshot();") != std::string::npos ||
        cam.find("auto pose = xbox360ps5::motion::emergency_pose.Overlay(xbox360ps5::motion::NormalizedSnapshot());") !=
            std::string::npos);
  // Kinect Sports (1.0.5_candidate_5): avatars start for Kinect games, and the skeleton hook goes
  // in when the title starts its NUI library (2B003), before NuiSkeletonGetNextFrame is compiled.
  const std::string avatar = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_avatar.cc");
  CHECK(avatar.find("if (KinectRequiredTitle(kernel_state())) {") != std::string::npos);
  CHECK(app.find("xboxkrnl::BindVirtualKinectEarly(kernel_state_);") != std::string::npos &&
        app.find("xboxkrnl::BindVirtualKinectEarly(kernel_state_);") > app.find("case 0x0002B003:"));
  CHECK(cam.find("void BindVirtualKinectEarly(KernelState* kernel_state) {") != std::string::npos);
  // The virtual depth frames show the same body (DBZ's hand detection reads them).
  const std::string depth = ReadText("include/xbox360ps5/virtual_depth_camera.hpp");
  CHECK(depth.find("Render(NormalizedSnapshot(),depth_);") != std::string::npos ||
        depth.find("Render(emergency_pose.Overlay(NormalizedSnapshot()),depth_);") != std::string::npos);  // fix 107
  std::printf("tree: core files use the code scan; the library offers Camera motion\n");
}

int RealImage(int argc, char** argv) {
  std::ifstream in(argv[2], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const uint32_t base = uint32_t(std::strtoul(argv[3], nullptr, 16));
  const uint32_t callback = uint32_t(std::strtoul(argv[4], nullptr, 16));
  const m::CodeView view{bytes.data(), bytes.size(), base};
  int bad = 0;
  if (argc > 5) {
    std::ifstream x(argv[5], std::ios::binary);
    std::vector<uint8_t> header((std::istreambuf_iterator<char>(x)), std::istreambuf_iterator<char>());
    const bool required = m::XexHeaderRequiresKinect(header.data(), std::min<size_t>(header.size(), 1u << 20));
    std::printf("XEX: skeletal tracking required: %s\n", required ? "yes" : "no");
    bad += !required;
  }
  const auto identity = m::CheckIdentityCallback(view, callback);
  std::printf("identity callback %08X: %s; 2C00F case %08X calls %08X\n", callback,
              identity.ok ? "verified" : identity.why, identity.event_case, identity.refresh);
  bad += !identity.ok;
  const auto skeleton = m::FindSkeletonGetNextFrame(bytes.data(), bytes.size(), base);
  std::printf("NuiSkeletonGetNextFrame: %zu candidate(s)", skeleton.size());
  if (skeleton.size() == 1) {
    const auto calls = m::CallsOf(view, skeleton[0]);
    std::printf(" at %08X, %zu call(s)", skeleton[0], calls.size());
    if (calls.size() == 1) std::printf(", returns to %08X", calls[0] + 4);
  }
  std::printf("\n");
  bad += skeleton.size() != 1;
  const auto seam = m::FindHandSeam(view);
  std::printf("hand reach seam: %zu match(es)", seam.matches);
  if (seam.site) std::printf(" at %08X (r11 %016llX)", seam.site, (unsigned long long)seam.r11);
  std::printf("\n");
  if (seam.site) {
    const auto th = m::FindHandThresholds(view, seam);
    std::printf("hand reach thresholds: %s, reach %.4f direction %.4f\n", th.found ? "found" : "not found",
                th.reach, th.direction);
  }
  return bad ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc >= 5 && std::strcmp(argv[1], "--image") == 0) return RealImage(argc, argv);
  CheckIdentitySynthetic();
  CheckRegisterUse();
  CheckXexHeader();
  CheckHandSeamAndCalls();
  CheckBodyScale();
  CheckTree();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-nui-image-scan: all passed\n");
  return 0;
}
