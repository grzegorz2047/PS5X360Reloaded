// SPDX-License-Identifier: MIT
// Fix 142f-diag (research/parallel-gpu-commands/STAGE2.md, section 14): one diagnostic image of the
// GPU command thread and what limits the frame rate, its detailed probes behind one switch
// (gpu-diag-detail.txt). Nothing it adds changes what the emulator does.
//  (switch) off by default; the file turns it on and off; fix 142d's probes and the new ones are
//      behind it;
//  (a) the resolve and memexport readback waits by reason: a ResolveReasonScope (or
//      MemexportReasonScope) names the wait it encloses, a fence wait of the kResolve (kMemexport)
//      cause counts under that reason (and its bytes), any other cause or thread does not, and the
//      reason comes back when the scope ends; the fast copies that did not wait, by outcome;
//  (b) the packet timer: an INDIRECT_BUFFER counts the timed packets nested in it, type 0 packets
//      count their own time by their first register's group and at the hot registers; every packet
//      counted by kind;
//  (c) the timer's estimated cost is finite and not negative; a first line's tick length comes from
//      the program's start, not 1 ns;
//  (d) Process by index buffer type, the texture set history, the float constant write classes,
//      the system constants, the faults by kind and guest address;
//  (e) the six lines' arithmetic and text (fixed tick length and timer cost);
//  (f) the assembled tree: every sub-part of fix 73's primitives and pipeline parts starts before it
//      stops and inside its part, every readback wait names its reason at its own place, each probe
//      at its place, the lines logged after fix 142d's, the switch read at the end of a submission;
//  (g) the patch: each line it changes carries "fix 142f-diag" (argv[1]: the fixes folder).
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "xbox360ps5/frame_cost.hpp"

namespace {

namespace fc = xbox360ps5::frame_cost;
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
uint64_t Load(const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); }
void Spin(int rounds) {
  std::atomic<int> sink{0};
  for (int i = 0; i < rounds; ++i) sink.fetch_add(1, std::memory_order_relaxed);
}
bool Has(const std::string& text, const std::string& part) {
  const bool found = text.find(part) != std::string::npos;
  if (!found) std::printf("  (not found: %s)\n", part.c_str());
  return found;
}

// (a)
void CheckResolveReasons() {
  fc::MarkGpuThread();
  const uint64_t full_before = Load(fc::resolve_wait_ns[fc::kResolveFullMode]);
  const uint64_t own_before = Load(fc::resolve_wait_ns[fc::kResolveOwnCopy]);
  const uint64_t unnamed_before = Load(fc::resolve_wait_ns[fc::kResolveUnnamed]);
  {
    fc::CauseScope cause(fc::kResolve);
    {
      fc::ResolveReasonScope reason(fc::kResolveFullMode, 4096);
      fc::AddFenceWait(1000);
      {
        fc::ResolveReasonScope inner(fc::kResolveOwnCopy, 8192);
        fc::AddFenceWait(70);
      }
      Expect(fc::resolve_reason == fc::kResolveFullMode, "the outer reason comes back");
      fc::AddFenceWait(500);
    }
    Expect(fc::resolve_reason == fc::kResolveUnnamed, "no reason outside a scope");
    fc::AddFenceWait(30);  // a kResolve wait in no named place
  }
  const uint64_t unnamed_export_before = Load(fc::memexport_wait_ns[0]);
  {
    fc::CauseScope cause(fc::kMemexport);
    fc::ResolveReasonScope reason(fc::kResolveFirstCopy, 1);
    fc::AddFenceWait(99);  // another cause: not a resolve wait (an unnamed memexport one)
    {
      fc::MemexportReasonScope small(2, 256);  // memexport_loss::Reason::kSmall
      fc::AddFenceWait(11);
    }
    fc::MemexportReasonScope lost(3, 1024);  // kLearned
    fc::AddFenceWait(22);
  }
  Expect(Load(fc::memexport_wait_ns[0]) - unnamed_export_before == 99, "an unnamed memexport wait");
  Expect(Load(fc::memexport_wait_ns[2]) == 11 && Load(fc::memexport_wait_bytes[2]) == 256, "small export waits");
  Expect(Load(fc::memexport_wait_ns[3]) == 22 && Load(fc::memexport_waits[3]) == 1, "lost-before waits");
  Expect(fc::memexport_reason == 0, "no memexport reason outside a scope");
  Expect(Load(fc::resolve_wait_ns[fc::kResolveFullMode]) - full_before == 1500, "full mode waits");
  Expect(Load(fc::resolve_wait_ns[fc::kResolveOwnCopy]) - own_before == 70, "own copy waits");
  Expect(Load(fc::resolve_wait_ns[fc::kResolveUnnamed]) - unnamed_before == 30, "unnamed resolve waits");
  Expect(Load(fc::resolve_wait_ns[fc::kResolveFirstCopy]) == 0, "another cause is not a resolve wait");
  Expect(Load(fc::resolve_waits[fc::kResolveFullMode]) == 2 && Load(fc::resolve_wait_bytes[fc::kResolveFullMode]) == 8192,
         "full mode: two waits of 4 KiB");
  Expect(Load(fc::resolve_wait_bytes[fc::kResolveOwnCopy]) == 8192, "own copy's bytes");
  // Another thread's scope names nothing and its waits count apart (fix 49).
  bool other_ok = false;
  std::thread([&] {
    fc::CauseScope cause(fc::kResolve);
    fc::ResolveReasonScope reason(fc::kResolveGrowth, 1);
    fc::AddFenceWait(5);
    other_ok = Load(fc::resolve_wait_ns[fc::kResolveGrowth]) == 0;
  }).join();
  Expect(other_ok, "another thread's waits are not the GPU thread's resolve waits");
}

