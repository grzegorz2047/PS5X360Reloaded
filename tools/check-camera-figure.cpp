// SPDX-License-Identifier: MIT
// Fix 185 (tools/source-1.0.2/fixes/185-kinect-camera-figure.patch): a figure of the tracked
// player as the Kinect colour camera's picture while the phone's camera picture is off, and whole
// 16-bit frames for titles that read the 16-bit picture (Harry Potter for Kinect).
//
//   check-camera-figure        run from an assembled tree's root
//
//  (a) the mapping: FigurePoint is NuiTransformSkeletonToDepthImage (285.63 per 320x240) scaled
//      to 640x480; a joint the skeleton frame has at sensor (x, y, z) is drawn there;
//  (b) the rasteriser: a standing body at 2.5 m draws its torso, head, left (blue) and right
//      (orange) limbs at the joints' places, all inside the picture and inside the joints' box
//      grown by the thickest limb; a body half out of the picture is clipped, not wrapped; joints
//      seen badly (confidence < 0.4) are not drawn; no body: the background alone;
//  (c) the mosaic: each Bayer site (G R / B G) takes its own channel of the part's colour;
//  (d) the cost: drawn once per new pose (the same pose twice draws once), into the same buffers
//      (no allocation after the first frame);
//  (e) the 16-bit picture: 0x96000 bytes, U Y V Y, grey stays grey, the background and the parts
//      keep their colour per 2x2 block;
//  (f) the camera: with the phone's picture off it gives the figure, with the option off or the
//      picture wanted (none came yet) the grey of before; a phone frame without a body gives the
//      background;
//  (g) the tree: the camera's branch and member, the core's option, its log line, the 16-bit read.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/camera_figure.hpp"
#include "xbox360ps5/motion_emergency.hpp"  // StandingBody
// The camera (f) needs the USB Kinect path's POSIX headers: it runs on the CI's Linux, not on a
// Windows host.
#if __has_include(<netinet/in.h>)
#define HAVE_CAMERA 1
// The core's log, as check-kinect-usb.cpp replaces it (the USB Kinect path logs).
#define XBOX360PS5_KINECT_USB_HOST_LOG
#include <string_view>
template <typename... Args>
void XELOGW(std::string_view, const Args&...) {}
template <typename... Args>
void XELOGI(std::string_view, const Args&...) {}
#include "xbox360ps5/virtual_depth_camera.hpp"
#else
#define HAVE_CAMERA 0
#endif

namespace m = xbox360ps5::motion;

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
static bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

static bool Near(float a, float b, float tolerance = 0.01f) { return std::fabs(a - b) <= tolerance; }

// Where joint n of `pose` lands in the picture.
static bool JointPixel(const m::Frame& pose, int n, int& x, int& y) {
  float sx, sy, sz, px, py;
  m::SkeletonSpace(pose.joints[n], sx, sy, sz);
  if (!m::FigurePoint(sx, sy, sz, px, py)) return false;
  x = int(px);
  y = int(py);
  return x >= 0 && x < 640 && y >= 0 && y < 480;
}
static uint8_t MaskAt(const std::vector<uint8_t>& mask, int x, int y) { return mask[size_t(y) * 640 + x]; }

static void CheckMapping() {
  float px, py;
  CHECK(m::FigurePoint(0, 0, 2.0f, px, py) && Near(px, 320) && Near(py, 240));
  // NuiTransformSkeletonToDepthImage: x 0.5 + X * 285.63 / (Z * 320), y 0.5 - Y * 285.63 / (Z * 240).
  CHECK(m::FigurePoint(0.5f, 0.25f, 2.5f, px, py));
  CHECK(Near(px, 640 * (0.5f + 0.5f * 285.63f / (2.5f * 320))) && Near(py, 480 * (0.5f - 0.25f * 285.63f / (2.5f * 240))));
  CHECK(Near(px, 320 + 0.5f * 571.26f / 2.5f, 0.05f) && Near(py, 240 - 0.25f * 571.26f / 2.5f, 0.05f));
  CHECK(!m::FigurePoint(0, 0, 0, px, py) && !m::FigurePoint(0, 0, -1, px, py));
  // The same point the skeleton frame has: SkeletonSpace, then the library's mapping.
  m::Frame pose = m::StandingBody(1);
  float sx, sy, sz;
  m::SkeletonSpace(pose.joints[11], sx, sy, sz);
  CHECK(m::FigurePoint(sx, sy, sz, px, py));
  std::vector<uint8_t> frame(m::kSkeletonFrameBytes);
  m::WriteSkeletonFrame(frame.data(), pose, 1, 0, 1);
  auto f32 = [&](size_t at) {
    uint32_t bits = uint32_t(frame[at]) << 24 | uint32_t(frame[at + 1]) << 16 | uint32_t(frame[at + 2]) << 8 | frame[at + 3];
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
  };
  const size_t hand = m::kSkeletonDataOffset + 0x20 + 11 * 16;  // the first body's right hand
  float fx, fy;
  CHECK(m::FigurePoint(f32(hand), f32(hand + 4), f32(hand + 8), fx, fy) && Near(fx, px) && Near(fy, py));
}

