// SPDX-License-Identifier: MIT
// Fix 156 (tools/source-1.0.2/fixes/156-nui-status-connected.patch; Zumba Fitness Core,
// 4D4A07F0, issue 63): the virtual Kinect is connected once the guest opened its camera and the
// title's skeleton frame function is bound, before any frame was delivered. Zumba asks
// XamNuiGetDeviceStatus once, right after its NUI start-up (1.0.5 log: "first call of
// XamNuiGetDeviceStatus from 8259A86C", before its first frame); the answer needed a frame in
// the last 500 ms, so it was told there was no camera and never read a skeleton
// (guest_skeleton_frames 0, "can't find the camera").
//
//   check-nui-device-status        run from an assembled tree's root
//
//  (a) VirtualNuiDeviceConnected: a Kinect title, "Camera motion" on, the camera open and the
//      skeleton bound; no frame needed, and one frame long ago still connected; any of the four
//      missing is not connected;
//  (b) the 500 ms rule stays for the player binding: VirtualNuiCameraLive and LiveNuiBinding
//      want a frame just delivered;
//  (c) VirtualNuiStatus: connected is success, camera and tilt success, no microphone,
//      status bits 5 (1, the system's "ready" bit, and 4, the earlier answer); not connected
//      is 0xC0050006 with no bits;
//  (d) the tree: XamNuiGetDeviceStatus and XamNuiIsDeviceReady answer from it in the virtual
//      mode, the first 16 answers and then each change are logged, LiveNuiBinding keeps the
//      frame rule;
//  (e) a title that polls from before its camera opens (Harry Potter Kinect 57520826, issue 66:
//      XamNuiGetDeviceStatus every frame from 82C32A54, the camera opened later): "not
//      connected" until the camera opens, then connected on the next poll with no frame ever
//      delivered; and one asked once with all set (Nike+ Kinect Training 4D53090F, its speech
//      library 82DA617C): connected.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "xbox360ps5/virtual_nui.hpp"

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
// The text from `start` to the end of its function (the next line that is just "}").
static std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

static void SetDevice(bool enabled, bool open, bool bound) {
  m::enabled = enabled;
  m::guest_camera_open = open;
  m::guest_skeleton_bound = bound;
}

static void CheckConnected() {
  m::ResetVirtualNui();
  SetDevice(true, true, true);
  // (a) no frame delivered yet: connected (Zumba's one early question).
  CHECK(m::VirtualNuiDeviceConnected(true));
  CHECK(!m::VirtualNuiDeviceConnected(false));  // not a Kinect title
  SetDevice(false, true, true);
  CHECK(!m::VirtualNuiDeviceConnected(true));  // "Camera motion" off
  SetDevice(true, false, true);
  CHECK(!m::VirtualNuiDeviceConnected(true));  // the guest's camera not open
  SetDevice(true, true, false);
  CHECK(!m::VirtualNuiDeviceConnected(true));  // the skeleton function not bound
  SetDevice(true, true, true);

  // (b) the player binding still wants a frame just delivered.
  CHECK(!m::VirtualNuiCameraLive(true));
  CHECK(m::LiveNuiBinding(true, 1).user_index == m::kUserIndexNone);
  m::Frame body;
  body.session = 7;
  body.tracked = true;
  m::PublishNuiFrame(body, 0x1234, 1);
  CHECK(m::VirtualNuiCameraLive(true));
  CHECK(m::VirtualNuiDeviceConnected(true));
  std::this_thread::sleep_for(std::chrono::milliseconds(650));
  CHECK(!m::VirtualNuiCameraLive(true));      // the frame is old now
  CHECK(m::VirtualNuiDeviceConnected(true));  // the device is still there
  m::ResetVirtualNui();
  CHECK(m::VirtualNuiDeviceConnected(true));
}

