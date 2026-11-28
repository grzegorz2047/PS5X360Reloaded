// SPDX-License-Identifier: MIT
// Fix 170 (tools/source-1.0.2/fixes/170-nui-speech-no-microphone.patch; Harry Potter Kinect,
// issue 74): the Kinect speech library (NUISP) waits for a microphone that no virtual Kinect
// has. See include/xbox360ps5/nui_speech.hpp for what the library does.
//
//   check-nui-speech        run from an assembled tree's root
//
//  1. the shapes: the start-up and Read call sites are found in the instruction words of Zumba
//     Fitness Core and Dance Central 3 (NUISP 2.0.21173.0, Harry Potter's and Nike+'s build) and
//     Kinect Sports Season Two (2.0.20764.3); the library's device check, Zumba's own Kinect
//     code and the status wrapper are not;
//  2. the caller: XamNuiGetDeviceStatus's LR returns into the wrapper, and the wrapper's saved
//     LR (one frame up) into the speech code; a broken chain is "other";
//  3. the answer: start-up gets 0x2 added, the Read gets 0x2 removed, everyone else fix 156's
//     bits;
//  4. a model of the library (start-up, the capture start that fails without a microphone, the
//     Read's 20 ms waits): Harry Potter's order (speech before the camera opens) waited forever
//     and now fails at start-up with Nike+'s error; Nike+'s order (camera first) is unchanged;
//     a Read reached anyway fails at its first timeout;
//  5. the tree: xam_nui.cc asks the classifier with the guest's LR and stack and answers the
//     adjusted bits only in its virtual Kinect branch, logged "(fix 170)".
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/nui_speech.hpp"

namespace ns = xbox360ps5::nui_speech;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// Guest memory as words; reads of anything else fail, like an unmapped page.
struct Guest {
  std::map<uint32_t, uint32_t> words;
  void Put(uint32_t address, std::initializer_list<uint32_t> list) {
    for (uint32_t w : list) {
      words[address] = w;
      address += 4;
    }
  }
  bool operator()(uint32_t address, uint32_t* value) const {
    auto it = words.find(address);
    if (it == words.end()) return false;
    *value = it->second;
    return true;
  }
};

