// SPDX-License-Identifier: MIT
// Fix 108 (Kinect Sports, issue 15): what a Kinect game asks the system for after "Pick main player".
//
//   check-kinect-diag        run from an assembled tree's root
//
//  (a) xbox360ps5/kinect_diag.hpp: the exports whose first call is logged (avatar, user, profile,
//      content, NUI, XUser, XamShow) and not others; each name once, at most 256, again after a
//      reset; a thread's snapshot at once and then once per 10 s; the back chain from a saved LR
//      word 8 bytes below each caller's frame, stopping at a word it cannot read, at a chain that
//      does not go up, or that jumps 1 MiB or more; the lines' formats;
//  (b) the tree: the exports' trampoline calls it only while on, reading guest words where the
//      memory allows; an empty file name adds 8 callers and its buffer's bytes; the virtual Kinect
//      turns it on when a Kinect title starts NUI and off with the game.
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
  return s.str();
}

int main() {
  // (a) The names.
  for (const char* name : {"XamAvatarGetAssets", "XamUserGetSigninState", "XamProfileFindAccount", "XamContentCreateEx",
                           "XContentGetDeviceData", "XamNuiIdentityEnroll", "XUserReadProfileSettings", "XamShowSigninUI"})
    CHECK(kd::Watched(name));
  for (const char* name : {"NtCreateFile", "KeWaitForSingleObject", "XamInputGetState", "XAudioSubmitRenderDriverFrame", "Xam", ""})
    CHECK(!kd::Watched(name));
  CHECK(!kd::Watched(nullptr));
  {
    kd::FirstCalls calls;
    CHECK(calls.First("XamAvatarGetAssets") && !calls.First("XamAvatarGetAssets") && calls.First("XamUserGetXUID"));
    for (int n = 0; n < 300; ++n) calls.First(("XamUserX" + std::to_string(n)).c_str());
    CHECK(!calls.First("XamUserNotSeenBefore"));  // at most 256 names
    calls.Reset();
    CHECK(calls.First("XamAvatarGetAssets"));
  }
  {
    int64_t last = 0;
    CHECK(kd::SnapshotDue(5000, last) && last == 5000);
    CHECK(!kd::SnapshotDue(5001, last) && !kd::SnapshotDue(14999, last));
    CHECK(kd::SnapshotDue(15000, last) && last == 15000);
    int64_t other = 0;
    CHECK(kd::SnapshotDue(15001, other));  // another thread's slot
    int64_t zero = 0;
    CHECK(kd::SnapshotDue(0, zero) && zero == 1);  // a clock at 0 still marks the slot
  }
  {
    // A stack: frames at 0x7000F000 -> 0x7000F100 -> 0x7000F200 -> 0x7000F300, each caller's saved
    // LR 8 bytes below the next frame.
    std::map<uint32_t, uint32_t> memory = {
        {0x7000F000, 0x7000F100}, {0x7000F0F8, 0x82000010},
        {0x7000F100, 0x7000F200}, {0x7000F1F8, 0x82000020},
        {0x7000F200, 0x7000F300}, {0x7000F2F8, 0x82000030},
        {0x7000F300, 0x7000F300}};  // the top: a frame that points at itself
    auto read = [&](uint32_t address, uint32_t* value) {
      auto it = memory.find(address);
      if (it == memory.end()) return false;
      *value = it->second;
      return true;
    };
    uint32_t out[8] = {};
    int n = kd::BackChain(0x82A388A8, 0x7000F000, read, out, 8);
    CHECK(n == 4 && out[0] == 0x82A388A8 && out[1] == 0x82000010 && out[2] == 0x82000020 && out[3] == 0x82000030);
    CHECK(kd::Chain(out, n) == "82A388A8 < 82000010 < 82000020 < 82000030");
    CHECK(kd::BackChain(1, 0x7000F000, read, out, 2) == 2);   // as many as asked
    CHECK(kd::BackChain(1, 0x7000E000, read, out, 8) == 1);   // an sp it cannot read: the LR only
    memory[0x7000F100] = 0x7000F000;                           // a chain going down
    CHECK(kd::BackChain(1, 0x7000F000, read, out, 8) == 2);
    memory[0x7000F100] = 0x7020F100;                           // a jump of 2 MiB
    CHECK(kd::BackChain(1, 0x7000F000, read, out, 8) == 2);
    memory.erase(0x7000F0F8);                                  // a saved LR it cannot read
    CHECK(kd::BackChain(1, 0x7000F000, read, out, 8) == 1);
    CHECK(kd::BackChain(1, 0x7000F000, read, out, 0) == 0);
  }
  {
    const uint8_t bytes[] = {0x00, 0x4F, 0xFF};
    CHECK(kd::Hex(bytes, 3) == "00 4F FF" && kd::Hex(bytes, 0).empty());
  }

  // (b) The tree.
  const std::string shim = ReadText(".deps/xenia-canary/src/xenia/kernel/util/shim_utils.h");
  const std::string io = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc");
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  if (shim.empty() || io.empty() || cam.empty()) {
    std::printf("no assembled tree here; the tree not checked\n");
  } else {
    const size_t note = shim.find("xbox360ps5::hang_watch::NoteGuestCall(export_entry->name);  // fix 47");
    const size_t hook = shim.find("if (xbox360ps5::kinect_diag::enabled.load(std::memory_order_relaxed))\n"
                                  "          KinectDiagCall(export_entry->name, ppc_context);  // fix 108");
    CHECK(note != std::string::npos && hook != std::string::npos && note < hook && hook - note < 200);
    CHECK(shim.find("if (!heap || !heap->QueryProtect(address, &protection) || !(protection & kMemoryProtectRead))") !=
          std::string::npos);
    CHECK(shim.find("\"Kinect diag: first call of {} from {} (r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X}), thread {} (fix 108)\"") !=
          std::string::npos);
    CHECK(shim.find("\"Kinect diag: thread {} calls {} from {} (fix 108)\"") != std::string::npos);
    CHECK(io.find("if (raw.empty() && name) {") != std::string::npos &&
          io.find("shim::KinectDiagRead, deep, kd::kFrames);") != std::string::npos);
    CHECK(io.find("\"Guest path: the empty name's callers {}; its buffer {:08X} (length {}, maximum {}), 32 bytes \"") !=
          std::string::npos);
    const size_t bind = cam.find("void BindVirtualKinectEarly(KernelState* kernel_state) {");
    const size_t on = cam.find("if (!xbox360ps5::kinect_diag::enabled.exchange(true))");
    CHECK(bind != std::string::npos && on > bind && on - bind < 400);
    CHECK(cam.find("xbox360ps5::kinect_diag::enabled = false;  // fix 108") != std::string::npos &&
          cam.find("xbox360ps5::kinect_diag::first_calls.Reset();") != std::string::npos);
    std::printf("tree: the trampoline, the empty name's report and the virtual Kinect use it\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-diag: all passed\n");
  return 0;
}
