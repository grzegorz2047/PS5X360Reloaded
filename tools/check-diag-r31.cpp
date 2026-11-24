// SPDX-License-Identifier: MIT
// Fix 156k (tools/source-1.0.2/fixes/156k-diag-r31-skeleton-after-depth.patch; Kinect Sports
// Season Two 4D5309D6, issue 63):
//  - the Kinect diag's 10 s thread snapshot also gives each function's r31 along the back
//    chain (saved by its callee 0x10 below the caller's stack pointer) and that address's
//    first word: Season Two's darts load waits in a loop that keeps its load operation in r31
//    (82620560), and the line names the operation and its vtable;
//  - no skeleton frame before the camera's first depth frame: fix 156i numbers skeleton frames
//    as their depth frame, and one handed out earlier on the skeleton's own clock (684, then
//    3, 5, 6 on 1.0.6-work-fix156j) ran the numbers backwards.
//
//   check-diag-r31        run from an assembled tree's root
//
//  (a) BackChainR31 over a fake guest stack: each saved LR with the word 0x10 below the
//      caller's stack pointer, its first word when readable, the same stops as BackChain;
//      ChainR31's text;
//  (b) the tree: the snapshot's second line; the skeleton wait holds until a depth frame.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "xbox360ps5/kinect_diag.hpp"

namespace kd = xbox360ps5::kinect_diag;

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

static void CheckWalk() {
  // Three frames: sp 7018F000 -> 7018F100 -> 7018F200 -> 7018F300 (top).
  std::map<uint32_t, uint32_t> mem = {
      {0x7018F000, 0x7018F100}, {0x7018F100 - 8, 0x82493214}, {0x7018F100 - 0x10, 0x40A00000},
      {0x7018F100, 0x7018F200}, {0x7018F200 - 8, 0x82420760}, {0x7018F200 - 0x10, 0x00000005},
      {0x7018F200, 0x7018F300}, {0x7018F300 - 8, 0x82620594}, {0x7018F300 - 0x10, 0x40B00000},
      {0x7018F300, 0x00000000},
      {0x40A00000, 0x8206BBD8}, {0x40B00000, 0x82070000},
  };
  auto read = [&](uint32_t a, uint32_t* v) {
    auto it = mem.find(a);
    if (it == mem.end()) return false;
    *v = it->second;
    return true;
  };
  kd::SavedR31 frames[kd::kFrames];
  const int n = kd::BackChainR31(0x7018F000, read, frames, kd::kFrames);
  CHECK(n == 3);
  CHECK(frames[0].ret == 0x82493214 && frames[0].r31 == 0x40A00000 && frames[0].readable &&
        frames[0].first_word == 0x8206BBD8);
  CHECK(frames[1].ret == 0x82420760 && frames[1].r31 == 5 && !frames[1].readable);
  CHECK(frames[2].ret == 0x82620594 && frames[2].r31 == 0x40B00000 &&
        frames[2].first_word == 0x82070000);
  CHECK(kd::ChainR31(frames, n) ==
        "82493214 r31 40A00000 [8206BBD8] < 82420760 r31 00000005 < 82620594 r31 40B00000 [82070000]");
  CHECK(kd::BackChainR31(0x7018F000, read, frames, 1) == 1);
  CHECK(kd::BackChainR31(0, read, frames, kd::kFrames) == 0);
  // A chain that goes down (or jumps more than 1 MiB) stops.
  mem[0x7018F000] = 0x7018E000;
  CHECK(kd::BackChainR31(0x7018F000, read, frames, kd::kFrames) == 0);
}

static void CheckTree() {
  const std::string shim = ReadText(".deps/xenia-canary/src/xenia/kernel/util/shim_utils.h");
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  if (shim.empty() || cam.empty()) {
    std::printf("FAIL: no assembled tree here (shim_utils.h, xboxkrnl_pscam.cc)\n");
    ++failures;
    return;
  }
  const size_t first = shim.find("XELOGW(\"Kinect diag: thread {} calls {} from {} (fix 108)\"");
  const size_t second = shim.find("Kinect diag: thread {} r31 by frame {} (fix 156k)");
  CHECK(first != std::string::npos && second != std::string::npos && first < second);
  CHECK(Has(shim, "kd::BackChainR31(uint32_t(context->r[1]), KinectDiagRead, frames, kd::kFrames);"));
  CHECK(Has(cam, "while (period == source.delivered_period || !g_depth_frame_number.load()) {"));
}

int main() {
  CheckWalk();
  CheckTree();
  if (failures) {
    std::printf("check-diag-r31: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-diag-r31: ok\n");
  return 0;
}
