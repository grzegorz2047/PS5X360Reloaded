// SPDX-License-Identifier: MIT
// Fix 156d (tools/source-1.0.2/fixes/156d-camera-state-idle.patch; Kinect Sports Season Two
// 4D5309D6, issue 63): the virtual camera's GetState (PsCamDeviceRequest code 0) says
// Streaming only while the title reads frames. Season Two's NUI library re-runs its camera
// start after a sport's load (82A642B8, from its camera worker 82A65F00): Ready (4) configures
// the camera and sets the streams up, Streaming (2) skips that. The camera said Streaming from
// the first frame read until a Stop the game never sends, so the streams were never set up
// again and the sport's loading screen never ended.
//
//   check-pscam-state        run from an assembled tree's root
//
//  (a) CameraState: Absent without a camera; Ready before any frame read; Streaming with reads
//      queued (however old the last request) or a request within 1 s; Ready after 1 s without
//      a read queued;
//  (b) a title's frame loop: reads asked for every 33 ms with one or two queued stay Streaming
//      between requests; a pause for the sport's load turns Ready after 1 s, and the next
//      read turns it back;
//  (c) the tree: GetState answers CameraState from the queued reads and the last read
//      request's time, read under the camera's lock; a frame read stamps the time; the
//      answer's changes are logged (16 lines); Stop still clears the queues and streaming_.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/pscam_state.hpp"

namespace ps = xbox360ps5::pscam_state;
using namespace std::chrono_literals;

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

static void CheckState() {
  CHECK(ps::CameraState(false, true, 3, 0ms) == ps::kAbsent);
  CHECK(ps::CameraState(true, false, 0, 0ms) == ps::kReady);  // boot: not read yet
  CHECK(ps::CameraState(true, true, 2, 0ms) == ps::kStreaming);
  CHECK(ps::CameraState(true, true, 1, 10s) == ps::kStreaming);  // a read is still queued
  CHECK(ps::CameraState(true, true, 0, 33ms) == ps::kStreaming);  // between two requests
  CHECK(ps::CameraState(true, true, 0, 1000ms) == ps::kStreaming);
  CHECK(ps::CameraState(true, true, 0, 1001ms) == ps::kReady);
  CHECK(ps::CameraState(true, true, 0, 60s) == ps::kReady);
  CHECK(ps::kAbsent == 0 && ps::kStreaming == 2 && ps::kReady == 4);
}

// (b) a title reading at 30 fps, then pausing for a load, then starting again.
static void CheckFrameLoop() {
  auto since = 0ms;
  size_t queued = 2;
  bool ever = false;
  for (int frame = 0; frame < 90; ++frame) {
    ever = true;
    since = 0ms;          // a read asked for
    queued = frame % 2 ? 1 : 2;
    CHECK(ps::CameraState(true, ever, queued, since) == ps::kStreaming);
    queued = 0;           // both served before the next request
    since = 33ms;
    CHECK(ps::CameraState(true, ever, queued, since) == ps::kStreaming);
  }
  since = 500ms;  // the sport's load begins: no reads
  CHECK(ps::CameraState(true, ever, 0, since) == ps::kStreaming);
  since = 1500ms;
  CHECK(ps::CameraState(true, ever, 0, since) == ps::kReady);  // the library sets up again
  CHECK(ps::CameraState(true, ever, 1, 0ms) == ps::kStreaming);
}

static void CheckTree() {
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  if (cam.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_pscam.cc)\n");
    ++failures;
    return;
  }
  CHECK(Has(cam, "#include \"xbox360ps5/pscam_state.hpp\"  // fix 156d"));
  const size_t get = cam.find("      case CameraRequestCode::kGetState: {");
  const size_t end = cam.find("        return X_STATUS_SUCCESS;", get);
  const std::string state = get == std::string::npos ? "" : cam.substr(get, end - get);
  const size_t lock = state.find("std::lock_guard<xe_mutex> lock(mutex_);");
  const size_t queued = state.find("queued = depth_reads_.size() + color_reads_.size();");
  const size_t last = state.find("last = last_read_request_;");
  const size_t answer = state.find("xbox360ps5::pscam_state::CameraState(\n"
                                   "            camera_ != nullptr, streaming_.load(), queued,\n"
                                   "            std::chrono::steady_clock::now() - last);");
  CHECK(lock != std::string::npos && queued != std::string::npos && last != std::string::npos &&
        answer != std::string::npos && lock < queued && queued < answer && last < answer);
  CHECK(Has(state, "Guest<X_PSCAM_STATE_REQUEST>(request_ptr)->state = state;"));
  CHECK(!Has(state, "streaming_ ? kDeviceStateStreaming"));
  CHECK(Has(state, "Virtual Kinect: GetState now answers {} ({} frame reads queued) "));
  CHECK(Has(state, "if (shown.fetch_add(1) < 16)"));
  const size_t read = cam.find("        streaming_ = true;\n"
                               "        last_read_request_ = std::chrono::steady_clock::now();  // fix 156d");
  CHECK(read != std::string::npos);
  CHECK(Has(cam, "  std::chrono::steady_clock::time_point last_read_request_{};"));
  CHECK(Has(cam, "        depth_reads_.clear();\n        color_reads_.clear();\n        streaming_ = false;"));
}

int main() {
  CheckState();
  CheckFrameLoop();
  CheckTree();
  if (failures) {
    std::printf("check-pscam-state: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-pscam-state: ok\n");
  return 0;
}
