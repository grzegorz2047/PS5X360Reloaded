// SPDX-License-Identifier: MIT
// Fix 156l (tools/source-1.0.2/fixes/156l-nui-identity-answer.patch; Fighters Uncaged 5553084E,
// issue 67): Kinect ID for titles that identify the player themselves. Fighters Uncaged links
// the NUI library's in-title identification (NuiIdentityIdentify 827D8A50,
// NuiIdentityGetEnrollmentInformation 827D8B58); its face recognition never completed on the
// virtual camera, so the title stayed at "Identification in progress". Identify now answers the
// tracked body on the next skeleton frame: "frame processed", then "complete" with enrollment 0
// for the signed-in profile, which GetEnrollmentInformation reports. Fix 156n
// (156n-identify-pending.patch): Identify answers E_PENDING, as the library does for a queued
// job (the script waits for the callback only then), and the answer comes kAnswerDelayFrames
// skeleton frames later, after the script's wait cleared its result tables.
//
//   check-nui-identity        run from an assembled tree's root
//
//  (a) the shapes: both functions found in synthetic code, a function missing one mark is not,
//      a mark in the next function is not borrowed;
//  (b) Identify's checks and queue (flags, untracked body, no callback, duplicates, the limit,
//      E_PENDING and the delay);
//      GetEnrollmentInformation; the profile slot;
//  (c) the messages, read by a model of Fighters Uncaged's callback (8219B110) and its Lua
//      getters: the result is set, enrollment 0, the profile's user index;
//  (d) a model of the script's identification states: 156l's S_OK never reaches the wait,
//      156n's E_PENDING and delayed answer are read;
//  (e) fix 156q (156q-nui-identity-newer-build.patch; Zumba Fitness 4D4A07F0, #63): the
//      library's newer build (0x160-byte job, flags check allowing 0x20000000) is found with
//      its layout; its flags; its message layout as Zumba's callback (82331FE8) reads it;
//  (f) the tree: the handlers, the binding with the skeleton function, the delivery after the
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
// Fix 156q: the library's newer build (Zumba Fitness 8251D6D8, Dragon Ball Z 824997F0).
constexpr uint32_t kMulliJobNew = 0x1D660160;      // mulli r11,r6,0x160
constexpr uint32_t kRlwinmFlags = 0x57AB00C3;      // rlwinm. r11,r29,0,3,1

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

// Fix 156q: the newer build is found, with its layout; the older one keeps its own.
static void CheckNewBuild() {
  Image image;
  const uint32_t identify = image.Put({kMflr, kOriNotConnected, kRlwinmFlags, kOriInvalidArg,
                                       kMulliJobNew, kLiR3One, kBlForward, kBlr});
  const uint32_t enrollment = image.Put({kMflr, kCmplwiR3Eight, kOriInvalidArg, kLiFE, kLis4000,
                                         kOriOne, kOriFive, kBlr});
  ni::Found found = ni::FindIdentityFunctions(image.View());
  CHECK(found.ok() && found.identify[0] == identify && found.enrollment[0] == enrollment);
  CHECK(found.layout == ni::Layout::kNew);
  // Neither flags check: not it.
  Image no_flags;
  no_flags.Put({kMflr, kOriNotConnected, kOriInvalidArg, kMulliJobNew, kLiR3One, kBlForward, kBlr});
  CHECK(ni::FindIdentityFunctions(no_flags.View()).identify.empty());
  Image old;
  old.Put({kMflr, kOriNotConnected, kCmplwiR4, kOriInvalidArg, kMulliJob, kLiR3One, kBlForward, kBlr});
  CHECK(ni::FindIdentityFunctions(old.View()).layout == ni::Layout::kOld);
  // Flags: the newer build allows 0x20000000, the older none.
  ni::Reset();
  const uint32_t body = 0x00010001;
  CHECK(ni::Identify(body, 0x20000000, 0x82331FE8, 0, body, ni::AllowedFlags(ni::Layout::kNew)) ==
        ni::kPending);
  CHECK(ni::Identify(body, 0x20000000, 0x8219B110, 0, body, ni::AllowedFlags(ni::Layout::kOld)) ==
        ni::kInvalidArgument);
  CHECK(ni::Identify(body, 1, 0x82331FE8, 0, body, ni::AllowedFlags(ni::Layout::kNew)) ==
        ni::kInvalidArgument);
  ni::Reset();
  // The newer layout, as Zumba's callback (82331FE8) reads it: +0 kind, +8 tracking ID;
  // complete: +0x10 result (0), +0x14 enrollment (< 8), +0x18 matched (1); frame processed:
  // +0x10 quality issues.
  uint8_t m[ni::kMessageBytes];
  ni::WriteComplete(m, body, ni::AnswerFor(0), ni::Layout::kNew);
  CHECK(LoadBE(m) == 1 && LoadBE(m + 8) == body && LoadBE(m + 0x10) == 0 &&
        LoadBE(m + 0x14) == 0 && LoadBE(m + 0x18) == 1);
  CHECK(LoadBE(m + 4) == 0 && LoadBE(m + 0xC) == 0);
  ni::WriteFrameProcessed(m, body, ni::Layout::kNew);
  CHECK(LoadBE(m) == 0 && LoadBE(m + 8) == body && LoadBE(m + 0x10) == 0);
  // The older layout is unchanged (Fighters Uncaged's 8219B110).
  ni::WriteComplete(m, body, ni::AnswerFor(0), ni::Layout::kOld);
  CHECK(LoadBE(m + 4) == body && LoadBE(m + 8) == 0 && LoadBE(m + 0xC) == 0 && LoadBE(m + 0x10) == 1);
  CHECK(ni::kMessageBytes >= 0x1C);
}