// The switch.
void CheckSwitch() {
  Expect(!fc::DiagDetail(), "off by default");
  Expect(fc::ParseDiagDetail("1\n") && fc::ParseDiagDetail("  on # yes") && fc::ParseDiagDetail("ON"), "1 or on");
  Expect(!fc::ParseDiagDetail("0") && !fc::ParseDiagDetail("off") && !fc::ParseDiagDetail("") &&
             !fc::ParseDiagDetail("10") && !fc::ParseDiagDetail("# 1"),
         "0, off, empty, anything else");
  fc::MarkGpuThread();
  // Fix 142d's probes are behind it.
  const uint64_t draws = Load(fc::bind_draws);
  fc::BindBeginDraw();
  Expect(Load(fc::bind_draws) == draws && !fc::bind_probe_active, "fix 142d's draw probe off with the switch");
  const uint64_t all = Load(fc::packet_all_counts[fc::kPacketEvents]);
  { fc::PacketTimer event(0xC0004600u); }
  Expect(Load(fc::packet_all_counts[fc::kPacketEvents]) == all, "packets not counted one by one with the switch off");
  const char* path = "build/fix-checks/gpu-diag-detail.txt";
  std::remove(path);
  if (FILE* f = std::fopen(path, "wb")) { std::fputs("1\n", f); std::fclose(f); }
  Expect(fc::RefreshDiagDetail(path) && fc::DiagDetail(), "the file turns it on (a change)");
  Expect(!fc::RefreshDiagDetail(path), "read again at most once a second");
  fc::BindBeginDraw();
  Expect(Load(fc::bind_draws) == draws + 1, "fix 142d's draw probe on with the switch");
  { fc::PacketTimer event(0xC0004600u); }
  Expect(Load(fc::packet_all_counts[fc::kPacketEvents]) == all + 1, "every packet counted with the switch on");
  std::remove(path);
}

