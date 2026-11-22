// SPDX-License-Identifier: MIT
// Fix 99b (issue 15, part 2): the Kinect ID (NUI Identity) calls Kinect Sports makes as the
// player is picked answer as a Kinect ID with nothing stored, instead of undefined externs (which
// returned without setting r3) and E_FAIL from the identity messages, after which the game waited
// on "Pick main player".
//
//   check-kinect-identity        run from an assembled tree's root
//
// The tree's answers: each export defined once, with its result; the FE/2A004-2A009 messages
// succeed before the identity service's skeleton frames (2B002, still E_FAIL); the calls logged.
// 1.0.5-work-fix99b2: the frames the console refuses, counted by reason in the motion status, and
// the virtual Kinect's line when the title's body goes away.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "xbox360ps5/motion_input.hpp"

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The body of `name`'s _entry up to its DECLARE line.
std::string Body(const std::string& file, const std::string& name) {
  const size_t at = file.find("dword_result_t " + name + "_entry(");
  if (at == std::string::npos) return "";
  const size_t end = file.find("DECLARE_XAM_EXPORT1(" + name + ",", at);
  return end == std::string::npos ? "" : file.substr(at, end - at);
}

size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

// An XMP1 frame as the phone page packs it: sequence, session, flags, 20 joints of x, y, z, confidence.
std::string Packet(uint32_t sequence, uint32_t session, uint32_t flags, float z) {
  std::string bytes = "XMP1";
  auto put = [&](uint32_t v) {
    for (int n = 0; n < 4; ++n) bytes.push_back(char((v >> (8 * n)) & 0xFF));
  };
  auto putf = [&](float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put(v);
  };
  put(sequence);
  put(session);
  put(flags);
  for (int joint = 0; joint < 20; ++joint) {
    putf(0.1f);
    putf(0.2f);
    putf(z);
    putf(flags ? 0.9f : 0.0f);
  }
  return bytes;
}

void CheckMotionCounters() {
  namespace motion = xbox360ps5::motion;
  motion::SetEnabled(true);
  using R = motion::Result;
  CHECK(motion::Receive(Packet(1, 7, 1, 1.5f)) == R::accepted);
  CHECK(motion::Last().body && motion::Last().sequence == 1 && motion::Last().age_ms >= 0);
  CHECK(motion::Receive(Packet(1, 7, 1, 1.5f)) == R::replay);  // the same sequence again
  CHECK(motion::Receive(Packet(2, 7, 1, 4.5f)) == R::malformed);  // a joint beyond 4 m
  CHECK(motion::Receive(std::string_view("XMP0")) == R::malformed);
  CHECK(motion::Receive(Packet(3, 7, 0, 0.0f)) == R::accepted);  // no body: a loss
  CHECK(motion::Receive(Packet(4, 7, 0, 0.0f)) == R::accepted);  // still none: not another loss
  CHECK(!motion::Last().body && motion::Last().sequence == 4);
  CHECK(motion::refused_replay == 1 && motion::refused_out_of_range == 1 && motion::refused_malformed == 1);
  CHECK(motion::frames_without_body == 2 && motion::bodies_lost == 1);
  const std::string status = motion::Status();
  CHECK(status.find("\"refused\":{\"malformed\":1,\"out_of_range\":1,\"replay\":1,\"disabled\":0}") !=
        std::string::npos);
  // Fix 119 adds "picture" at the end.
  CHECK(status.find(",\"frames_without_body\":2,\"bodies_lost\":1}") != std::string::npos ||
        status.find(",\"frames_without_body\":2,\"bodies_lost\":1,\"picture\":0}") != std::string::npos);
  CHECK(status.find("\"packets\":3,") != std::string::npos);  // only accepted frames count
  motion::SetEnabled(false);
  CHECK(motion::Receive(Packet(5, 7, 1, 1.5f)) == R::disabled && motion::refused_disabled == 1);
}