// Zumba Fitness Core (NUISP 2.0.21173.0).
static void PutZumba(Guest& g) {
  // Read's wait loop: the timeout test 825BC4E8, the status call 825BC4FC, return 825BC500.
  g.Put(0x825BC4E0, {0x7D7D5B78, 0x4800005C, 0x2F030102, 0x409A004C, 0x817F00E4, 0x2F0B0000,
                     0x419A000C, 0x4BFDE35D, 0x48000008, 0x38600002, 0x546B07BD, 0x41820010,
                     0x817A3BF8, 0x2F0B0001, 0x409A0028, 0x546B07FF});
  // Start-up's "keep the error?" call 825BCDD4, return 825BCDD8.
  g.Put(0x825BCDB8, {0x7FE3FB78, 0x396B0008, 0x917F002C, 0x917F0030, 0x4BFFFE31, 0x7C7E1B79,
                     0x40800018, 0x4BFDDA85, 0x546B07BC, 0x216B0000, 0x7D6B5910, 0x7D7EF038,
                     0x7FC3F378, 0x38210080});
  // The device check that starts the capture (825BCC18, return 825BCC1C): fix 156's bits.
  g.Put(0x825BCBF8, {0x7D8802A6, 0x9181FFF8, 0xFBE1FFF0, 0x9421FFA0, 0x816300E4, 0x7C7F1B78,
                     0x2F0B0000, 0x419A0010, 0x4BFDDC41, 0x7C641B78, 0x48000008, 0x38800002,
                     0x7FE3FB78, 0x4BFFFE85});
  // Zumba's own Kinect code: 82278688 (bit 0x1, return 8227868C) and the Kinect manager's
  // start-up 8233254C (stores the bits, return 82332550).
  g.Put(0x82278668, {0x7D8802A6, 0x9181FFF8, 0xFBC1FFE8, 0xFBE1FFF0, 0x9421FF90, 0x39600000,
                     0x7C7F1B78, 0x99630054, 0x483221D1, 0x546B07FF, 0x40820010, 0x3D608007,
                     0x616B048F, 0x48000014});
  g.Put(0x8233252C, {0x48270795, 0x907D0074, 0x38800000, 0x481D2CF1, 0x2F030000, 0x4198FFCC,
                     0x7FA3EB78, 0x48000751, 0x4826830D, 0x546B07BC, 0x907D007C, 0x2B0B0000,
                     0x419A0044, 0x3BFD00A0});
  // The status wrapper 8259A858 (its call to the import returns 8259A86C).
  g.Put(0x8259A858, {0x7D8802A6, 0x9181FFF8, 0x9421FF90, 0x38610050, 0x484BFD4D, 0x8061005C,
                     0x38210070, 0x8181FFF8, 0x7D8803A6, 0x4E800020});
}
// Dance Central 3 (the same build, other addresses).
static void PutDanceCentral3(Guest& g) {
  g.Put(0x8286E130, {0x7D7D5B78, 0x4800005C, 0x2F030102, 0x409A004C, 0x817F00E4, 0x2F0B0000,
                     0x419A000C, 0x4BF8E74D, 0x48000008, 0x38600002, 0x546B07BD, 0x41820010,
                     0x817A7370, 0x2F0B0001, 0x409A0028, 0x546B07FF});
  g.Put(0x8286EA10, {0x7FE3FB78, 0x396B0008, 0x917F002C, 0x917F0030, 0x4BFFFE31, 0x7C7E1B79,
                     0x40800018, 0x4BF8DE6D, 0x546B07BC, 0x216B0000, 0x7D6B5910, 0x7D7EF038,
                     0x7FC3F378, 0x38210080});
}
// Kinect Sports Season Two (NUISP 2.0.20764.3: no "use the Kinect" flag in the Read).
static void PutSeasonTwo(Guest& g) {
  g.Put(0x82AACDB8, {0x807F0088, 0x388100AC, 0x48016F29, 0x48000050, 0x7D7D5B78, 0x48000048,
                     0x2F030102, 0x409A0038, 0x48165D61, 0x546B07BD, 0x41820010, 0x817AA6E0,
                     0x2F0B0001, 0x409A0028, 0x546B07FF, 0x3FA08007});
  g.Put(0x82AAD5B4, {0x917F002C, 0x917F0030, 0x4816557D, 0x7C641B78, 0x7FE3FB78, 0x4BFFFD81,
                     0x7C7E1B79, 0x40800018, 0x48165565, 0x546B07BC, 0x216B0000, 0x7D6B5910,
                     0x7D7EF038, 0x7FC3F378, 0x38210070});
}

static void CheckShapes() {
  Guest g;
  PutZumba(g);
  PutDanceCentral3(g);
  PutSeasonTwo(g);
  CHECK(ns::ClassifyReturn(0x825BC500, g) == ns::Site::kRead);
  CHECK(ns::ClassifyReturn(0x825BCDD8, g) == ns::Site::kStartup);
  CHECK(ns::ClassifyReturn(0x8286E150, g) == ns::Site::kRead);
  CHECK(ns::ClassifyReturn(0x8286EA30, g) == ns::Site::kStartup);
  CHECK(ns::ClassifyReturn(0x82AACDDC, g) == ns::Site::kRead);
  CHECK(ns::ClassifyReturn(0x82AAD5D8, g) == ns::Site::kStartup);
  // Not the speech library's microphone questions.
  CHECK(ns::ClassifyReturn(0x825BCC1C, g) == ns::Site::kOther);  // device check: starts capture
  CHECK(ns::ClassifyReturn(0x82AAD5C0, g) == ns::Site::kOther);  // Season Two's device check
  CHECK(ns::ClassifyReturn(0x8227868C, g) == ns::Site::kOther);  // Zumba: bit 0x1
  CHECK(ns::ClassifyReturn(0x82332550, g) == ns::Site::kOther);  // Zumba: Kinect manager
  CHECK(ns::ClassifyReturn(0x8259A86C, g) == ns::Site::kOther);  // inside the wrapper
  CHECK(ns::ClassifyReturn(0, g) == ns::Site::kOther);
  CHECK(ns::ClassifyReturn(0x825BC502, g) == ns::Site::kOther);  // unaligned
  CHECK(ns::ClassifyReturn(0x90000000, g) == ns::Site::kOther);  // unreadable
  // Every word of the window is needed: one page missing leaves it alone.
  Guest torn = g;
  torn.words.erase(0x825BC4E0);
  CHECK(ns::ClassifyReturn(0x825BC500, torn) == ns::Site::kOther);
  // A Read site whose call is not after a timeout test is not the Read.
  Guest no_timeout = g;
  no_timeout.words[0x825BC4E8] = 0x2F030000;  // cmpwi cr6,r3,0
  CHECK(ns::ClassifyReturn(0x825BC500, no_timeout) == ns::Site::kOther);
  // Start-up without the "and" that keeps the error is not start-up.
  Guest no_and = g;
  no_and.words[0x825BCDE4] = 0x7FC3F378;
  CHECK(ns::ClassifyReturn(0x825BCDD8, no_and) == ns::Site::kOther);
  CHECK(std::string(ns::SiteName(ns::Site::kStartup)) == "start-up");
  CHECK(std::string(ns::SiteName(ns::Site::kRead)) == "audio read");
}