// (b)
void CheckPackets() {
  fc::MarkGpuThread();
  fc::diag_detail.store(true);
  fc::packet_sample_every.store(1);
  const uint64_t nested_before = Load(fc::packet_indirect_nested);
  const uint64_t ib_before = Load(fc::packet_counts[fc::kPacketIndirect]);
  uint64_t counts_before[fc::kRegGroups];
  for (uint32_t g = 0; g < fc::kRegGroups; ++g) counts_before[g] = Load(fc::packet_register_counts[g]);
  {
    fc::PacketTimer ib(0xC0003F00u);  // type 3, INDIRECT_BUFFER
    { fc::PacketTimer a(0x00034000u); }  // type 0 at 0x4000: float constants
    { fc::PacketTimer b(0x00004800u); }  // fetch constants
    { fc::PacketTimer c(0x00004900u); }  // bool/loop constants
    { fc::PacketTimer d(0x00002000u); }  // RB_SURFACE_INFO: another register
    {
      fc::PacketTimer inner_ib(0xC0003F00u);
      { fc::PacketTimer e(0x00004000u); }
    }
  }
  // The outer IB: 6 timed packets nested (4, the inner IB, the one in it); the inner: 1.
  Expect(Load(fc::packet_counts[fc::kPacketIndirect]) - ib_before == 2, "two INDIRECT_BUFFERs timed");
  Expect(Load(fc::packet_indirect_nested) - nested_before == 7, "6 + 1 timed packets nested in them");
  const uint32_t want[fc::kRegGroups] = {2, 1, 1, 1};
  for (uint32_t g = 0; g < fc::kRegGroups; ++g) {
    Expect(Load(fc::packet_register_counts[g]) - counts_before[g] == want[g],
           std::string("type 0 counted under ") + fc::RegisterGroupName(g));
  }
  // Off the GPU thread nothing is timed.
  const uint64_t ib_now = Load(fc::packet_counts[fc::kPacketIndirect]);
  std::thread([] { fc::PacketTimer ib(0xC0003F00u); }).join();
  Expect(Load(fc::packet_counts[fc::kPacketIndirect]) == ib_now, "other threads' packets are not timed");
  fc::packet_sample_every.store(16);
}

// (d)
void CheckDraws() {
  fc::MarkGpuThread();
  fc::diag_detail.store(true);
  // Process: a timed draw.
  fc::bind_probe_active = true;
  const fc::BindMark prepare = fc::BindStart();
  Spin(20000);
  const fc::BindMark request = fc::BindStart();
  Spin(2000);
  fc::NoteProcess(2, prepare, request);  // host converted
  fc::NoteProcess(1, fc::BindMark(), fc::BindMark());  // guest DMA, not timed
  fc::bind_probe_active = false;
  Expect(Load(fc::process_counts[2]) == 1 && Load(fc::process_counts[1]) == 1, "Process counted by type");
  Expect(Load(fc::process_prepare_ticks[2]) > Load(fc::process_request_ticks[2]) &&
             Load(fc::process_request_ticks[2]) > 0 && Load(fc::process_prepare_ticks[1]) == 0,
         "before the request and the request, timed draws only");
  // Texture sets.
  fc::TextureSetHistory history;
  const uint64_t a[2] = {1, 2}, b[2] = {3, 4}, sa[1] = {9};
  const uint64_t draws = Load(fc::texture_set_draws[1]), same = Load(fc::texture_set_same_previous[1]),
                 found = Load(fc::texture_set_in_history[1]);
  history.Note(1, 7, 0, 100, a, 2, sa, 1);  // new
  history.Note(1, 7, 0, 100, a, 2, sa, 1);  // the previous draw's, in the history
  history.Note(1, 7, 0, 100, b, 2, sa, 1);  // new
  history.Note(1, 7, 0, 100, a, 2, sa, 1);  // in the history, not the previous
  history.Note(1, 7, 1, 100, a, 2, sa, 1);  // another epoch: new
  history.Note(1, 8, 0, 100, a, 2, sa, 1);  // another frame: the history starts again
  history.Note(1, 8, 0, 100, a, 2, sa, 0);  // the samplers' count is part of the key
  Expect(Load(fc::texture_set_draws[1]) - draws == 7, "every set counted");
  Expect(Load(fc::texture_set_same_previous[1]) - same == 1, "the previous draw's set");
  Expect(Load(fc::texture_set_in_history[1]) - found == 2, "found among the last of the frame");
  // Float constant writes and system constants.
  fc::NoteFloatConstantWrite(3, true, true);
  fc::NoteFloatConstantWrite(300, false, false);
  fc::NoteFloatConstantWrite(300, false, true);
  Expect(Load(fc::constant_writes[0][fc::kConstantIdentical]) == 1 &&
             Load(fc::constant_writes[1][fc::kConstantUnused]) == 1 &&
             Load(fc::constant_writes[1][fc::kConstantUsed]) == 1,
         "constant writes by stage and class");
  fc::NoteSystemConstants(true);
  fc::NoteSystemConstants(false);
  Expect(Load(fc::system_constant_calls) == 2 && Load(fc::system_constant_changes) == 1, "system constants");
  // Outcomes.
  fc::NoteResolveOutcome(fc::kOutcomeCarried);
  Expect(Load(fc::resolve_outcomes[fc::kOutcomeCarried]) == 1, "a carried-over copy");
  // Hot registers: a type 0 packet at 0x2102.
  fc::packet_sample_every.store(1);
  { fc::PacketTimer hot(0x00002102u); }
  Expect(Load(fc::packet_hot_counts[0]) == 1, "type 0 at 2102 counted apart");
  fc::packet_sample_every.store(16);
  // Faults: 16 writes to a watched page at one host address, resolved by a stand-in.
  fc::guest_pc_resolver.store(+[](uint64_t host) -> uint32_t { return uint32_t(host); });
  fc::fault_sample.store(0);
  for (int i = 0; i < 16; ++i) fc::NoteFault(fc::kFaultWriteWatch, 0x82001234u);
  fc::NoteFault(fc::kFaultMmio, 0x82005678u);
  Expect(Load(fc::faults[fc::kFaultWriteWatch]) == 16 && Load(fc::faults[fc::kFaultMmio]) == 1, "faults by kind");
  uint64_t write_watch = 0;
  for (const fc::FaultSlot& slot : fc::fault_slots)
    if (slot.key.load() == ((uint64_t(0x82001234u) << 8) | (fc::kFaultWriteWatch + 1))) write_watch = slot.count.load();
  Expect(write_watch == 16, "one in 8 resolved, scaled back");
  fc::diag_detail.store(false);
  fc::NoteFault(fc::kFaultMmio, 1);
  Expect(Load(fc::faults[fc::kFaultMmio]) == 1, "no faults counted with the switch off");
  fc::diag_detail.store(true);
}