static void CheckRaster() {
  std::vector<uint8_t> mask;
  m::Frame none;
  m::CameraFigure::DrawMask(none, mask);
  CHECK(mask.size() == 640u * 480u);
  bool empty = true;
  for (uint8_t v : mask) empty &= v == m::kBackground;
  CHECK(empty);

  m::Frame pose = m::StandingBody(1);
  pose.tracked = true;
  m::CameraFigure::DrawMask(pose, mask);
  size_t counts[4] = {};
  int min_x = 640, max_x = -1, min_y = 480, max_y = -1;
  for (int y = 0; y < 480; ++y)
    for (int x = 0; x < 640; ++x) {
      const uint8_t v = MaskAt(mask, x, y);
      CHECK(v <= m::kRightLimb);
      ++counts[v & 3];
      if (v != m::kBackground) {
        min_x = std::min(min_x, x); max_x = std::max(max_x, x);
        min_y = std::min(min_y, y); max_y = std::max(max_y, y);
      }
    }
  CHECK(counts[m::kTorso] > 3000 && counts[m::kLeftLimb] > 1000 && counts[m::kRightLimb] > 1000);
  CHECK(counts[m::kBackground] > counts[m::kTorso]);
  // Inside the joints' box grown by the thickest part (the torso, 0.16 m at about 2.5 m).
  int jx0 = 640, jx1 = -1, jy0 = 480, jy1 = -1;
  for (int n = 0; n < 20; ++n) {
    int x, y;
    if (JointPixel(pose, n, x, y)) {
      jx0 = std::min(jx0, x); jx1 = std::max(jx1, x); jy0 = std::min(jy0, y); jy1 = std::max(jy1, y);
    }
  }
  const int grow = 50;
  CHECK(min_x >= jx0 - grow && max_x <= jx1 + grow && min_y >= jy0 - grow && max_y <= jy1 + grow);
  // The parts at their joints: the head, the hip centre, a hand on each side, a foot on each side.
  int x, y;
  CHECK(JointPixel(pose, 3, x, y) && MaskAt(mask, x, y) == m::kTorso);
  CHECK(JointPixel(pose, 1, x, y) && MaskAt(mask, x, y) == m::kTorso);
  CHECK(JointPixel(pose, 7, x, y) && MaskAt(mask, x, y) == m::kLeftLimb);
  CHECK(JointPixel(pose, 11, x, y) && MaskAt(mask, x, y) == m::kRightLimb);
  CHECK(JointPixel(pose, 15, x, y) && MaskAt(mask, x, y) == m::kLeftLimb);
  CHECK(JointPixel(pose, 19, x, y) && MaskAt(mask, x, y) == m::kRightLimb);
  // Mirrored, as the library maps it: the player's left hand (sensor -x) on the picture's left.
  int lx, ly, rx, ry;
  CHECK(JointPixel(pose, 7, lx, ly) && JointPixel(pose, 11, rx, ry) && lx < rx);

  // Half out of the picture: clipped at the edge, nothing wraps to the other side.
  m::Frame side = pose;
  for (auto& j : side.joints) j.x += 1.4f;  // the phone's +x (sensor -x): to this picture's left
  m::CameraFigure::DrawMask(side, mask);
  size_t left_half = 0, right_half = 0;
  for (int yy = 0; yy < 480; ++yy)
    for (int xx = 0; xx < 640; ++xx)
      if (MaskAt(mask, xx, yy) != m::kBackground) (xx < 320 ? left_half : right_half)++;
  CHECK(left_half > 0 && right_half == 0);

  // A right arm seen badly is not drawn; the rest is.
  m::Frame bad = pose;
  for (int n : {9, 10, 11}) bad.joints[n].confidence = 0.3f;
  m::CameraFigure::DrawMask(bad, mask);
  CHECK(JointPixel(bad, 11, x, y) && MaskAt(mask, x, y) == m::kBackground);
  CHECK(JointPixel(bad, 7, x, y) && MaskAt(mask, x, y) == m::kLeftLimb);
}

