// SPDX-License-Identifier: MIT
// Fix 156l (tools/source-1.0.2/fixes/156l-nui-identity-answer.patch; Fighters Uncaged 5553084E,
// issue 67): Kinect ID for titles that identify the player themselves. Fighters Uncaged links
// the NUI library's in-title identification (NuiIdentityIdentify 827D8A50,
// NuiIdentityGetEnrollmentInformation 827D8B58); its face recognition never completed on the
// virtual camera, so the title stayed at "Identification in progress". Identify now answers the
// tracked body on the next skeleton frame: "frame processed", then "complete" with enrollment 0
// for the signed-in profile, which GetEnrollmentInformation reports.
//
//   check-nui-identity        run from an assembled tree's root
//
//  (a) the shapes: both functions found in synthetic code, a function missing one mark is not,
//      a mark in the next function is not borrowed;
//  (b) Identify's checks and queue (flags, untracked body, no callback, duplicates, the limit);
//      GetEnrollmentInformation; the profile slot;
//  (c) the messages, read by a model of Fighters Uncaged's callback (8219B110) and its Lua
//      getters: the result is set, enrollment 0, the profile's user index;
//  (d) the tree: the handlers, the binding with the skeleton function, the delivery after the
//      skeleton frame, the reset.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/nui_identity.hpp"

namespace ni = xbox360ps5::nui_identity;

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
static uint32_t LoadBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// PowerPC words.
constexpr uint32_t kMflr = 0x7D8802A6, kBlr = 0x4E800020, kNop = 0x60000000;
constexpr uint32_t kOriNotConnected = 0x6063048F;  // ori r3,r3,0x48F
constexpr uint32_t kOriInvalidArg = 0x60630057;    // ori r3,r3,0x57
constexpr uint32_t kMulliJob = 0x1D660150;         // mulli r11,r6,0x150
constexpr uint32_t kCmplwiR4 = 0x2B040000;         // cmplwi cr6,r4,0
constexpr uint32_t kLiR3One = 0x38600001;          // li r3,1
constexpr uint32_t kBlForward = 0x48000101;        // bl +0x100
constexpr uint32_t kCmplwiR3Eight = 0x2B030008;    // cmplwi cr6,r3,8
constexpr uint32_t kLiFE = 0x392000FE;             // li r9,0xFE
constexpr uint32_t kLis4000 = 0x3D604000;          // lis r11,0x4000
constexpr uint32_t kOriOne = 0x616B0001;           // ori r11,r11,1
constexpr uint32_t kOriFive = 0x60630005;          // ori r3,r3,5

struct Image {
  std::vector<uint8_t> bytes;
  uint32_t base = 0x82000000;
  uint32_t Put(std::initializer_list<uint32_t> words) {
    const uint32_t at = base + uint32_t(bytes.size());
    for (uint32_t w : words) {
      bytes.push_back(uint8_t(w >> 24));
      bytes.push_back(uint8_t(w >> 16));
      bytes.push_back(uint8_t(w >> 8));
      bytes.push_back(uint8_t(w));
    }
    return at;
  }
  xbox360ps5::motion::CodeView View() const { return {bytes.data(), bytes.size(), base}; }
};

static void CheckShapes() {
  Image image;
  image.Put({kMflr, kNop, kBlr});
  const uint32_t identify = image.Put({kMflr, kOriNotConnected, kCmplwiR4, kOriInvalidArg,
                                       kMulliJob, kLiR3One, kBlForward, kBlr});
  const uint32_t enrollment = image.Put({kMflr, kCmplwiR3Eight, kOriInvalidArg, kLiFE, kLis4000,
                                         kOriOne, kOriFive, kBlr});
  // Identify without its job call (li r3,1 not followed by bl): not it.
  image.Put({kMflr, kOriNotConnected, kCmplwiR4, kOriInvalidArg, kMulliJob, kLiR3One, kNop, kBlr});
  // Most of Identify, and the missing mark in the next function: not borrowed.
  image.Put({kMflr, kOriNotConnected, kCmplwiR4, kOriInvalidArg, kBlr});
  image.Put({kMflr, kMulliJob, kLiR3One, kBlForward, kBlr});
  ni::Found found = ni::FindIdentityFunctions(image.View());
  CHECK(found.identify.size() == 1 && found.identify[0] == identify);
  CHECK(found.enrollment.size() == 1 && found.enrollment[0] == enrollment);
  CHECK(found.ok());
  // A second Identify: neither is bound.
  image.Put({kMflr, kOriNotConnected, kCmplwiR4, kOriInvalidArg, kMulliJob, kLiR3One, kBlForward,
             kBlr});
  found = ni::FindIdentityFunctions(image.View());
  CHECK(found.identify.size() == 2 && !found.ok());
  // Only the enrollment information function (Kinect Sports Season Two, Zumba, DBZ).
  Image system_id;
  system_id.Put({kMflr, kCmplwiR3Eight, kOriInvalidArg, kLiFE, kLis4000, kOriOne, kOriFive, kBlr});
  found = ni::FindIdentityFunctions(system_id.View());
  CHECK(found.identify.empty() && found.enrollment.size() == 1 && !found.ok());
  CHECK(ni::FindIdentityFunctions({nullptr, 0, 0x82000000}).identify.empty());
}

