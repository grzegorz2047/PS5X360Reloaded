// SPDX-License-Identifier: MIT
// Fix 156f (tools/source-1.0.2/fixes/156f-mic-array-stubs.patch; Kinect titles, issues 63/66):
// the Kinect microphone array, not emulated, answered instead of an undefined extern.
// XamVoiceGetMicArrayAudioEx (xam 0x490) was an undefined extern: it returned without setting
// r3, so Kinect Sports Season Two's speech capture (831FF140, 831FF184: `cmpwi r3,0; blt` to
// its error path) took its own first argument, a pointer, for success. With fix 156's ready
// bit (0x2) Season Two starts that capture (82AAD368). XamVoiceSetMicArrayIdleUsers (xam
// 0x48C) had no function for XexGetProcedureAddress (Fighters Uncaged, Season Two).
//
//   check-mic-array        run from an assembled tree's root
//
//  the tree: both exports declared in xam_voice.cc and listed at their ordinals; the audio
//  request answers 0x8007048F (negative, "device not connected") and writes nothing; the
//  idle-users call answers success; each call is logged (16 lines in all).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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

int main() {
  // A guest HRESULT is negative when its top bit is set: the capture's `blt` takes it.
  constexpr uint32_t kNotConnected = 0x8007048F;
  CHECK(int32_t(kNotConnected) < 0);
  const std::string voice = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_voice.cc");
  const std::string table = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_table.inc");
  if (voice.empty() || table.empty()) {
    std::printf("FAIL: no assembled tree here (xam_voice.cc, xam_table.inc)\n");
    return 1;
  }
  CHECK(Has(table, "XE_EXPORT(xam,      0x00000490, XamVoiceGetMicArrayAudioEx,"));
  CHECK(Has(table, "XE_EXPORT(xam,      0x0000048C, XamVoiceSetMicArrayIdleUsers,"));
  CHECK(Has(voice, "constexpr uint32_t kMicArrayNotConnected = 0x8007048F;"));
  const std::string audio = Body(voice, "dword_result_t XamVoiceGetMicArrayAudioEx_entry(");
  CHECK(Has(audio, "return kMicArrayNotConnected;"));
  CHECK(!Has(audio, "Zero()") && !Has(audio, "*a"));  // writes nothing back
  CHECK(Has(voice, "DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayAudioEx, kNone, kStub);"));
  const std::string idle = Body(voice, "dword_result_t XamVoiceSetMicArrayIdleUsers_entry(");
  CHECK(Has(idle, "return X_ERROR_SUCCESS;"));
  CHECK(Has(voice, "DECLARE_XAM_EXPORT1(XamVoiceSetMicArrayIdleUsers, kNone, kStub);"));
  CHECK(Has(voice, "Kinect microphone: {}({:08X}, {:08X}) -> {:08X}, no microphone array "));
  CHECK(Has(voice, "if (shown.fetch_add(1) < 16)"));
  // The array's status still says "not connected" (unchanged).
  CHECK(Has(Body(voice, "dword_result_t XamVoiceGetMicArrayStatus_entry("), "return 0;"));
  if (failures) {
    std::printf("check-mic-array: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-mic-array: ok\n");
  return 0;
}