static void CheckCaller() {
  Guest g;
  PutZumba(g);
  // The wrapper's frame: sp 7042F2B0 -> its caller's sp 7042F360, saved LR at 7042F358.
  const uint32_t sp = 0x7042F2B0, back = 0x7042F360;
  g.Put(sp, {back});
  g.Put(back - 8, {0x825BC500});
  uint32_t where = 1;
  CHECK(ns::ClassifyCaller(0x8259A86C, sp, g, &where) == ns::Site::kRead);
  CHECK(where == 0x825BC500);
  g.words[back - 8] = 0x825BCDD8;
  CHECK(ns::ClassifyCaller(0x8259A86C, sp, g, &where) == ns::Site::kStartup);
  CHECK(where == 0x825BCDD8);
  g.words[back - 8] = 0x82332550;  // Zumba's Kinect manager
  CHECK(ns::ClassifyCaller(0x8259A86C, sp, g, &where) == ns::Site::kOther);
  CHECK(where == 0);
  // A direct call from the speech code (the import called without a wrapper).
  CHECK(ns::ClassifyCaller(0x825BC500, 0, g) == ns::Site::kRead);
  // Broken chains: no word at sp, a chain going down, one too far up.
  Guest broken = g;
  broken.words.erase(sp);
  CHECK(ns::ClassifyCaller(0x8259A86C, sp, broken) == ns::Site::kOther);
  broken = g;
  broken.words[sp] = sp - 0x10;
  CHECK(ns::ClassifyCaller(0x8259A86C, sp, broken) == ns::Site::kOther);
  broken.words[sp] = sp + 0x200000;
  CHECK(ns::ClassifyCaller(0x8259A86C, sp, broken) == ns::Site::kOther);
  CHECK(ns::ClassifyCaller(0x8259A86C, 0, g) == ns::Site::kOther);
}

static void CheckAnswer() {
  for (uint32_t bits : {0u, 1u, 2u, 3u, 4u, 5u}) {
    CHECK(ns::SpeechStatusBits(ns::Site::kOther, bits) == bits);
    CHECK(ns::SpeechStatusBits(ns::Site::kStartup, bits) == (bits | 2));
    CHECK(ns::SpeechStatusBits(ns::Site::kRead, bits) == (bits & ~2u));
  }
  // Fix 156's connected answer stays 0x3 for everyone else; the Read hears "connected, not
  // ready" (0x1), so it fails with 0x80070015 rather than "not connected".
  CHECK(ns::SpeechStatusBits(ns::Site::kRead, 3) == 1);
  CHECK(ns::SpeechStatusBits(ns::Site::kStartup, 0) == 2);
}