static void CheckIdentify() {
  ni::Reset();
  const uint32_t body = 0x00010001, cb = 0x8219B110;
  CHECK(ni::Identify(body, 1, cb, 0, body) == ni::kInvalidArgument);
  CHECK(ni::Identify(body, 0, cb, 0, 0) == ni::kBodyNotTracked);         // no body tracked
  CHECK(ni::Identify(0x00020002, 0, cb, 0, body) == ni::kBodyNotTracked);  // another body
  CHECK(ni::Identify(0, 0, cb, 0, 0) == ni::kBodyNotTracked);
  CHECK(ni::Identify(body, 0, 0, 0, body) == ni::kInvalidArgument);
  CHECK(ni::TakePending().empty());
  CHECK(ni::Identify(body, 0, cb, 0, body) == ni::kOk);
  CHECK(ni::Identify(body, 0, cb, 0, body) == ni::kOk);  // the same request again: once
  auto taken = ni::TakePending();
  CHECK(taken.size() == 1 && taken[0].tracking_id == body && taken[0].callback == cb);
  CHECK(ni::TakePending().empty());
  for (uint32_t n = 0; n < 6; ++n) CHECK(ni::Identify(body, 0, cb, n, body) == ni::kOk);
  taken = ni::TakePending();
  CHECK(taken.size() == ni::kMaxPending && taken.front().context == 2 && taken.back().context == 5);
  CHECK(ni::Identify(body, 0, cb, 0, body) == ni::kOk);
  ni::Reset();
  CHECK(ni::TakePending().empty());

  CHECK(ni::ProfileSlot(1, 0x3) == 1);
  CHECK(ni::ProfileSlot(2, 0x3) == 0);  // the bound slot signed out: the first signed in
  CHECK(ni::ProfileSlot(ni::kUserIndexNone, 0x4) == 2);
  CHECK(ni::ProfileSlot(0, 0) == ni::kUserIndexNone);

  auto info = ni::GetEnrollmentInformation(0, true, 0);
  CHECK(info.result == ni::kOk && info.write && info.user_index == 0 && info.flags == ni::kEnrolled);
  info = ni::GetEnrollmentInformation(1, true, 0);
  CHECK(info.result == ni::kOk && info.write && info.user_index == ni::kUserIndexNone && !info.flags);
  info = ni::GetEnrollmentInformation(0, true, ni::kUserIndexNone);
  CHECK(info.write && info.user_index == ni::kUserIndexNone);
  info = ni::GetEnrollmentInformation(8, true, 0);
  CHECK(info.result == ni::kInvalidArgument && !info.write);
  info = ni::GetEnrollmentInformation(0, false, 0);
  CHECK(info.result == ni::kInvalidArgument && !info.write);
}

// Fighters Uncaged's result callback (8219B110) and Lua getters, reduced: the player is the
// slot of the tracking ID; "frame processed" sets the quality, "complete" the result, the
// enrollment and the auto-profile flag.
struct Title {
  uint32_t tracking[6] = {0x00010001, 0, 0, 0, 0, 0};
  bool quality_set[6] = {}, result_set[6] = {}, profile_set[6] = {};
  uint32_t quality[6] = {}, result[6] = {}, enrollment[6] = {};
  bool auto_profile[6] = {};
  int Callback(const uint8_t* m) {
    int player = -1;
    for (int n = 0; n < 6; ++n)
      if (tracking[n] && tracking[n] == LoadBE(m + 4)) player = n;
    if (player < 0) return 0;
    if (LoadBE(m) == 0) {
      quality[player] = LoadBE(m + 8);
      quality_set[player] = true;
      return 1;
    }
    if (LoadBE(m) == 1) {
      result[player] = LoadBE(m + 8);
      result_set[player] = true;
      enrollment[player] = LoadBE(m + 0xC);
      profile_set[player] = true;
      auto_profile[player] = LoadBE(m + 0x10) != 0;
    }
    return 0;
  }
};

