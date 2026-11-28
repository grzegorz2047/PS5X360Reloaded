// SPDX-License-Identifier: MIT
// Fix 156s (tools/source-1.0.2/fixes/156s-diag-player-choice.patch; Zumba Fitness 4D4A07F0,
// issue 63): a diagnostic of which body the title picks as its player. Zumba stays at
// "Searching for player..." with our body tracked and identified; its player choice and its
// play area are vector code. Each second (120 lines at most) the log gives the title's choice
// (its skeleton consumer, the frame function's caller's r31), the NUI library's record of it
// (NuiSkeletonSetTrackedSkeletons' globals) and our body's ID; once, Zumba's play area.
//
//   check-player-choice        run from an assembled tree's root
//
//  (a) FindSetTrackedSkeletons on synthetic code shaped like Zumba's 8250F6A8: the function,
//      its globals and offsets; one mark missing, not found; TrackingId;
//  (b) FindZumbaPlayArea: only with Zumba's three words in place;
//  (c) RateLimit: once a second, 120 lines;
//  (d) the tree: the line after each skeleton frame, read-only; the binding; the reset.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/nui_choice.hpp"

namespace nc = xbox360ps5::nui_choice;

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

struct Image {
  std::vector<uint8_t> bytes;
  uint32_t base = 0x82000000;
  void At(uint32_t address, uint32_t w) {
    const uint32_t o = address - base;
    if (bytes.size() < o + 4) bytes.resize(o + 4, 0);
    bytes[o] = uint8_t(w >> 24);
    bytes[o + 1] = uint8_t(w >> 16);
    bytes[o + 2] = uint8_t(w >> 8);
    bytes[o + 3] = uint8_t(w);
  }
  uint32_t Put(uint32_t address, std::initializer_list<uint32_t> words) {
    uint32_t a = address;
    for (uint32_t w : words) At(a, w), a += 4;
    return address;
  }
  xbox360ps5::motion::CodeView View() const { return {bytes.data(), bytes.size(), base}; }
};

// Zumba's NuiSkeletonSetTrackedSkeletons (8250F6A8), reduced to the marks.
static const std::initializer_list<uint32_t> kSetTracked = {
    0x7D8802A6,  // mflr r12
    0x7C7E1B78,  // mr r30,r3
    0x2B030000,  // cmplwi cr6,r3,0
    0x60630057,  // ori r3,r3,0x57
    0x3D6082AF,  // lis r11,-0x7d51 (0x82AF)
    0x3BEB3E00,  // addi r31,r11,0x3e00
    0x817F0874,  // lwz r11,0x874(r31)
    0x60630002,  // ori r3,r3,2
    0x817F1F10,  // lwz r11,0x1f10(r31)
    0x60630005,  // ori r3,r3,5
    0x817F0084,  // lwz r11,0x84(r31)
    0x6149E8AC,  // ori r9,r10,0xe8ac
    0x6128E8B4,  // ori r8,r9,0xe8b4
    0x6149E8B8,  // ori r9,r10,0xe8b8
    0x63DE000B,  // ori r30,r30,0xb
    0x4E800020,  // blr
};

static void CheckFind() {
  Image image;
  image.Put(0x82000000, {0x7D8802A6, 0x4E800020});
  const uint32_t fn = image.Put(0x82000100, kSetTracked);
  auto found = nc::FindSetTrackedSkeletons(image.View());
  CHECK(found.size() == 1);
  if (found.size() == 1) {
    const auto& s = found[0];
    CHECK(s.function == fn && s.globals == 0x82AF3E00);
    CHECK(s.ready == 0x874 && s.enabled == 0x1F10 && s.system == 0x84);
    CHECK(s.base == 0xE8AC && s.id0 == 0xE8B4 && s.id1 == 0xE8B8);
  }
  // Without the system-path code (8301000B), as the older library builds: not found.
  Image older;
  std::vector<uint32_t> words(kSetTracked);
  words[14] = 0x60000000;  // nop
  uint32_t a = 0x82000100;
  for (uint32_t w : words) older.At(a, w), a += 4;
  CHECK(nc::FindSetTrackedSkeletons(older.View()).empty());
  CHECK(nc::TrackingId(0, 0x1000) == 0);
  CHECK(nc::TrackingId(5, 0x1000) == 0x1005);
}

static void CheckPlayArea() {
  Image image;
  image.At(0x8227B6F4, 0x3D4082BB);
  image.At(0x8227B6FC, 0x906ACD98);
  image.At(0x8227D078, 0x387F00AC);
  auto area = nc::FindZumbaPlayArea(image.View());
  CHECK(area.ok && area.pointer == 0x82BACD98 && area.offset == 0xAC);
  image.At(0x8227D078, 0x387F00B0);
  CHECK(!nc::FindZumbaPlayArea(image.View()).ok);
  Image small;
  small.At(0x82000000, 0);
  CHECK(!nc::FindZumbaPlayArea(small.View()).ok);
}

static void CheckRate() {
  nc::RateLimit limit;
  CHECK(limit.Due(0));
  CHECK(!limit.Due(999));
  CHECK(limit.Due(1000));
  int lines = 2;
  for (int64_t t = 2000; t < 1000 * 400; t += 1000) lines += limit.Due(t);
  CHECK(lines == int(nc::kChoiceLines));
}

static void CheckTree() {
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  if (cam.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_pscam.cc)\n");
    ++failures;
    return;
  }
  CHECK(Has(cam, "#include \"xbox360ps5/nui_choice.hpp\"  // fix 156s"));
  CHECK(Has(cam, "      kernel_state, uint32_t(ppc_context->r[3]), frame));\n"
                 "  LogPlayerChoice(ppc_context, kernel_state, frame);  // fix 156s"));
  const size_t fn = cam.find("void LogPlayerChoice(");
  const size_t end = cam.find("void SkeletonGetNextFrameHandler(", fn);
  CHECK(fn != std::string::npos && end != std::string::npos);
  const std::string body = fn == std::string::npos ? "" : cam.substr(fn, end - fn);
  CHECK(Has(body, "if (!g_choice_limit.Due(now_ms)) return;"));
  CHECK(!Has(body, "store_and_swap"));  // read only
  CHECK(Has(body, "held == frame"));
  CHECK(Has(body, "Virtual Kinect: player choice: the title {} (mode {}, IDs {:08X} {:08X})"));
  CHECK(Has(body, "Virtual Kinect: the title's play area at {:08X}: forward boundary {:.2f} m"));
  CHECK(Has(cam, "const auto found = xbox360ps5::nui_choice::FindSetTrackedSkeletons(view);"));
  CHECK(Has(cam, "g_play_area = xbox360ps5::nui_choice::FindZumbaPlayArea(view);"));
  CHECK(Has(cam, "    g_choice_limit = {};"));
}

int main() {
  CheckFind();
  CheckPlayArea();
  CheckRate();
  CheckTree();
  if (failures) {
    std::printf("check-player-choice: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-player-choice: ok\n");
  return 0;
}