// ---- A model of the speech library's audio source (Zumba 825BC448..825BCE60) --------------
constexpr int32_t kNotConnected = int32_t(0x8007048F), kNotReady = int32_t(0x80070015);
constexpr int32_t kCaptureFailed = kNotConnected;  // fix 156f: no microphone array
struct Source {
  int32_t error = 0;          // +0xD8
  bool capturing = false;     // +0xE0
  bool capture_error = false; // the global the capture's error callback sets
};
// The device check (825BCAB0): with 0x2 and no capture error it starts the capture.
static void OnDeviceStatus(Source& s, uint32_t bits) {
  if ((bits & 2) && !s.capture_error) {
    s.error = kCaptureFailed;  // XamVoiceGetMicArrayAudioEx fails, the capture never starts
    return;
  }
  s.error = (bits & 1) ? kNotReady : kNotConnected;
  s.capture_error = false;
}
// Start-up (825BCD38): `ask` answers the status calls, by site.
template <typename Ask>
static int32_t StartUp(Source& s, Ask ask) {
  OnDeviceStatus(s, ask(ns::Site::kOther));
  int32_t result = s.error;
  if (result < 0 && !(ask(ns::Site::kStartup) & 2)) result = 0;  // wait for the notification
  return result;
}
// The Read (825BC448): 20 ms waits; nothing sets the event without a capture. Returns the
// Read's result, or 1 when it was still waiting after `timeouts` waits.
template <typename Ask>
static int32_t Read(Source& s, Ask ask, int timeouts) {
  for (int i = 0; i < timeouts; ++i) {
    if (s.capturing) return 0;  // frames set the event
    const uint32_t bits = ask(ns::Site::kRead);
    if ((bits & 2) && !s.capture_error) continue;  // ready: wait again
    return (bits & 1) ? kNotReady : kNotConnected;
  }
  return 1;
}

static void CheckModel() {
  // The answer the emulator gives at each site, before and after fix 170.
  const auto before = [](uint32_t bits) { return [bits](ns::Site) { return bits; }; };
  const auto after = [](uint32_t bits) {
    return [bits](ns::Site site) { return ns::SpeechStatusBits(site, bits); };
  };
  // Harry Potter: speech starts with the camera closed (fix 156: bits 0), reads once it opened.
  {
    Source s;
    CHECK(StartUp(s, before(0)) == 0);         // waits for a notification that never comes
    CHECK(Read(s, before(3), 1000) == 1);       // ... and the Read waits forever
  }
  {
    Source s;
    CHECK(StartUp(s, after(0)) == kNotConnected);  // fails now, as Nike+ does
    Source t;
    CHECK(StartUp(t, before(0)) == 0);
    CHECK(Read(t, after(3), 1000) == kNotReady);   // a Read reached anyway fails at once
    CHECK(Read(t, after(0), 1000) == kNotConnected);
  }
  // Nike+, Zumba, Season Two: speech starts after the camera opened: unchanged.
  for (bool fixed : {false, true}) {
    Source s;
    const int32_t r = fixed ? StartUp(s, after(3)) : StartUp(s, before(3));
    CHECK(r == kCaptureFailed);
  }
  // A capture that runs (a microphone one day) is not touched: frames come first.
  Source running;
  running.capturing = true;
  CHECK(Read(running, after(3), 1) == 0);
}

// ---- The tree -----------------------------------------------------------------------------
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

static void CheckTree() {
  const std::string nui = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_nui.cc");
  if (nui.empty()) {
    std::printf("FAIL: no assembled tree here (xam_nui.cc)\n");
    ++failures;
    return;
  }
  CHECK(Has(nui, "#include \"xbox360ps5/nui_speech.hpp\""));
  const std::string status = Body(nui, "dword_result_t XamNuiGetDeviceStatus_entry(");
  CHECK(Has(status, "const ppc_context_t& ctx"));
  CHECK(Has(status, "xbox360ps5::nui_speech::ClassifyCaller(uint32_t(ctx->lr), uint32_t(ctx->r[1]),"));
  CHECK(Has(status, "xbox360ps5::nui_speech::SpeechStatusBits(site, status.status_bits)"));
  CHECK(Has(status, "status_ptr->status_bits = bits;"));
  CHECK(!Has(status, "status_ptr->status_bits = status.status_bits;"));
  CHECK(Has(status, "(fix 170)"));
  // Only in the virtual Kinect branch, before fix 156's own answer is returned.
  const size_t virtual_at = status.find("if (VirtualNuiMode())");
  const size_t classify_at = status.find("nui_speech::ClassifyCaller");
  const size_t returned_at = status.find("return status.result;");
  CHECK(virtual_at != std::string::npos && classify_at > virtual_at && classify_at < returned_at);
  // The export's declaration is unchanged.
  CHECK(Has(nui, "DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus,"));
}

int main() {
  CheckShapes();
  CheckCaller();
  CheckAnswer();
  CheckModel();
  CheckTree();
  if (failures) {
    std::printf("check-nui-speech: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-nui-speech: ok\n");
  return 0;
}