static void CheckStatus() {
  const m::NuiDeviceStatus on = m::VirtualNuiStatus(true);
  CHECK(on.result == 0);
  CHECK(on.camera_status == 0 && on.tilt_status == 0);
  CHECK(on.mic_status == 0xC000009Du);
  CHECK(on.status_bits == 5u && (on.status_bits & 1u) && (on.status_bits & 4u));
  CHECK(!(on.status_bits & 0x40u));  // never "4 and 0x40 without 1" (the troubleshooter)
  const m::NuiDeviceStatus off = m::VirtualNuiStatus(false);
  CHECK(off.result == 0xC0050006u);
  CHECK(off.status_bits == 0 && off.tilt_status == 0xC000009Du && off.mic_status == 0xC000009Du);
}

static void CheckTree() {
  const std::string nui = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_nui.cc");
  const std::string header = ReadText("include/xbox360ps5/virtual_nui.hpp");
  if (nui.empty() || header.empty()) {
    std::printf("FAIL: no assembled tree here (xam_nui.cc, virtual_nui.hpp)\n");
    ++failures;
    return;
  }
  const std::string status = Body(nui, "dword_result_t XamNuiGetDeviceStatus_entry(");
  const size_t virtual_mode = status.find("if (VirtualNuiMode()) {");
  const size_t answer = status.find(
      "xbox360ps5::motion::VirtualNuiStatus(\n        xbox360ps5::motion::VirtualNuiDeviceConnected(true));");
  const size_t returned = status.find("    return status.result;\n  }");
  CHECK(virtual_mode != std::string::npos && answer != std::string::npos &&
        returned != std::string::npos && virtual_mode < answer && answer < returned);
  CHECK(Has(status, "status_ptr->status_bits = status.status_bits;") &&
        Has(status, "status_ptr->tilt_status = status.tilt_status;") &&
        Has(status, "status_ptr->mic_status = status.mic_status;"));
  CHECK(Has(status, "const bool changed = last.exchange(status.result) != status.result;") &&
        Has(status, "if (shown.fetch_add(1) < 16 || (changed && changes.fetch_add(1) < 16))") &&
        Has(status, "Virtual Kinect: XamNuiGetDeviceStatus -> {:08X}, status bits {:X}"));
  CHECK(!Has(status, "VirtualNuiCameraLive"));
  const std::string ready = Body(nui, "dword_result_t XamNuiIsDeviceReady_entry(");
  CHECK(Has(ready, "    return xbox360ps5::motion::VirtualNuiDeviceConnected(true);"));
  CHECK(!Has(ready, "VirtualNuiCameraLive"));
  const std::string binding = Body(header, "inline NuiBinding LiveNuiBinding(");
  CHECK(Has(binding, "if (!VirtualNuiCameraLive(kinect_title)) return {};"));
  CHECK(Has(header, "constexpr uint32_t kVirtualNuiStatusBits = 0x5;"));
}

// (e) the answer a title gets from XamNuiGetDeviceStatus on each poll.
static uint32_t Poll() { return m::VirtualNuiStatus(m::VirtualNuiDeviceConnected(true)).result; }

static void CheckLateCamera() {
  m::ResetVirtualNui();
  // Harry Potter: the skeleton function bound at load, "Camera motion" on, the camera not open.
  SetDevice(true, false, true);
  for (int frame = 0; frame < 30; ++frame) CHECK(Poll() == 0xC0050006u);
  m::guest_camera_open = true;  // the guest opens its camera (PsCamDeviceRequest)
  CHECK(Poll() == 0u);          // the next poll: connected, no frame delivered
  CHECK(!m::VirtualNuiCameraLive(true));
  for (int frame = 0; frame < 30; ++frame) CHECK(Poll() == 0u);
  CHECK(m::VirtualNuiStatus(true).status_bits == 5u);
  // Nike+: one question, with the camera open and the skeleton bound, no frame yet.
  m::ResetVirtualNui();
  SetDevice(true, true, true);
  CHECK(Poll() == 0u);
}

int main() {
  CheckConnected();
  CheckLateCamera();
  CheckStatus();
  CheckTree();
  if (failures) {
    std::printf("check-nui-device-status: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-nui-device-status: ok\n");
  return 0;
}
