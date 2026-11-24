// SPDX-License-Identifier: MIT
// Fix 156c (tools/source-1.0.2/fixes/156c-nui-elevation-angle.patch; Kinect Adventures 4D5308ED
// and Kinect Sports Season Two 4D5309D6, issue 63): the virtual camera's tilt motor.
// XamNuiCameraElevationGetAngle always failed, and Kinect Adventures moves the motor on its
// start screen (SetAngle +4, then +12, 1.0.5 log) and polls the angle every frame. It also
// handed a struct on the host's stack to XamNuiCameraTiltGetStatus_entry, which takes a guest
// pointer.
//
//   check-nui-elevation        run from an assembled tree's root
//
//  (a) the angle: level (0) at first and after ResetVirtualNui; the last SetAngle target,
//      clamped to +-27 degrees (Zumba's -25 as the guest passes it, 0xFFFFFFE7, is -25);
//      never moving;
//  (b) the tree: GetAngle answers from it with success while the guest's camera is open in the
//      virtual mode (both pointers checked, camera_moving may be null), and fails otherwise
//      as before; SetAngle stores the target in the virtual mode and still succeeds; no
//      host-stack struct is passed to XamNuiCameraTiltGetStatus_entry any more (GetAngle,
//      SetFlags).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
static std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

static void CheckAngle() {
  m::ResetVirtualNui();
  CHECK(m::VirtualTiltAngle().angle == 0 && m::VirtualTiltAngle().moving == 0);
  m::SetVirtualTiltAngle(4);
  CHECK(m::VirtualTiltAngle().angle == 4);
  m::SetVirtualTiltAngle(12);
  CHECK(m::VirtualTiltAngle().angle == 12 && m::VirtualTiltAngle().moving == 0);
  m::SetVirtualTiltAngle(int32_t(uint32_t(0xFFFFFFE7u)));
  CHECK(m::VirtualTiltAngle().angle == -25);
  m::SetVirtualTiltAngle(27);
  CHECK(m::VirtualTiltAngle().angle == 27);
  m::SetVirtualTiltAngle(28);
  CHECK(m::VirtualTiltAngle().angle == 27);
  m::SetVirtualTiltAngle(-1000);
  CHECK(m::VirtualTiltAngle().angle == -27);
  m::SetVirtualTiltAngle(int32_t(uint32_t(0x7018EBE4u)));  // a pointer-like value
  CHECK(m::VirtualTiltAngle().angle == 27);
  CHECK(m::ClampTiltAngle(INT32_MIN) == -27 && m::ClampTiltAngle(INT32_MAX) == 27);
  CHECK(m::ClampTiltAngle(-27) == -27 && m::ClampTiltAngle(0) == 0);
  m::ResetVirtualNui();
  CHECK(m::VirtualTiltAngle().angle == 0);
}

static void CheckTree() {
  const std::string nui = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_nui.cc");
  if (nui.empty()) {
    std::printf("FAIL: no assembled tree here (xam_nui.cc)\n");
    ++failures;
    return;
  }
  const std::string get = Body(nui, "dword_result_t XamNuiCameraElevationGetAngle_entry(");
  const size_t mode = get.find("if (VirtualNuiMode() && xbox360ps5::motion::guest_camera_open.load()) {");
  const size_t checked = get.find("if (!NuiWritable(elevation_angle.guest_address(), 4) ||\n"
                                  "        (camera_moving && !NuiWritable(camera_moving.guest_address(), 4)))");
  const size_t angle = get.find("const auto tilt = xbox360ps5::motion::VirtualTiltAngle();");
  const size_t wrote = get.find("*elevation_angle = uint32_t(tilt.angle);");
  const size_t ok = get.find("    return X_ERROR_SUCCESS;\n  }");
  CHECK(mode != std::string::npos && checked != std::string::npos && angle != std::string::npos &&
        wrote != std::string::npos && ok != std::string::npos && mode < checked &&
        checked < angle && angle < wrote && wrote < ok);
  CHECK(Has(get, "if (camera_moving) *camera_moving = tilt.moving;"));
  CHECK(get.find("return X_E_FAIL;", ok) != std::string::npos);  // outside it, as before
  CHECK(Has(get, "Virtual Kinect: XamNuiCameraElevationGetAngle -> {} degrees, not moving"));
  // No struct on the host's stack for a function that takes a guest pointer.
  CHECK(!Has(nui, "XamNuiCameraTiltGetStatus_entry(&"));
  const std::string flags = Body(nui, "dword_result_t XamNuiCameraSetFlags_entry(");
  CHECK(!flags.empty() && !Has(flags, "X_NUI_TILT_STATUS tilt_status"));
  const std::string set = Body(nui, "dword_result_t XamNuiCameraElevationSetAngle_entry(");
  CHECK(Has(set, "  if (VirtualNuiMode())\n    xbox360ps5::motion::SetVirtualTiltAngle(int32_t(uint32_t(angle)));"));
  CHECK(Has(set, "return X_ERROR_SUCCESS;"));
  const std::string header = ReadText("include/xbox360ps5/virtual_nui.hpp");
  CHECK(Has(Body(header, "inline void ResetVirtualNui() {"), "nui_tilt_angle = 0;"));
}

int main() {
  CheckAngle();
  CheckTree();
  if (failures) {
    std::printf("check-nui-elevation: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-nui-elevation: ok\n");
  return 0;
}
