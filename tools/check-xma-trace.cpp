// SPDX-License-Identifier: MIT
// Fix 155 (tools/source-1.0.2/fixes/155-xma-trace.patch): a trace of the emulator's XMA contexts,
// to tell why Grand Theft Auto V's audio thread waits forever for decoded output.
//  (a) xma_trace.hpp: Decode reads every field of a made-up big-endian context; Describe's text;
//      BlocksAdded counts the write offset's advance around the output ring;
//  (b) Trace: the first 4 kicks of each context are lines (64 in all), a kick with input but no
//      output counts as "without output", a new error or parser error is a line; fix 67's lifts are
//      lines (16); locks, clears, allocations, releases and register 0606h reads are counted; the
//      30 s summary comes only with activity; when nothing was kicked for 5 s while a context is
//      allocated, one dump per silence (3 in all) with each allocated context's totals, its fields
//      now and its last events, and none while kicks go on;
//  (c) the assembled tree: xma_decoder.cc traces kicks before and after the decoder's pass, locks,
//      clears (with fix 67's lift and the context id), allocations, releases and 0606h reads, and
//      runs the poller from Setup to Shutdown.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/xma_trace.hpp"

namespace {
namespace fs = std::filesystem;
namespace xt = xbox360ps5::xma_trace;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
void Put(uint8_t* raw, int dword, uint32_t v) {
  raw[dword * 4] = uint8_t(v >> 24), raw[dword * 4 + 1] = uint8_t(v >> 16), raw[dword * 4 + 2] = uint8_t(v >> 8),
                raw[dword * 4 + 3] = uint8_t(v);
}
// A context: input 0 valid with 16 packets, output valid, 8 blocks, write `write`, read `read`.
struct Raw {
  uint8_t b[64] = {};
};
Raw Context(uint32_t write, uint32_t read, bool valid0 = true, bool error = false) {
  Raw r;
  Put(r.b, 0, 16u | (3u << 12) | (uint32_t(valid0) << 20) | (8u << 22) | (write << 27));
  Put(r.b, 1, 5u | (1u << 31));
  Put(r.b, 2, 1234u | (error ? (7u << 26) | (1u << 31) : 0));
  Put(r.b, 3, (2u << 26) | (1u << 31));
  Put(r.b, 4, 1u << 31);
  Put(r.b, 5, 0x0A1EA000), Put(r.b, 6, 0x0A1EB000), Put(r.b, 7, 0x0B10F680);
  Put(r.b, 9, read | (1u << 30));
  return r;
}

void Pure() {
  const Raw r = Context(6, 2);
  const xt::Fields f = xt::Decode(r.b);
  Expect(f.packets0 == 16 && f.loop_count == 3 && f.valid0 == 1 && f.valid1 == 0 && f.blocks == 8 && f.write == 6 &&
             f.packets1 == 5 && f.output_valid == 1 && f.read_offset == 1234 && f.error == 0 && f.error_set == 0 &&
             f.parser_error == 2 && f.parser_error_set == 1 && f.current_buffer == 1 && f.input0 == 0x0A1EA000 &&
             f.input1 == 0x0A1EB000 && f.output == 0x0B10F680 && f.read == 2 && f.stop_when_done == 1,
         "(a) Decode reads every field");
  Expect(xt::Describe(f) ==
             "in0 valid 16p @0A1EA000, in1 empty 5p @0A1EB000, buffer 1 read 1234 bits, out valid write 6 read 2 of 8 "
             "blocks @0B10F680, error 0, parser 2 set, loops 3, stop when done",
         "(a) Describe: " + xt::Describe(f));
  Expect(xt::BlocksAdded(xt::Decode(Context(2, 0).b), xt::Decode(Context(5, 0).b)) == 3 &&
             xt::BlocksAdded(xt::Decode(Context(6, 0).b), xt::Decode(Context(1, 0).b)) == 3 &&
             xt::BlocksAdded(xt::Decode(Context(4, 0).b), xt::Decode(Context(4, 0).b)) == 0,
         "(a) BlocksAdded around the ring of 8");
}

void Tracing() {
  xt::Trace t;
  const Raw a = Context(0, 0), b = Context(3, 0), c = Context(3, 0), e = Context(3, 0, true, true);
  std::vector<std::string> lines;
  for (int i = 0; i < 6; ++i) {
    const auto got = t.Kick(7, 0xFFCA91C0, a.b, b.b, 1000 + uint64_t(i));
    lines.insert(lines.end(), got.begin(), got.end());
  }
  Expect(lines.size() == 4 && lines[0].rfind("XMA trace (fix 155): kick ctx 7 (FFCA91C0) #1: +3 blocks; before ", 0) == 0,
         "(b) the first 4 kicks of a context are lines");
  const auto stuck = t.Kick(7, 0xFFCA91C0, b.b, c.b, 2000);
  Expect(stuck.empty(), "(b) the 7th kick is no line");
  const auto error = t.Kick(7, 0xFFCA91C0, c.b, e.b, 2001);
  Expect(error.size() == 1 && error[0].find("ctx 7 (FFCA91C0) error after kick #8: ") != std::string::npos,
         "(b) a new error is a line");
  int many = 0;
  for (uint32_t id = 10; id < 40; ++id)
    for (int k = 0; k < 4; ++k) many += int(t.Kick(id, 0, a.b, b.b, 3000).size());
  Expect(many == 60, "(b) 64 kick lines in all (4 already): " + std::to_string(many));
  for (int i = 0; i < 20; ++i) {
    const auto line = t.Unwatch(3, 0xFFCA90C0);
    Expect(bool(line) == (i < 16), "(b) 16 of fix 67's lifts are lines");
    if (i == 0) Expect(line && line->find("ctx 3 at FFCA90C0 before its clear (lift 1)") != std::string::npos, "(b) the lift's line");
  }

  xt::Trace s;
  auto raw = [&](uint32_t, uint8_t* out) { std::memcpy(out, Context(5, 1).b, 64); };
  Expect(s.Poll(100, raw).empty(), "(b) nothing to say at the start");
  s.Other(xt::Kind::kAllocate, 4, a.b, a.b, 200);
  s.Other(xt::Kind::kAllocate, 9, a.b, a.b, 200);
  s.Other(xt::Kind::kRelease, 9, a.b, a.b, 300);
  s.Read0606(0x41);
  s.Kick(4, 0xFFCA9100, a.b, b.b, 1000);
  s.Kick(4, 0xFFCA9100, b.b, c.b, 1100);
  s.Other(xt::Kind::kLock, 4, c.b, c.b, 1200);
  s.Other(xt::Kind::kClear, 4, c.b, a.b, 1300);
  Expect(s.Poll(5000, raw).empty(), "(b) no dump within 5 s of the last kick");
  const auto dump = s.Poll(6200, raw);
  Expect(dump.size() == 2 && dump[0] == "XMA trace (fix 155): no context kicked for 5.1 s (the last: ctx 4); 1 allocated:",
         "(b) the silence: " + (dump.empty() ? std::string() : dump[0]));
  Expect(dump.size() == 2 &&
             dump[1].find("ctx 4: 2 kicks (1 without output), 1 locks, 1 clears, 3 blocks out, 0 input buffers used, 0 "
                          "errors; now in0 valid 16p") != std::string::npos &&
             dump[1].find("out valid write 5 read 1 of 8 blocks") != std::string::npos &&
             dump[1].find(" [-5.2 s kick +3, in 10->10, write 0->3, read 0] [-5.1 s kick +0, in 10->10, write 3->3, "
                          "read 0] [-5.0 s lock +0") != std::string::npos &&
             dump[1].find("clear +5, in 10->10, write 3->0, read 0]") != std::string::npos,
         "(b) the allocated context's totals, fields now and last events: " + (dump.size() > 1 ? dump[1] : ""));
  Expect(s.Poll(9000, raw).empty(), "(b) one dump per silence");
  bool quiet = true;
  for (uint64_t ms = 10000; ms <= 28000; ms += 2000) {
    s.Kick(4, 0, a.b, b.b, ms);
    quiet = quiet && s.Poll(ms + 1000, raw).empty();
  }
  Expect(quiet, "(b) no dump while kicks go on");
  const auto summary = s.Poll(30200, raw);
  Expect(summary.size() == 1 &&
             summary[0] == "XMA trace (fix 155): last 30 s: 12 kicks on 1 contexts, 1 without output; 1 locks, 1 clears, "
                           "2 allocated, 1 released (1 allocated now); register 0606h read 1 times (last 65); 0 new "
                           "errors",
         "(b) the summary: " + (summary.empty() ? std::string() : summary[0]));
  Expect(s.Poll(34000, raw).size() == 2, "(b) the next silence is dumped");
  Expect(s.Poll(70000, raw).empty(), "(b) no summary without activity, no second dump of one silence");
  Expect(s.Poll(65000, raw).empty(), "(b) a clock going back says nothing");
}

void Tree() {
  const std::string x = Read(".deps/xenia-canary/src/xenia/apu/xma_decoder.cc");
  const size_t kick = x.find("const uint32_t kicked_value = value;");
  Expect(kick != std::string::npos && x.find("trace_before[bit]);", kick) < x.find("context.Enable();", kick) &&
             x.find("trace155.Kick(context_id", kick) > x.find("WaitForWorkDone();", kick),
         "(c) a kick's fields before the pass and after the worker is done");
  Expect(x.find("trace155.Other(xbox360ps5::xma_trace::Kind::kLock") != std::string::npos &&
             x.find("trace155.Other(xbox360ps5::xma_trace::Kind::kClear") != std::string::npos &&
             x.find("trace155.Other(xbox360ps5::xma_trace::Kind::kAllocate") != std::string::npos &&
             x.find("trace155.Other(xbox360ps5::xma_trace::Kind::kRelease") != std::string::npos &&
             x.find("trace155.Read0606(current_context_index);") != std::string::npos,
         "(c) locks, clears, allocations, releases and 0606h reads");
  Expect(x.find("uint32_t(sizeof(XMA_CONTEXT_DATA)), context_id);  // fix 67") != std::string::npos &&
             x.find("if (auto line = trace155.Unwatch(context_id, guest_address))") != std::string::npos,
         "(c) fix 67's lift says its context");
  Expect(x.find("trace155_thread = std::thread(Trace155Loop, memory(), context_data_first_ptr_);") !=
                 std::string::npos &&
             x.find("if (trace155_thread.joinable()) trace155_thread.join();") != std::string::npos,
         "(c) the poller from Setup to Shutdown");
}

}  // namespace

int main() {
  Pure();
  Tracing();
  if (fs::exists(".deps/xenia-canary/src/xenia/apu/xma_decoder.cc")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("xma trace: all checks passed\n");
  return 0;
}