static void CheckIdentify() {
  ni::Reset();
  const uint32_t body = 0x00010001, cb = 0x8219B110;
  CHECK(ni::Identify(body, 1, cb, 0, body) == ni::kInvalidArgument);
  CHECK(ni::Identify(body, 0, cb, 0, 0) == ni::kBodyNotTracked);         // no body tracked
  CHECK(ni::Identify(0x00020002, 0, cb, 0, body) == ni::kBodyNotTracked);  // another body
  CHECK(ni::Identify(0, 0, cb, 0, 0) == ni::kBodyNotTracked);
  CHECK(ni::Identify(body, 0, 0, 0, body) == ni::kInvalidArgument);
  CHECK(ni::TakeDue().empty());
  // Fix 156n: a queued request answers E_PENDING (the script waits only then) and is due
  // kAnswerDelayFrames skeleton frames later.
  CHECK(ni::Identify(body, 0, cb, 0, body) == ni::kPending);
  CHECK(ni::Identify(body, 0, cb, 0, body) == ni::kPending);  // the same request again: once
  for (uint32_t f = 1; f < ni::kAnswerDelayFrames; ++f) CHECK(ni::TakeDue().empty());
  auto taken = ni::TakeDue();
  CHECK(taken.size() == 1 && taken[0].tracking_id == body && taken[0].callback == cb);
  CHECK(ni::TakeDue().empty());
  for (uint32_t n = 0; n < 6; ++n) CHECK(ni::Identify(body, 0, cb, n, body) == ni::kPending);
  for (uint32_t f = 1; f < ni::kAnswerDelayFrames; ++f) ni::TakeDue();
  taken = ni::TakeDue();
  CHECK(taken.size() == ni::kMaxPending && taken.front().context == 2 && taken.back().context == 5);
  CHECK(ni::Identify(body, 0, cb, 0, body) == ni::kPending);
  ni::Reset();
  for (uint32_t f = 0; f <= ni::kAnswerDelayFrames; ++f) CHECK(ni::TakeDue().empty());

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

// Fighters Uncaged's PlayerIdentifier (scripts/AllScripts.s), reduced: frame 1 calls
// tryIdentifySkeleton and waits only on E_PENDING; entering the wait (the next update) clears
// the results; each update the result is read; a skeleton frame (and the answers due then)
// comes before each update.
static bool ScriptIdentifies(bool pending_answer, uint32_t delay) {
  ni::Reset();
  Title title;
  enum { Identify, EnterWait, Wait, Done } state = Identify;
  std::vector<std::pair<uint32_t, ni::Request>> queue;  // (frames left, request), old way
  for (int frame = 0; frame < 120; ++frame) {
    // Skeleton frame: the answers due now.
    std::vector<ni::Request> due;
    if (pending_answer) {
      due = ni::TakeDue();
    } else {
      for (auto it = queue.begin(); it != queue.end();) {
        if (--it->first == 0) { due.push_back(it->second); it = queue.erase(it); } else ++it;
      }
    }
    for (const auto& r : due) {
      uint8_t m[ni::kMessageBytes];
      ni::WriteFrameProcessed(m, r.tracking_id);
      title.Callback(m);
      ni::WriteComplete(m, r.tracking_id, ni::AnswerFor(0));
      title.Callback(m);
    }
    // Script update.
    if (state == Identify) {
      uint32_t result = ni::Identify(0x00010001, 0, 0x8219B110, 0, 0x00010001);
      if (!pending_answer) {  // 156l: S_OK, answered on the next skeleton frame
        for (auto& r : ni::TakeDue()) (void)r;
        queue.push_back({delay, {0x00010001, 0x8219B110, 0, 0}});
        result = ni::kOk;
      }
      if (result == ni::kPending) state = EnterWait;  // else: the error path, asked again
    } else if (state == EnterWait) {
      title.result_set[0] = title.quality_set[0] = false;  // enterWaitIdentifySkeletonCallback
      state = Wait;
    } else if (state == Wait) {
      if (title.result_set[0] && title.result[0] == ni::kOk) state = Done;
    }
  }
  return state == Done;
}

static void CheckScript() {
  CHECK(!ScriptIdentifies(false, 1));  // 156l: S_OK, the script never waits
  CHECK(ScriptIdentifies(true, ni::kAnswerDelayFrames));
  CHECK(ni::kAnswerDelayFrames >= 3);  // after the wait's entry cleared the results
}

// Fix 156u (156u-nui-identity-proven-titles.patch): bound only in the titles it was proven in.
// Dance Central 3 crashed on 1.0.6_candidate_9 with the functions bound (never called) and
// played without the binding (162f).
static void CheckProvenTitles() {
  CHECK(ni::ProvenTitle(0x5553084E));   // Fighters Uncaged
  CHECK(ni::ProvenTitle(0x4E4D084E));   // Dragon Ball Z for Kinect
  CHECK(ni::ProvenTitle(0x4D5308C9));   // Kinect Sports
  CHECK(!ni::ProvenTitle(0x373307D9));  // Dance Central 3
  CHECK(!ni::ProvenTitle(0x4D4A07F0));  // Zumba Fitness
  CHECK(!ni::ProvenTitle(0x4D5309D6));  // Kinect Sports Season Two (no in-title identify)
  CHECK(!ni::ProvenTitle(0));
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
  CHECK(Has(cam, "auto requests = xbox360ps5::nui_identity::TakeDue();  // fix 156n"));
  const size_t body = cam.find("void DeliverIdentityAnswers(");
  const std::string fn = body == std::string::npos ? "" : cam.substr(body, 2200);
  const size_t frame = fn.find("WriteFrameProcessed(m, r.tracking_id, layout);");
  const size_t complete = fn.find("WriteComplete(m, r.tracking_id, answer, layout);");
  CHECK(frame != std::string::npos && complete != std::string::npos && frame < complete);
  CHECK(Has(fn, "memory->SystemHeapFree(message);"));
  CHECK(Has(cam, "  xbox360ps5::motion::ResetVirtualNui();\n  xbox360ps5::nui_identity::Reset();  // fix 156l"));
  CHECK(Has(cam, "Virtual Kinect: the title's Kinect ID answers for the signed-in profile"));
  // Fix 156u: the title check comes before anything is bound or the layout is set.
  const size_t proven = cam.find("if (!xbox360ps5::nui_identity::ProvenTitle(title_id)) {");
  const size_t layout_set = cam.find("xbox360ps5::nui_identity::bound_layout = int(found.layout);");
  const size_t bind = cam.find("!BindTitleFunction(kernel_state, xex, enrollment, IdentityEnrollmentHandler)");
  CHECK(proven != std::string::npos && layout_set != std::string::npos && bind != std::string::npos);
  CHECK(proven < layout_set && proven < bind);
  CHECK(Has(cam, "const uint32_t title_id = kernel_state->title_id();"));
  CHECK(Has(cam, "not bound in \"\n"
                 "           \"title {:08X}, which keeps its own identification (fix 156u)\""));
  // Fix 156q: the layout is the bound identify's, and the flags follow it.
  CHECK(Has(cam, "xbox360ps5::nui_identity::bound_layout = int(found.layout);  // fix 156q"));
  CHECK(Has(cam, "xbox360ps5::nui_identity::AllowedFlags(layout));  // fix 156q"));
}

int main() {
  CheckShapes();
  CheckIdentify();
  CheckMessages();
  CheckScript();
  CheckNewBuild();
  CheckProvenTitles();
  CheckTree();
  if (failures) {
    std::printf("check-nui-identity: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-nui-identity: ok\n");
  return 0;
}
