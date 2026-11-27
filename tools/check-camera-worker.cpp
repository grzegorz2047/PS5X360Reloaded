// SPDX-License-Identifier: MIT
// Fix 156e (tools/source-1.0.2/fixes/156e-camera-packed-depth.patch; Kinect Sports Season Two
// 4D5309D6, issue 63): packed depth reads and the camera worker's state. On
// 1.0.6-work-fix156d the darts load still left three frame reads queued that were never
// completed, with nothing in the log saying why. Season Two's NUI read completion (82A645F0)
// takes a depth frame of 0x96000 bytes (16-bit samples) or 0x67200 (packed 11-bit, marked
// 0x80); the virtual camera refused any buffer smaller than 0x96000.
//
//   check-camera-worker        run from an assembled tree's root
//
//  (a) DepthReadBytes: 0x96000 for a buffer that holds it, 0x67200 for one that holds only the
//      packed frame, 0 (the read fails) below that;
//  (b) CameraWorkerJson: the guest routine the worker is in and for how long (0 when in none),
//      the reads put back, failed and answered packed; it sits in the status line after
//      guest_last_request;
//  (c) the tree: a depth read gets the packed frame when its buffer only holds that, with
//      0x67200 as its byte count; failures are counted and logged (too small, request not
//      mapped, cancelled by a stop); the worker marks the routine around the guest call;
//      put-back reads are counted; each stream's buffer sizes and Configure's words are logged.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/motion_input.hpp"
#include "xbox360ps5/pscam_state.hpp"

namespace m = xbox360ps5::motion;
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

static void CheckSizes() {
  CHECK(ps::kDepthUnpackedBytes == 0x96000u && ps::kDepthPackedBytes == 0x67200u);
  CHECK(ps::DepthReadBytes(0x96000) == 0x96000u);
  CHECK(ps::DepthReadBytes(0x100000) == 0x96000u);
  CHECK(ps::DepthReadBytes(0x95FFF) == 0x67200u);
  CHECK(ps::DepthReadBytes(0x67200) == 0x67200u);
  CHECK(ps::DepthReadBytes(0x671FF) == 0u);
  CHECK(ps::DepthReadBytes(0) == 0u);
}

static void CheckJson() {
  m::camera_worker_routine = 0;
  m::camera_worker_since_ms = 0;
  m::camera_reads_requeued = 0;
  m::camera_reads_failed = 0;
  m::camera_reads_packed = 0;
  CHECK(m::CameraWorkerJson(5000) ==
        ",\"camera_worker\":{\"routine\":\"00000000\",\"busy_ms\":0,\"requeued\":0,\"failed\":0,"
        "\"packed_depth\":0}");
  m::camera_worker_routine = 0x82A645F0;
  m::camera_worker_since_ms = 1000;
  m::camera_reads_requeued = 7;
  m::camera_reads_failed = 2;
  m::camera_reads_packed = 3;
  CHECK(m::CameraWorkerJson(4500) ==
        ",\"camera_worker\":{\"routine\":\"82A645F0\",\"busy_ms\":3500,\"requeued\":7,\"failed\":2,"
        "\"packed_depth\":3}");
  const std::string status = m::Status();
  const size_t last = status.find("\"guest_last_request\":");
  const size_t worker = status.find(",\"camera_worker\":{\"routine\":\"82A645F0\"");
  const size_t bound = status.find(",\"guest_skeleton_bound\":");
  CHECK(last != std::string::npos && worker != std::string::npos && bound != std::string::npos &&
        last < worker && worker < bound);
  m::camera_worker_routine = 0;
}

static void CheckTree() {
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  const std::string input = ReadText("include/xbox360ps5/motion_input.hpp");
  if (cam.empty() || input.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_pscam.cc, motion_input.hpp)\n");
    ++failures;
    return;
  }
  CHECK(Has(input, "    CameraWorkerJson(SteadyMs()) +  // fix 156e"));
  CHECK(Has(cam, "depth ? pst::DepthReadBytes(request->buffer_size) : 0;"));
  CHECK(Has(cam, "const bool packed = depth_bytes == pst::kDepthPackedBytes &&\n"
                 "                        frame_.size() == pst::kDepthPackedBytes;"));
  CHECK(Has(cam, "const uint32_t copy_bytes = packed ? pst::kDepthPackedBytes : frame_bytes;") ||
        Has(cam, "const uint32_t copy_bytes = packed ? pst::kDepthPackedBytes\n"
                 "                                : yuv  ? uint32_t(xbox360ps5::motion::kUyvyBytes)\n"
                 "                                       : frame_bytes;"));  // fix 185: 16-bit colour reads
  CHECK(Has(cam, "if (request->buffer_size < copy_bytes ||\n        !IsGuestRange(buffer, copy_bytes)) {"));
  CHECK(Has(cam, "std::memcpy(Guest<uint8_t>(buffer), frame_.data(), pst::kDepthPackedBytes);"));
  CHECK(Has(cam, "    done.bytes = copy_bytes;\n    Complete(done);"));
  CHECK(Has(cam, "NoteFailedRead(done, depth, request->buffer_size, \"buffer too small or not mapped\");"));
  CHECK(Has(cam, "NoteFailedRead(done,depth,0,\"request not mapped\");"));
  CHECK(Has(cam, "NoteFailedRead(read, depth, 0, \"cancelled by a stop\");  // fix 156e"));
  CHECK(Has(cam, "Virtual Kinect: {} frame read {:08X} completed with {:08X}: {} (buffer size "));
  const size_t mark = cam.find("xbox360ps5::motion::camera_worker_routine = request.completion;");
  const size_t exec = cam.find("kernel_state_->processor()->Execute(worker_thread_->thread_state(),", mark);
  const size_t clear = cam.find("xbox360ps5::motion::camera_worker_routine = 0;", exec);
  CHECK(mark != std::string::npos && exec != std::string::npos && clear != std::string::npos);
  CHECK(Has(cam, "++xbox360ps5::motion::camera_reads_requeued;  // fix 156e"));
  CHECK(Has(cam, "Virtual Kinect: {} frame reads with a {:X}-byte buffer (fix 156e)"));
  CHECK(Has(cam, "Virtual Kinect: Configure {:08X} {:08X} {:08X} {:08X} (+14..+20) (fix 156e)"));
}

int main() {
  CheckSizes();
  CheckJson();
  CheckTree();
  if (failures) {
    std::printf("check-camera-worker: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-camera-worker: ok\n");
  return 0;
}