// (c)
void CheckCost() {
  const fc::TimerCost cost = fc::EstimatePacketTimerCost(2048);
  Expect(std::isfinite(cost.inside_ticks) && std::isfinite(cost.outside_ticks) && cost.inside_ticks >= 0 &&
             cost.outside_ticks >= 0,
         "the timer's cost is finite and not negative");
  Expect(cost.inside_ticks + cost.outside_ticks > 0, "the timer costs something");
  Expect(cost.inside_ticks + cost.outside_ticks < 100000, "and not absurdly much (ticks per timer)");
  Expect(fc::EstimatePacketTimerCost(0).inside_ticks == 0, "no rounds, no estimate");
  // A first line's tick length: from the program's start (a few ms ago), near the TSC's real rate.
  Spin(2000000);
  const double origin = fc::OriginNsPerTick();
  Expect(origin > 0.05 && origin < 5.0, "the ticks' length from the program's start (" + std::to_string(origin) + ")");
}

// (d)
void CheckLines() {
  fc::ProbeReporter reporter;
  const fc::TimerCost cost{40.0, 60.0};  // ticks; 1 ns a tick below
  reporter.Lines(1.0, 1.0, &cost);       // the baseline
  // 2 s: executing 1,600 ms with every wait in it.
  fc::totals.execute_ns.fetch_add(1600000000ull);
  fc::totals.guest_wait_ns.fetch_add(200000000ull);
  fc::totals.guest_waits.fetch_add(400);
  fc::totals.wait_ns[fc::kPipeline].fetch_add(10000000ull);
  fc::section_ticks[fc::kDraw].fetch_add(800000000ull);
  fc::section_ticks[fc::kWait].fetch_add(100000000ull);
  fc::section_ticks[fc::kUpload].fetch_add(100000000ull);
  // Packets: 10 top packets, 5 timed (scale 2); an IB of 1,000 ticks own with 40 timed packets nested
  // in 2 timed IBs.
  fc::packet_top.fetch_add(10);
  fc::packet_top_sampled.fetch_add(5);
  fc::packet_ticks[fc::kPacketIndirect].fetch_add(1000000);
  fc::packet_counts[fc::kPacketIndirect].fetch_add(2);
  fc::packet_indirect_nested.fetch_add(40);
  fc::packet_register_ticks[fc::kRegFloat].fetch_add(300000);
  fc::packet_register_counts[fc::kRegFloat].fetch_add(3000);
  // Draws: 80 draws, 10 timed (scale 8); Process 1,000,000 ticks timed.
  fc::bind_draws.fetch_add(80);
  fc::bind_sampled.fetch_add(10);
  fc::draw_sub_ticks[fc::kSubProcess].fetch_add(1000000);
  fc::draw_sub_ticks[fc::kSubConfigurePipeline].fetch_add(500000);
  // Memexport waits: 4 small exports of 5 ms, 512 B each.
  fc::memexport_wait_ns[2].fetch_add(20000000ull);
  fc::memexport_waits[2].fetch_add(4);
  fc::memexport_wait_bytes[2].fetch_add(2048);
  // Every packet: 1,000 EVENT_WRITEs counted one by one, 10 timed at 300 ticks each (fix 75's scale:
  // 10 x 2 = 20 over 2 s, 10/s; one by one 500/s); WAIT_REG_MEM: 4 timed holding 8 ms of sleeping.
  fc::packet_all_counts[fc::kPacketEvents].fetch_add(1000);
  fc::packet_ticks[fc::kPacketEvents].fetch_add(3000);
  fc::packet_counts[fc::kPacketEvents].fetch_add(10);
  fc::packet_wait_reg_sleep_ns.fetch_add(8000000ull);
  // Resolve waits: 2 full mode waits of 30 ms, 64 KiB each.
  fc::resolve_wait_ns[fc::kResolveFullMode].fetch_add(60000000ull);
  fc::resolve_waits[fc::kResolveFullMode].fetch_add(2);
  fc::resolve_wait_bytes[fc::kResolveFullMode].fetch_add(131072);
  const std::string lines = reporter.Lines(2.0, 1.0, &cost);
  std::printf("%s\n", lines.c_str());
  // executing 800 ms/s; sections 400 + 50 (waits) + 50 = 500; WAIT_REG_MEM 100; rest 200.
  Expect(Has(lines, "executing 800 ms/s with every wait in it"), "executing");
  Expect(Has(lines, "draws 400.0") && Has(lines, "uploads 50.0") && Has(lines, "(draws hold pipeline waits 5.0)"),
         "the sections and the pipeline waits in the draws");
  Expect(Has(lines, "GPU waits 50.0 ms/s; WAIT_REG_MEM sleeps 100.0 ms/s in 200/s"), "the waits");
  Expect(Has(lines, "the rest of executing (packets, registers, constants) without waits 200.0 ms/s"), "the rest");
  Expect(Has(lines, "the timer costs about 40 ns inside a timed packet and 60 ns outside it"), "the timer's cost");
  // IB: 1,000,000 ticks x 2 = 1 ms/s; 4/2 s = 2/s; 80 nested / 4 IBs = 20 each; 80 x 60 ns / 2 s.
  Expect(Has(lines, "INDIRECT_BUFFER own 1.0 ms/s in 2/s with 20.0 timed packets nested each (25000 ns of its own "
                    "time per nested packet), of which about 0.0 ms/s is their timers"),
         "the INDIRECT_BUFFER's figures");
  // Float constants: 300,000 x 2 / 2 s = 0.3 ms/s in 3,000/s, 100 ns each.
  Expect(Has(lines, "float constants 0.3 ms/s in 3000/s (100 ns each)"), "type 0 float constants");
  // Process: 1,000,000 x 8 / 2 s = 4 ms/s; ConfigurePipeline 2 ms/s.
  Expect(Has(lines, "primitives: Process 4.0") && Has(lines, "; pipeline: ConfigurePipeline 2.0"), "the sub-parts");
  Expect(Has(lines, "Frame time readback waits (GPU thread, fix 142f-diag): resolves 30.0 ms/s (full mode 30.0 ms/s in "
                    "1.0/s (64 KiB each)"),
         "the resolve waits");
  Expect(Has(lines, "; memexport 10.0 ms/s (unnamed 0.0 ms/s in 0.0/s (0.0 KiB each), the option 0.0 ms/s in 0.0/s "
                    "(0.0 KiB each), small export (fix 30) 10.0 ms/s in 2.0/s (0.5 KiB each)"),
         "the memexport waits");
  Expect(Has(lines, "GPU diagnostics detail (fix 142f-diag): on"), "the switch's state first");
  Expect(Has(lines, "Frame time draw detail (GPU thread, fix 142f-diag): Process by index buffer type: none ") &&
             Has(lines, "texture sets: vertex ") && Has(lines, "float constant writes: vertex identical ") &&
             Has(lines, "system constants ") && Has(lines, "type 0 packets at the hot registers: 2102 "),
         "the draws' detail line");
  Expect(Has(lines, "Frame time faults (fix 142f-diag, every thread): MMIO ") &&
             Has(lines, "guest code faulting most (1 in 8 resolved, 00000000 the emulator's own): "),
         "the faults line");
  Expect(Has(lines, "fast copies without a wait: the earlier copy handed over ") &&
             Has(lines, "the resolves' own time "),
         "the fast copies and the resolves' own time");
  Expect(Has(lines, "EVENT_WRITE* 500/s (fix 75's scaled 10/s), 300 ns each, 0.1 ms/s"),
         "every packet counted, own time from the timed ones");
  Expect(Has(lines, "WAIT_REG_MEM's own time holds 8.0 ms/s of sleeping"), "the sleeps inside WAIT_REG_MEM");
  Expect(std::count(lines.begin(), lines.end(), '\n') == 6, "seven lines");
}