static void CheckMosaicAndCache() {
  std::vector<uint8_t> mask(640 * 480, m::kBackground), mosaic;
  mask[0] = m::kTorso;
  mask[1] = m::kTorso;
  mask[640] = m::kLeftLimb;
  mask[641] = m::kRightLimb;
  m::CameraFigure::MaskToMosaic(mask, mosaic);
  CHECK(mosaic.size() == 640u * 480u);
  CHECK(mosaic[0] == m::kFigureColours[m::kTorso][1]);        // G
  CHECK(mosaic[1] == m::kFigureColours[m::kTorso][0]);        // R
  CHECK(mosaic[640] == m::kFigureColours[m::kLeftLimb][2]);   // B
  CHECK(mosaic[641] == m::kFigureColours[m::kRightLimb][1]);  // G
  CHECK(mosaic[2] == m::kFigureColours[m::kBackground][1] && mosaic[3] == m::kFigureColours[m::kBackground][0]);

  m::CameraFigure figure;
  m::Frame pose = m::StandingBody(3);
  pose.tracked = true;
  pose.sequence = 10;
  const auto* first = &figure.Mosaic(pose);
  const uint8_t* data = first->data();
  CHECK(figure.draws() == 1);
  figure.Mosaic(pose);
  CHECK(figure.draws() == 1);  // the same pose: not drawn again
  pose.sequence = 11;
  pose.joints[11].y += 0.2f;
  const auto& second = figure.Mosaic(pose);
  CHECK(figure.draws() == 2 && &second == first && second.data() == data);  // the same buffer
  m::Frame gone = pose;
  gone.tracked = false;
  const auto& empty = figure.Mosaic(gone);
  CHECK(figure.draws() == 3 && empty.data() == data);
  bool background = true;
  for (uint32_t yy = 0; yy < 480; ++yy)
    for (uint32_t xx = 0; xx < 640; ++xx)
      background &= empty[yy * 640 + xx] == m::kFigureColours[m::kBackground][m::MosaicChannel(xx, yy)];
  CHECK(background);
}

static void CheckUyvy() {
  std::vector<uint8_t> grey(640 * 480, 128), out;
  m::BayerToUyvy(grey, out);
  CHECK(out.size() == 0x96000 && m::kUyvyBytes == 0x96000 && m::kColorYuvReadBytes == 0x96000);
  bool flat = true;
  for (uint8_t v : out) flat &= v == 128;
  CHECK(flat);
  // The background and a light block keep their colours.
  std::vector<uint8_t> mask(640 * 480, m::kBackground), mosaic;
  for (int y = 100; y < 104; ++y)
    for (int x = 200; x < 204; ++x) mask[y * 640 + x] = m::kRightLimb;
  m::CameraFigure::MaskToMosaic(mask, mosaic);
  m::BayerToUyvy(mosaic, out);
  uint8_t by, bu, bv, ry, ru, rv;
  const auto& bg = m::kFigureColours[m::kBackground];
  const auto& right = m::kFigureColours[m::kRightLimb];
  m::RgbToYuv(bg[0], bg[1], bg[2], by, bu, bv);
  m::RgbToYuv(right[0], right[1], right[2], ry, ru, rv);
  CHECK(out[0] == bu && out[1] == by && out[2] == bv && out[3] == by);  // U Y V Y
  const size_t at = (size_t(101) * 640 + 200) * 2;
  CHECK(out[at] == ru && out[at + 1] == ry && out[at + 2] == rv && out[at + 3] == ry);
  CHECK(ru < 128 && rv > 128 && by < 60);  // orange: little blue, much red; the background dark
  std::vector<uint8_t> small(10, 0);
  m::BayerToUyvy(small, out);
  CHECK(out.size() == 0x96000 && out[0] == 128);
}

// One XMP1 packet (as the phone page sends it) of `pose`.
static std::string Packet(const m::Frame& pose) {
  std::string out(336, '\0');
  std::memcpy(out.data(), "XMP1", 4);
  auto put = [&](size_t at, uint32_t v) { for (int n = 0; n < 4; ++n) out[at + n] = char(v >> (8 * n)); };
  put(4, pose.sequence);
  put(8, pose.session);
  put(12, pose.tracked ? 1 : 0);
  for (int n = 0; n < 20; ++n) {
    const float v[4] = {pose.joints[n].x, pose.joints[n].y, pose.joints[n].z, pose.joints[n].confidence};
    for (int c = 0; c < 4; ++c) {
      uint32_t bits;
      std::memcpy(&bits, &v[c], 4);
      put(16 + n * 16 + c * 4, bits);
    }
  }
  return out;
}

