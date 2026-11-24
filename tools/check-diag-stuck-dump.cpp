// SPDX-License-Identifier: MIT
// Fix 156o (tools/source-1.0.2/fixes/156o-diag-stuck-objects.patch; Kinect Sports Season Two
// 4D5309D6, issue 63): the objects a stuck thread waits on. 156m named the darts (and tennis)
// load's wait: 82620594 keeps an avatar loader in r31 (vtable 82026DA0) whose IsDone needs its
// resource at +0x1E8 in state 2, flags at +0x214/+0x21C and every part of its model done.
// When a thread's 10 s snapshot has the same back chain (4 frames or more) as its previous
// one, once per chain, the Kinect diag dumps each readable r31 object along it and the heap
// objects those point to.
//
//   check-diag-stuck-dump        run from an assembled tree's root
//
//  (a) StuckChain: the same chain twice dumps once; a short chain never; a new chain again;
//  (b) DumpObjects over fake memory: the op (r31), its resource at +0x1E8 (one level), not a
//      stack or code pointer, no object twice, the limit; DumpLines' text;
//  (c) the tree: the dump in the snapshot, after the r31 line; Zumba's line of the skeleton's
//      hands against its shoulders (its hand cursor never appears; a stretched arm should read
//      forward 0.3 m or more).
#include <cstdint>
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
  std::string out;
  for (char c : s.str())
    if (c != '\r') out += c;
  return out;
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

static void CheckStuck() {
  kd::StuckChain stuck;
  const uint32_t a[5] = {0x82420760, 0x82620594, 0x825BFCD0, 0x825BFD64, 0x89A3FE98};
  const uint32_t b[5] = {0x82420760, 0x82620594, 0x825BFCD0, 0x825BFD64, 0x896F3D2C};
  CHECK(!stuck.Note(a, 5));  // first sight
  CHECK(stuck.Note(a, 5));   // the same 10 s later: dump
  CHECK(!stuck.Note(a, 5));  // already dumped
  CHECK(!stuck.Note(b, 5));
  CHECK(stuck.Note(b, 5));   // another place, stuck again
  kd::StuckChain shallow;
  CHECK(!shallow.Note(a, 3) && !shallow.Note(a, 3));  // a worker's short chain: never
}

static void CheckDump() {
  std::map<uint32_t, uint32_t> mem;
  const uint32_t op = 0x40E90010, resource = 0x41200000, other = 0x406FEA50;
  for (uint32_t i = 0; i < kd::kDumpWords; ++i) mem[op + 4 * i] = 0;
  mem[op] = 0x82026DA0;
  mem[op + 0x1E8] = resource;   // the avatar resource
  mem[op + 0x1EC] = 0x7018F000; // a stack address: not followed
  mem[op + 0x1F0] = 0x82447E60; // code: not followed
  mem[op + 0x1F4] = resource;   // twice: dumped once
  mem[op + 0x214] = 1;
  for (uint32_t i = 0; i < 0x118; ++i) mem[resource + 4 * i] = i;  // shorter than kDumpWords
  mem[resource + 8] = 1;        // its state
  mem[other] = 0x8207889C;
  auto read = [&](uint32_t a, uint32_t* v) {
    auto it = mem.find(a);
    if (it == mem.end()) return false;
    *v = it->second;
    return true;
  };
  kd::SavedR31 frames[3];
  frames[0].ret = 0x82620594; frames[0].r31 = op; frames[0].readable = 4;
  frames[1].ret = 0x825BFCD0; frames[1].r31 = 0x002DEB80; frames[1].readable = 4;  // not heap
  frames[2].ret = 0x82577E6C; frames[2].r31 = other; frames[2].readable = 1;
  const auto objects = kd::DumpObjects(frames, 3, read);
  CHECK(objects.size() == 3);
  CHECK(objects[0].address == op && objects[0].parent == 0 && objects[0].ret == 0x82620594 &&
        objects[0].words.size() == kd::kDumpWords && objects[0].words[0x214 / 4] == 1);
  CHECK(objects[1].address == other && objects[1].words.size() == 1);
  CHECK(objects[2].address == resource && objects[2].parent == op &&
        objects[2].words.size() == 0x118 && objects[2].words[2] == 1);
  const auto lines = kd::DumpLines(objects[0]);
  CHECK(lines.size() == kd::kDumpWords / 8);
  CHECK(lines[0] == "+000: 82026DA0 00000000 00000000 00000000 00000000 00000000 00000000 00000000");
  CHECK(lines[0x1E0 / 32] == "+1E0: 00000000 00000000 41200000 7018F000 82447E60 41200000 00000000 00000000");
  // The limit: an op full of pointers to distinct objects.
  std::map<uint32_t, uint32_t> many;
  for (uint32_t i = 0; i < kd::kDumpWords; ++i) {
    many[op + 4 * i] = 0x42000000 + 0x100 * i;
    many[0x42000000 + 0x100 * i] = i;
  }
  auto read_many = [&](uint32_t a, uint32_t* v) {
    auto it = many.find(a);
    if (it == many.end()) return false;
    *v = it->second;
    return true;
  };
  CHECK(kd::DumpObjects(frames, 1, read_many).size() == kd::kDumpObjects);
}

static void CheckTree() {
  const std::string shim = ReadText(".deps/xenia-canary/src/xenia/kernel/util/shim_utils.h");
  if (shim.empty()) {
    std::printf("FAIL: no assembled tree here (shim_utils.h)\n");
    ++failures;
    return;
  }
  const size_t r31 = shim.find("Kinect diag: thread {} r31 by frame {} (fix 156k)");
  const size_t note = shim.find("if (stuck.Note(callers, n)) {");
  const size_t dump = shim.find("kd::DumpObjects(frames, m, KinectDiagRead);");
  CHECK(r31 != std::string::npos && note != std::string::npos && r31 < note && note < dump);
  CHECK(Has(shim, "thread_local kd::StuckChain stuck;"));
  CHECK(Has(shim, "Kinect diag: thread {} object {:08X} (from {:08X}, frame {:08X}) {} (fix 156o)"));
  // Zumba: the skeleton's hands against its shoulders, before the frame is written.
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  const size_t hands = cam.find("Virtual Kinect: hands against shoulders: left forward {:.2f} up {:.2f}");
  const size_t write = cam.find("  xbox360ps5::motion::WriteSkeletonFrame(\n"
                                "      memory->TranslateVirtual(frame_ptr), pose, frame_number,");
  CHECK(hands != std::string::npos && write != std::string::npos && hands < write);
  CHECK(Has(cam, "SkeletonSpace(pose.joints[7], hx, hy, hz);") &&
        Has(cam, "SkeletonSpace(pose.joints[11], gx, gy, gz);"));
}

int main() {
  CheckStuck();
  CheckDump();
  CheckTree();
  if (failures) {
    std::printf("check-diag-stuck-dump: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-diag-stuck-dump: ok\n");
  return 0;
}
