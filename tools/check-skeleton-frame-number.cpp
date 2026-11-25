// SPDX-License-Identifier: MIT
// Fix 156i (tools/source-1.0.2/fixes/156i-skeleton-frame-number.patch; Fighters Uncaged
// 5553084E #67, Zumba Fitness Core 4D4A07F0 #63): a skeleton frame carries the number of the
// depth frame it stands for, as the console's NUI does. Fighters Uncaged's per-frame fetch
// (821A1148) keeps the largest frame number of its skeleton, depth and color frames and takes a
// new one from a source only when that source's number differs; the virtual skeleton was
// numbered on its own clock and the depth frames on the camera's, so the numbers never met and
// the title saw no body ("No body detected") except while the Guide paused its image streams.
//
//   check-skeleton-frame-number        run from an assembled tree's root
//
//  (a) SkeletonFrameNumber: the last delivered depth frame's number; the skeleton's period
//      before any depth frame;
//  (b) a model of Fighters Uncaged's fetch: with skeleton frames on their own clock the body
//      is never taken while depth frames come; numbered as the depth frame, it is taken every
//      depth frame;
//  (c) the tree: the camera records each delivered depth frame's number (the counter it
//      writes for the title), the skeleton frame is numbered and timed with it, the first 4
//      are logged, and the number is cleared with the camera.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/pscam_state.hpp"

namespace ps = xbox360ps5::pscam_state;

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

// Fighters Uncaged 821A1148, reduced: sync = the largest frame number seen; a source is read
// again only when its number differs from sync; the title uses the skeleton (the body) on a
// frame when its number equals sync. Returns how many of `frames` frames used a body.
static int BodiesUsed(int frames, uint32_t skeleton_offset, bool numbered_as_depth) {
  uint32_t sync = 0, skeleton = 0, depth = 0;
  int used = 0;
  for (int f = 1; f <= frames; ++f) {
    const uint32_t depth_now = 1000 + uint32_t(f);   // the camera's numbers
    const uint32_t period_now = uint32_t(f) + skeleton_offset;  // the skeleton's own clock
    if (skeleton != sync || skeleton == 0)
      skeleton = numbered_as_depth ? ps::SkeletonFrameNumber(depth, period_now) : period_now;
    sync = std::max(sync, skeleton);
    if (depth != sync) depth = depth_now;
    sync = std::max(sync, depth);
    if (skeleton == sync) ++used;
  }
  return used;
}

static void CheckNumber() {
  CHECK(ps::SkeletonFrameNumber(0, 7) == 7);       // no depth frame yet: the period
  CHECK(ps::SkeletonFrameNumber(1234, 7) == 1234);
  CHECK(ps::SkeletonFrameNumber(1, 99999) == 1);
  // Own clock, behind the depth numbers: never taken.
  CHECK(BodiesUsed(300, 0, false) == 0);
  // Numbered as the last depth frame: taken (from the second frame on).
  CHECK(BodiesUsed(300, 0, true) >= 290);
}

static void CheckTree() {
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  if (cam.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_pscam.cc)\n");
    ++failures;
    return;
  }
  CHECK(Has(cam, "std::atomic<uint32_t> g_depth_frame_number{0};"));
  const size_t set = cam.find("    if (depth) g_depth_frame_number = sequence;  // fix 156i\n"
                              "    const uint32_t counter_ptr = request->frame_counter_ptr;");
  CHECK(set != std::string::npos);
  CHECK(cam.find("*Guest<xe::be<uint32_t>>(counter_ptr) = sequence;", set) != std::string::npos);
  CHECK(Has(cam, "const uint32_t frame_number = xbox360ps5::pscam_state::SkeletonFrameNumber(\n"
                 "      g_depth_frame_number.load(), period);"));
  CHECK(Has(cam, "memory->TranslateVirtual(frame_ptr), pose, frame_number,\n"
                 "      uint64_t(frame_number) * kSkeletonPeriodMs,"));
  CHECK(Has(cam, "Virtual Kinect: skeleton frame {} (depth frame {}, skeleton period {}) "));
  CHECK(Has(cam, "g_depth_frame_number = 0;  // fix 156i"));
}

int main() {
  CheckNumber();
  CheckTree();
  if (failures) {
    std::printf("check-skeleton-frame-number: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-skeleton-frame-number: ok\n");
  return 0;
}