static void CheckCamera() {
#if !HAVE_CAMERA
  std::printf("skip (f): no POSIX headers on this host\n");
#else
  m::SetEnabled(true);
  auto camera = m::VirtualDepthCamera::Open(false, {});
  CHECK(camera != nullptr);
  if (!camera) return;
  m::Frame pose = m::StandingBody(77);
  pose.tracked = true;
  pose.sequence = 1;
  CHECK(m::Receive(Packet(pose)) == m::Result::accepted);
  std::vector<uint8_t> out;
  m::picture_wanted = false;
  m::camera_figure_wanted = true;
  CHECK(camera->CopyColorFrame(0, out) != 0);
  CHECK(out.size() == 640u * 480u);
  size_t lit = 0;
  for (uint8_t v : out) lit += v > 150;
  CHECK(lit > 2000);  // the figure
  CHECK(camera->FigureDraws() == 1);
  // The option off: the grey of before.
  m::camera_figure_wanted = false;
  std::vector<uint8_t> again;
  camera->CopyColorFrame(0, again);
  bool grey = !again.empty();
  for (uint8_t v : again) grey &= v == m::kNoPictureLevel;
  CHECK(grey);
  // The picture wanted, none yet: grey too.
  m::camera_figure_wanted = true;
  m::picture_wanted = true;
  camera->CopyColorFrame(0, again);
  grey = !again.empty();
  for (uint8_t v : again) grey &= v == m::kNoPictureLevel;
  CHECK(grey);
  // No body: the background.
  m::picture_wanted = false;
  m::Frame nobody;
  nobody.session = 77;
  nobody.sequence = 2;
  CHECK(m::Receive(Packet(nobody)) == m::Result::accepted);
  camera->CopyColorFrame(0, again);
  bool background = again.size() == 640u * 480u;
  for (uint32_t yy = 0; background && yy < 480; ++yy)
    for (uint32_t xx = 0; xx < 640; ++xx)
      background &= again[yy * 640 + xx] == m::kFigureColours[m::kBackground][m::MosaicChannel(xx, yy)];
  CHECK(background);
  camera.reset();
  m::SetEnabled(false);
#endif
}

static void CheckTree() {
  const std::string camera = ReadText("include/xbox360ps5/virtual_depth_camera.hpp");
  CHECK(Has(camera, "#include \"xbox360ps5/camera_figure.hpp\""));
  const size_t picture = camera.find("if (picture_store.Copy(SteadyMs(), picture_wanted.load() && enabled.load(), out)) {");
  const size_t figure = camera.find("if (!picture_wanted.load() && camera_figure_wanted.load()) {");
  const size_t mosaic = camera.find("figure_.Mosaic(emergency_pose.Overlay(NormalizedSnapshot()))");
  const size_t grey = camera.find("out.assign(kColorFrameBytes,kNoPictureLevel);return sequence;");
  CHECK(picture != std::string::npos && figure != std::string::npos && mosaic != std::string::npos &&
        grey != std::string::npos && picture < figure && figure < mosaic && mosaic < grey);
  CHECK(Has(camera, "CameraFigure figure_;  // fix 185"));
  // The depth frames draw the same body (the figure lines up with the skeleton and depth frames).
  CHECK(Has(camera, "Render(emergency_pose.Overlay(NormalizedSnapshot()),depth_)"));

  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  CHECK(Has(cam, "DEFINE_bool(kinect_camera_figure, true,"));
  const size_t opened = cam.find("XELOGI(\"PsCamDeviceRequest: no Kinect camera found\");");
  const size_t option = cam.find("xbox360ps5::motion::camera_figure_wanted = cvars::kinect_camera_figure;", opened);
  CHECK(opened != std::string::npos && option != std::string::npos && option - opened < 300);
  CHECK(Has(cam, "const bool yuv = !depth && request->buffer_size == xbox360ps5::motion::kColorYuvReadBytes;"));
  CHECK(Has(cam, ": yuv  ? uint32_t(xbox360ps5::motion::kUyvyBytes)"));
  const size_t convert = cam.find("xbox360ps5::motion::BayerToUyvy(frame_, yuv_);");
  const size_t copy = cam.find("std::memcpy(Guest<uint8_t>(buffer), yuv_.data(), xbox360ps5::motion::kUyvyBytes);", convert);
  CHECK(convert != std::string::npos && copy != std::string::npos);
  CHECK(Has(cam, "std::vector<uint8_t> yuv_;  // fix 185"));
  CHECK(Has(cam, "done.bytes = copy_bytes;"));
}

int main() {
  CheckMapping();
  CheckRaster();
  CheckMosaicAndCache();
  CheckUyvy();
  CheckCamera();
  CheckTree();
  if (failures) {
    std::printf("check-camera-figure: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-camera-figure: ok\n");
  return 0;
}
