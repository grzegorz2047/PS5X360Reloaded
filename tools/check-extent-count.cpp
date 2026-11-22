// SPDX-License-Identifier: MIT
// Fix 110 (tools/source-1.0.2/fixes/110-extent-count.patch): the screen extents (EVENT_WRITE_EXT)
// are only counted in fix 24's command processor trace. Red Dead Redemption on
// 1.0.5_candidate_64 (fix 96's line): SCREEN_EXT_RPT 36,000 to 51,000 a second, about one per
// draw, 0.57 us each against 0.02 for an EVENT_WRITE. Each was recorded like a fence write: the
// clock, the trace's mutex and two scans of its 32 targets (every extent has its own address),
// and pushed the writes a STALL report needs out of the trace. Checked:
//  (a) xenia/gpu/cp_trace.h: Count adds to the kind's total, which Total and the STALL report see,
//      and touches neither the recent writes nor the targets; Record is unchanged (the fence
//      writes after many extents are all still there);
//  (b) the assembled tree: EVENT_WRITE_EXT counts its write and records nothing, and still writes
//      the extents to guest memory; the other kinds still record.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/gpu/cp_trace.h"

#include <cstdint>
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
namespace ct = xbox360ps5::cp_trace;

void CheckTrace() {
  ct::Trace trace;
  for (uint32_t fence = 0; fence < 10; ++fence) trace.Record(1000 + fence, ct::Kind::kEventShd, 0x1FC4A010, fence);
  for (uint32_t n = 0; n < 50000; ++n) trace.Count(ct::Kind::kEventExt);  // a second of extents
  trace.Record(2000, ct::Kind::kEventExt, 0x00500000, 0);                    // one still recorded
  Expect(trace.Total(ct::Kind::kEventExt) == 50001, "Total counts the counted extents and the recorded one");
  Expect(trace.Total(ct::Kind::kEventShd) == 10 && trace.Total(ct::Kind::kMemWrite) == 0, "the other kinds' totals");
  const auto writes = trace.Writes();
  Expect(writes.size() == 11, "counted writes are not kept among the recent writes");
  Expect(trace.Targets().size() == 2 && !trace.TargetsWithValue(9).empty(),
         "nor among the targets: the fence's address is still found by its value");
}

void CheckTree() {
  const std::string pm4 = Read(".deps/xenia-canary/src/xenia/gpu/pm4_command_processor_implement.h");
  const size_t ext = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE_EXT(");
  const size_t end = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_EVENT_WRITE_ZPD(", ext);
  const std::string body = ext != std::string::npos && end != std::string::npos ? pm4.substr(ext, end - ext) : "";
  Expect(body.find("xbox360ps5::cp_trace::Global().Count(xbox360ps5::cp_trace::Kind::kEventExt);") != std::string::npos &&
             body.find("cp_trace::Global().Record(") == std::string::npos,
         "EVENT_WRITE_EXT counts its write and records nothing");
  Expect(body.find("destination[i] = extents[i];") != std::string::npos, "the extents still reach guest memory");
  for (const char* kind : {"Kind::kEventShd", "Kind::kMemWrite", "Kind::kRegToMem", "Kind::kCondWrite"}) {
    Expect(pm4.find(std::string("xbox360ps5::cp_trace::Global().Record(xbox360ps5::cp_trace::NowMs(), xbox360ps5::cp_trace::") +
                    kind) != std::string::npos,
           std::string("still recorded: ") + kind);
  }
}

}  // namespace

int main() {
  CheckTrace();
  CheckTree();
  if (failures) {
    std::printf("check-extent-count: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-extent-count: ok\n");
  return 0;
}