// (e)
void CheckTree() {
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string main = Read("src/canary_game_main.cpp");
  Expect(!cp.empty() && !main.empty(), "the tree's files are there (run from the assembled tree's root)");
  const size_t draw = cp.find("bool VulkanCommandProcessor::IssueDrawInner(");
  const size_t primitive_mark = cp.find("draw_timer.Mark(fc::kDrawPrimitive);", draw);
  const size_t targets_mark = cp.find("draw_timer.Mark(fc::kDrawTargets);", draw);
  const size_t pipeline_mark = cp.find("draw_timer.Mark(fc::kDrawPipeline);", draw);
  Expect(draw != std::string::npos && primitive_mark != std::string::npos && pipeline_mark > targets_mark,
         "fix 73's marks in IssueDrawInner");
  struct Sub { const char* part; const char* mark; size_t from, to; };
  const Sub subs[] = {
      {"kSubProcess", "process", draw, primitive_mark},
      {"kSubModifications", "modifications", draw, primitive_mark},
      {"kSubTranslations", "translations", draw, primitive_mark},
      {"kSubSamplers", "samplers", draw, primitive_mark},
      {"kSubWindowOffset", "window", draw, primitive_mark},
      {"kSubConfigurePipeline", "configure", targets_mark, pipeline_mark},
      {"kSubPipelineReady", "ready", targets_mark, pipeline_mark},
      {"kSubRequestTextures", "textures", targets_mark, pipeline_mark},
      {"kSubBindPipeline", "bind", targets_mark, pipeline_mark},
      {"kSubViewport", "viewport", targets_mark, pipeline_mark},
      {"kSubQuery", "query", targets_mark, pipeline_mark},
  };
  for (const Sub& sub : subs) {
    const std::string start = std::string("const fc::BindMark fix142f_") + sub.mark + " = fc::BindStart();";
    const std::string stop = std::string("fc::DrawSubStop(fc::") + sub.part + ", fix142f_" + sub.mark + ");";
    const size_t a = cp.find(start, sub.from), b = cp.find(stop, sub.from);
    Expect(a != std::string::npos && b != std::string::npos && a < b && b < sub.to,
           std::string(sub.part) + " starts, then stops, inside its fix 73 part");
  }
  // Process is around PrimitiveProcessor::Process, ConfigurePipeline around its call.
  const size_t process = cp.find("primitive_processor_->Process(primitive_processing_result)", draw);
  Expect(cp.rfind("fix142f_process = fc::BindStart();", process) > draw &&
             cp.find("fc::DrawSubStop(fc::kSubProcess", process) < cp.find("// Shader modifications.", process),
         "Process's sub-part is around the call");
  const size_t configure = cp.find("pipeline_cache_->ConfigurePipeline(", targets_mark);
  Expect(cp.find("fix142f_configure = fc::BindStart();", targets_mark) < configure &&
             cp.find("fc::DrawSubStop(fc::kSubConfigurePipeline", targets_mark) > configure,
         "ConfigurePipeline's sub-part is around the call");
  // Every wait of the kResolve cause names its reason.
  size_t at = 0, resolve_scopes = 0;
  static const std::string cause = "xbox360ps5::frame_cost::kResolve);";
  while ((at = cp.find(cause, at)) != std::string::npos) {
    ++resolve_scopes;
    const size_t reason = cp.find("ResolveReasonScope fix142f_reason(", at);
    const size_t next_line_block = cp.find("#endif", at);
    Expect(reason != std::string::npos && reason < next_line_block,
           "a kResolve wait at offset " + std::to_string(at) + " names its reason");
    at += cause.size();
  }
  Expect(resolve_scopes == 3, "three kResolve waits (full mode, a copy missing, downscale buffer growth)");
  const size_t memexport = cp.find("xbox360ps5::frame_cost::kMemexport);");
  Expect(memexport != std::string::npos &&
             cp.find("MemexportReasonScope fix142f_memexport(", memexport) < cp.find("#endif", memexport) &&
             cp.find("uint32_t(memexport_reason), memexport_total_size);", memexport) < cp.find("#endif", memexport),
         "the memexport readback wait names its reason (fix 48's Decide)");
  Expect(cp.find("xbox360ps5::frame_cost::kMemexport);", memexport + 1) == std::string::npos,
         "one memexport readback wait");
  Expect(Has(cp, "own_copy && newest_copy >= 0 ? xbox360ps5::frame_cost::kResolveOwnCopy"),
         "own copy against first copy");
  // Each reason at its own wait (the two waits' lines are alike: a patch applied with too little
  // context once swapped them).
  const size_t full = cp.find("// Wait for GPU to finish (accurate but slow)");
  Expect(full != std::string::npos &&
             cp.find("kResolveFullMode, readback_length", full) < cp.find("} else if (read_cache_miss", full),
         "full mode named at the full mode wait");
  const size_t fallback = cp.find("      read_index = write_index;\n#if XE_PLATFORM_PS5");
  Expect(fallback != std::string::npos &&
             cp.find("kResolveOwnCopy", fallback) < cp.find("resolve readback fallback", fallback),
         "first copy or own copy named at the fallback wait");
  const size_t growth = cp.find("// The old buffer may still be referenced by commands recorded for");
  Expect(growth != std::string::npos &&
             cp.find("kResolveGrowth, size", growth) < cp.find("resolve_downscale_buffer_size_ = 0;", growth),
         "growth named at the downscale buffer's wait");
  // The query sub-part around BindVIZPredicate and UpdateQuerySegment, the viewport's second half
  // after them.
  const size_t predicate = cp.find("BindVIZPredicate(predicate_offset);", targets_mark);
  const size_t segment = cp.find("UpdateQuerySegment(", predicate);
  Expect(cp.rfind("fix142f_query = fc::BindStart();", predicate) > targets_mark &&
             cp.find("fc::DrawSubStop(fc::kSubQuery, fix142f_query);", segment) <
                 cp.find("GetHostViewportInfo(", segment) &&
             cp.find("fc::DrawSubStop(fc::kSubViewport, fix142f_viewport_args);", segment) < pipeline_mark,
         "the query sub-part, and the viewport's arguments after it");
  // Process by index buffer type.
  const std::string pp = Read(".deps/xenia-canary/src/xenia/gpu/primitive_processor.cc");
  const size_t process_def = pp.find("bool PrimitiveProcessor::Process(ProcessingResult& result_out) {");
  const size_t request_comment = pp.find("// Request the indices in the shared memory", process_def);
  const size_t note = pp.find("xbox360ps5::frame_cost::NoteProcess(uint32_t(cacheable.index_buffer_type)", process_def);
  Expect(process_def != std::string::npos &&
             pp.find("fix142f_prepare = xbox360ps5::frame_cost::BindStart();", process_def) < request_comment &&
             pp.find("fix142f_request = xbox360ps5::frame_cost::BindStart();", process_def) < request_comment &&
             note > pp.find("RequestRange(guest_index_base", process_def) && note < pp.find("return true;\n}", process_def),
         "Process: started at its top, the request marked before it, noted before its last return");
  // Texture sets, constant writes, system constants.
  const size_t note_sets = cp.find("xbox360ps5::frame_cost::texture_set_history.Note(");
  Expect(note_sets != std::string::npos && note_sets < cp.find("if (written.Matches(frame_current_", note_sets),
         "the texture sets noted before fix 104's check");
  const size_t write_register = cp.find("void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {");
  Expect(cp.find("if (!fix142f_in_run) {", write_register) < cp.find("CommandProcessor::WriteRegister(index, value);", write_register) &&
             cp.find("if (!fix142f_in_run) {", write_register) > cp.find("// Fix 105:", write_register) &&
             cp.find("if (!fix142f_in_run) {", write_register) < cp.find("// Only plain shader constants are eligible.", write_register),
         "a single register write classified after fix 105's fetch skip, before the float skip");
  const size_t swapped = cp.find("void VulkanCommandProcessor::WriteRegistersSwapped(");
  Expect(cp.find("Fix142fClassifyFloatConstants(register_file_->values, start_index, source", swapped) <
             cp.find("register_run::Write(", swapped) &&
             cp.find("fix142f_in_run = true;", swapped) < cp.find("register_run::Write(", swapped),
         "a run classified before it is written, once");
  const size_t system = cp.find("void VulkanCommandProcessor::UpdateSystemConstantValues(");
  Expect(cp.find("NoteSystemConstants(dirty);", system) < cp.find("\n}\n", cp.find("if (dirty) {", system + 1) ),
         "the system constants noted at the end of their update");
  // Outcomes, faults, the device's descriptor indexing, the switch.
  Expect(cp.find("kOutcomeFamilyLate") > cp.find("++readback_family_late_;") &&
             cp.find("kOutcomeCarried") > cp.find("NoteLateReadback();") &&
             cp.find("kOutcomeHanded") > cp.find("memory::vastcpy(memory_->TranslatePhysical(written_address)"),
         "the fast copies' outcomes at their places");
  const std::string mmio = Read(".deps/xenia-canary/src/xenia/cpu/mmio_handler.cc");
  Expect(Has(mmio, "NoteFault(xbox360ps5::frame_cost::kFaultCleared") &&
             Has(mmio, "? xbox360ps5::frame_cost::kFaultWriteWatch") &&
             Has(mmio, ": xbox360ps5::frame_cost::kFaultReadWatch") &&
             mmio.find("NoteFault(xbox360ps5::frame_cost::kFaultMmio") < mmio.find("TryDecodeLoadStore(p, decoded_load_store)"),
         "the faults by kind in MMIOHandler::ExceptionCallback");
  const std::string backend = Read(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_backend.cc");
  Expect(Has(backend, "xbox360ps5::frame_cost::guest_pc_resolver.store(") &&
             backend.find("#include \"xenia/base/platform.h\"") < backend.find("#if XE_PLATFORM_PS5"),
         "the JIT's code map resolves the faults' guest addresses");
  const std::string device = Read(".deps/xenia-canary/src/xenia/ui/vulkan/vulkan_device.cc");
  Expect(Has(device, "Vulkan descriptor indexing (fix 142f-diag, supported)") &&
             Has(device, "maxPerStageDescriptorUpdateAfterBindSampledImages"),
         "the device's descriptor indexing in its dump");
  const size_t end_submission = cp.find("bool VulkanCommandProcessor::EndSubmission(bool is_swap) {");
  Expect(cp.find("RefreshDiagDetail(XBOX360PS5_DATA_ROOT \"/gpu-diag-detail.txt\")", end_submission) <
             cp.find("\n}\n", end_submission),
         "the switch read at the end of a submission");
  // The lines, after fix 142d's.
  const size_t bindings = main.find("frame_cost_bindings.Lines(double(summary_samples))");
  const size_t probe = main.find("frame_cost_probe.Lines(double(summary_samples))");
  Expect(bindings != std::string::npos && probe > bindings, "the lines right after fix 142d's");
}

// (g)
void CheckPatch(const std::string& fixes) {
  const std::string patch = Read(fixes + "/142f-diag-gpu-thread.patch");
  Expect(!patch.empty(), "the fix's patch is there");
  std::istringstream lines(patch);
  std::string line;
  int removed = 0, marked = 0;
  while (std::getline(lines, line)) {
    if (line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0) continue;
    if (!line.empty() && line[0] == '-') ++removed;
    if (!line.empty() && line[0] == '+' && line.find("142f-diag") != std::string::npos) ++marked;
  }
  Expect(removed <= 20, "few lines changed (the reporters' first tick length, fix 142d's switch): " +
                            std::to_string(removed));
  Expect(marked >= removed, "each changed line comes back marked fix 142f-diag");
}

}  // namespace

int main(int argc, char** argv) {
  CheckSwitch();
  CheckResolveReasons();
  CheckPackets();
  CheckCost();
  CheckDraws();
  CheckLines();
  const bool model_only = argc > 1 && std::string(argv[1]) == "--model-only";
  if (!model_only) {
    CheckTree();
    if (argc > 1) CheckPatch(argv[1]);
  }
  if (failures) {
    std::printf("check-gpu-thread-probe: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-gpu-thread-probe: ok\n");
  return 0;
}
