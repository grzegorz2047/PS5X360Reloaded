// SPDX-License-Identifier: MIT
// Fix 81 (tools/source-1.0.2/fixes/81-enum-trace.patch): the life of the emulator's enumerators
// by kind. Battlefield 3 keeps about 15 enumerators a second and one XamAlloc buffer with each;
// fix 80 did not change that on 1.0.4_candidate_34. Each step counted for its kind (app id, open
// message): made, enumerated (items, no more, failed), taken, given back, closed, destroyed.
//  (a) include/xbox360ps5/enum_trace.hpp: the counts by kind, the first 4 of a kind reported for
//      the log, steps of an enumerator not made through it ignored, and the line;
//  (b) the assembled tree: XEnumerator::Initialize reports each enumerator (and logs the first of
//      a kind), its destructor, XamEnumerate's result, XamGetPrivateEnumStructureFromHandle,
//      ObDereferenceObject of an enumerator and NtClose of one each count their step; the
//      summary gives the line after fix 79's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/enum_trace.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
namespace et = xbox360ps5::enum_trace;

void CheckCounts() {
  Expect(et::Line() == "Enumerators by kind (app/open message, since the start): none", "nothing made: none");
  int logged = 0;
  // A kind made 6 times, each enumerated twice (items, then no more), taken and given back;
  // two closed and destroyed, four kept.
  for (uint32_t n = 0; n < 6; ++n) {
    const uint32_t object = 0x30000010 + (n << 12);
    logged += et::Made(object, 0xFB, 0x58039, 0x5803A, 0x5400, 8, 0) ? 1 : 0;
    et::Note(object, et::kTaken);
    et::Note(object, et::kGivenBack);
    et::Note(object, et::kEnumerated, 0, 3);
    et::Note(object, et::kEnumerated, et::kNoMoreFiles, 0);
    if (n < 2) {
      et::Note(object, et::kClosed);
      et::Note(object, et::kDestroyed);
    }
  }
  // Another kind, one enumerator, failed once.
  logged += et::Made(0x30100010, 0xFE, 0x58040, 0, 0, 1, 2) ? 1 : 0;
  et::Note(0x30100010, et::kEnumerated, 0x65B, 0);
  et::Note(0x3FFF0010, et::kClosed);  // never made through it: ignored
  Expect(logged == 5, "the first 4 of a kind reported for the log (" + std::to_string(logged) + ")");
  const std::string line = et::Line();
  std::printf("   %s\n", line.c_str());
  Expect(line.find("000000FB/00058039 (close 0005803A, extra 21504, 8 per call, flags 0) made 6, enumerated 12 (items 18, "
                   "no more 6, failed 0), taken 6, given back 6, closed 2, destroyed 2, alive 4") != std::string::npos,
         "a kind's steps, and how many are alive");
  Expect(line.find("; 000000FE/00058040 (close 00000000, extra 0, 1 per call, flags 2) made 1, enumerated 1 (items 0, "
                   "no more 0, failed 1), taken 0, given back 0, closed 0, destroyed 0, alive 1") != std::string::npos,
         "another kind, a failed enumeration");
  et::Note(0x30000010, et::kClosed);  // destroyed already: no longer counted
  Expect(et::Line() == line, "a destroyed enumerator's later steps are not counted");
}

void CheckTree() {
  const std::string K = ".deps/xenia-canary/src/xenia/kernel/";
  const std::string en = Read(K + "xenumerator.cc");
  const size_t made = en.find("if (xbox360ps5::enum_trace::Made(this->guest_object(), app_id, open_message,");
  const size_t log = en.find("\"Enumerator {:08X}: app {:08X}, open message {:08X}, close message \"", made);
  const size_t dtor = en.find("XEnumerator::~XEnumerator() {");
  const size_t destroyed = en.find("xbox360ps5::enum_trace::Note(guest_ptr, xbox360ps5::enum_trace::kDestroyed);  // fix 81", dtor);
  Expect(made != std::string::npos && log != std::string::npos && destroyed != std::string::npos,
         "Initialize reports each enumerator and logs the first of a kind; the destructor counts its end");
  const std::string xam = Read(K + "xam/xam_enum.cc");
  const size_t write = xam.find("e->WriteItems(buffer_ptr.as<uint8_t*>(), buffer_size, &item_count);");
  const size_t enumerated = xam.find("xbox360ps5::enum_trace::kEnumerated, result,", write);
  const size_t retain = xam.find("  e->RetainHandle();\n  xbox360ps5::enum_trace::Note(e->guest_object(),\n"
                                 "                               xbox360ps5::enum_trace::kTaken);  // fix 81");
  Expect(write != std::string::npos && enumerated != std::string::npos && enumerated - write < 200 && retain != std::string::npos,
         "XamEnumerate counts each result, XamGetPrivateEnumStructureFromHandle each taking");
  const std::string ob = Read(K + "xboxkrnl/xboxkrnl_ob.cc");
  const size_t deref = ob.find("void xeObDereferenceObject(PPCContext* context, uint32_t native_ptr) {");
  const size_t given = ob.find("xbox360ps5::enum_trace::kGivenBack);", deref);
  const size_t release = ob.find("object->ReleaseHandle();", deref);
  const size_t close = ob.find("uint32_t NtClose(uint32_t handle) {");
  const size_t closed = ob.find("xbox360ps5::enum_trace::kClosed);", close);
  const size_t table = ob.find("return kernel_state()->object_table()->ReleaseHandle(handle);", close);
  Expect(deref != std::string::npos && given < release && close != std::string::npos && closed < table && closed != std::string::npos,
         "ObDereferenceObject and NtClose count an enumerator's step before releasing it");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t heap = main_code.find("XELOGW(\"{}\", xbox360ps5::system_heap::Line());");
  const size_t kinds = main_code.find("XELOGW(\"{}\", xbox360ps5::enum_trace::Line());");
  Expect(heap != std::string::npos && kinds != std::string::npos && heap < kinds, "the summary gives the line after fix 79's");
}

}  // namespace

int main() {
  CheckCounts();
  CheckTree();
  if (failures) {
    std::printf("check-enum-trace: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-enum-trace: ok\n");
  return 0;
}