static void CheckMessages() {
  for (uint32_t user : {0u, 2u, ni::kUserIndexNone}) {
    Title title;
    uint8_t m[ni::kMessageBytes];
    const auto answer = ni::AnswerFor(user);
    ni::WriteFrameProcessed(m, 0x00010001);
    CHECK(title.Callback(m) == 1);
    CHECK(title.quality_set[0] && title.quality[0] == 0 && !title.result_set[0]);
    ni::WriteComplete(m, 0x00010001, answer);
    CHECK(title.Callback(m) == 0);
    CHECK(title.result_set[0] && title.result[0] == ni::kOk);  // isIdentificationResultSetted
    const auto info = ni::GetEnrollmentInformation(title.enrollment[0], true, user);
    if (user < 4) {
      CHECK(title.enrollment[0] == 0 && title.auto_profile[0]);
      CHECK(info.user_index == user && info.flags == ni::kEnrolled);  // getEnrollmentUserIndex
    } else {
      CHECK(title.enrollment[0] == ni::kEnrollmentUnknown && !title.auto_profile[0]);
      CHECK(info.result == ni::kInvalidArgument);  // no enrollment to ask about
    }
    // Another body's message changes nothing.
    Title other;
    ni::WriteComplete(m, 0x00030003, answer);
    CHECK(other.Callback(m) == 0 && !other.result_set[0]);
  }
}

static void CheckTree() {
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  if (cam.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_pscam.cc)\n");
    ++failures;
    return;
  }
  CHECK(Has(cam, "#include \"xbox360ps5/nui_identity.hpp\"  // fix 156l"));
  CHECK(Has(cam, "void IdentityIdentifyHandler(cpu::ppc::PPCContext* ppc_context, KernelState* kernel_state) {"));
  CHECK(Has(cam, "void IdentityEnrollmentHandler(cpu::ppc::PPCContext* ppc_context, KernelState* kernel_state) {"));
  CHECK(Has(cam, "  if (found.identify.empty()) return;\n  if (!found.ok()) {"));
  CHECK(Has(cam, "  if (!BindTitleFunction(kernel_state, xex, enrollment, IdentityEnrollmentHandler) ||\n"
                 "      !BindTitleFunction(kernel_state, xex, identify, IdentityIdentifyHandler)) {"));
  CHECK(Has(cam, "  BindEstimatedHands(kernel_state, xex);\n  BindIdentity(kernel_state, xex, base, size);  // fix 156l"));
  // The answers after the skeleton frame and its identity-changed notice, outside the lock.
  const size_t notice = cam.find("++xbox360ps5::motion::nui_identity_notifications;");
  const size_t deliver = cam.find("  DeliverIdentityAnswers(kernel_state, binding.user_index);\n  return X_E_SUCCESS;");
  const size_t unlock = cam.rfind("  lock.unlock();", notice);
  CHECK(notice != std::string::npos && deliver != std::string::npos && notice < deliver);
  CHECK(unlock != std::string::npos && unlock < deliver);
  const size_t body = cam.find("void DeliverIdentityAnswers(");
  const std::string fn = body == std::string::npos ? "" : cam.substr(body, 2200);
  const size_t frame = fn.find("WriteFrameProcessed(m, r.tracking_id);");
  const size_t complete = fn.find("WriteComplete(m, r.tracking_id, answer);");
  CHECK(frame != std::string::npos && complete != std::string::npos && frame < complete);
  CHECK(Has(fn, "memory->SystemHeapFree(message);"));
  CHECK(Has(cam, "  xbox360ps5::motion::ResetVirtualNui();\n  xbox360ps5::nui_identity::Reset();  // fix 156l"));
  CHECK(Has(cam, "Virtual Kinect: the title's Kinect ID answers for the signed-in profile"));
}

int main() {
  CheckShapes();
  CheckIdentify();
  CheckMessages();
  CheckTree();
  if (failures) {
    std::printf("check-nui-identity: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-nui-identity: ok\n");
  return 0;
}