int main() {
  CheckMotionCounters();
  const std::string nui = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_nui.cc");
  const std::string app = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/apps/xam_app.cc");
  if (nui.empty() || app.empty()) {
    std::printf("no assembled tree here; nothing checked\n");
    return 0;
  }
  // Each call with the answer of a Kinect ID that works with nothing stored.
  const struct {
    const char* name;
    const char* result;
  } calls[] = {
      {"XamReadBiometricData", "return X_ERROR_NOT_FOUND;"},
      {"XamWriteBiometricData", "return X_ERROR_SUCCESS;"},
      {"XamUserNuiEnableBiometric", "return X_ERROR_SUCCESS;"},
      {"XamNuiCameraElevationSetAngle", "return X_ERROR_SUCCESS;"},
      {"XamCacheOpenFile", "return X_ERROR_FILE_NOT_FOUND;"},
      {"XamCacheCloseFile", "return X_ERROR_SUCCESS;"},
      {"XamCacheReset", "return X_ERROR_SUCCESS;"},
      {"XamXlfsInitializeUploadQueue", "return X_E_FAIL;"},
      {"XamXlfsUninitializeUploadQueue", "return X_ERROR_SUCCESS;"},
      {"XamXlfsMountUploadQueueInstance", "return X_E_FAIL;"},
      {"XamXlfsUnmountUploadQueueInstance", "return X_ERROR_SUCCESS;"},
  };
  for (const auto& call : calls) {
    const std::string body = Body(nui, call.name);
    if (body.empty()) std::printf("missing: %s\n", call.name);
    CHECK(!body.empty());
    CHECK(body.find(call.result) != std::string::npos);
    CHECK(body.find(std::string("KinectIdentityLog(\"") + call.name + "\"") != std::string::npos);
    CHECK(Count(nui, std::string("DECLARE_XAM_EXPORT1(") + call.name + ",") == 1);
  }
  // No biometric data: the size reads 0.
  CHECK(Body(nui, "XamReadBiometricData").find("if (size_ptr) *size_ptr = 0;") != std::string::npos);
  // The old answer of XamUserNuiEnableBiometric is gone.
  CHECK(Body(nui, "XamUserNuiEnableBiometric").find("return X_E_INVALIDARG;") == std::string::npos);
  CHECK(nui.find("void KinectIdentityLog(const char* call, uint32_t a, uint32_t b, uint32_t result) {") !=
        std::string::npos);
  // The identity messages succeed; the skeleton frames to the service (2B002) still fail.
  const size_t a004 = app.find("case 0x0002A004:");
  const size_t a005 = app.find("case 0x0002A005:");
  const size_t a008 = app.find("case 0x0002A009: {");
  const size_t b002 = app.find("case 0x0002B002: {");
  CHECK(a004 != std::string::npos && a004 < a005);
  CHECK(a005 != std::string::npos && a005 < a008 && a008 < b002 && b002 != std::string::npos);
  CHECK(app.find("case 0x0002A008:\n") != std::string::npos);
  const std::string messages = a008 != std::string::npos && b002 != std::string::npos && a008 < b002
                                   ? app.substr(a008, b002 - a008)
                                   : std::string();
  CHECK(messages.find("return X_E_SUCCESS;") != std::string::npos);
  CHECK(messages.find("(fix 99b)") != std::string::npos);
  CHECK(app.find("answered E_FAIL (said once)") != std::string::npos);
  // 1.0.5-work-fix99b2: the body going away, said with the phone's last frame and the refusals.
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  CHECK(cam.find("\"Virtual Kinect: the body is gone: the phone's last frame came {} ms ago ({}, sequence \"") !=
        std::string::npos);
  CHECK(cam.find("xbox360ps5::motion::RefusedJson());") != std::string::npos);
  CHECK(cam.find("if (had_body && !pose.tracked && losses_shown < 32) {") != std::string::npos);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-identity: all passed\n");
  return 0;
}
