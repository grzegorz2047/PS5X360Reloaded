// SPDX-License-Identifier: MIT
// Fix 142f (research/parallel-gpu-commands/STAGE2.md, section 14): a diagnostic of the GPU command
// thread after fix 142e. Nothing it adds changes what is done.
//  (a) the resolve readback waits by reason: a ResolveReasonScope names the wait it encloses, a
//      fence wait of the kResolve cause counts under that reason (and its bytes), any other cause
//      or thread does not, and the reason comes back when the scope ends;
//  (b) the packet timer: an INDIRECT_BUFFER counts the timed packets nested in it, type 0 packets
//      count their own time by their first register's group;
//  (c) the timer's estimated cost is finite and not negative;
//  (d) the three lines' arithmetic and text (fixed tick length and timer cost);
//  (e) the assembled tree: every sub-part of fix 73's primitives and pipeline parts starts before it
//      stops and inside its part, every wait of the kResolve cause has a reason, the lines are
//      logged after fix 142d's;
//  (f) the patch only adds lines (argv[1]: the fixes folder).
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
  {
    fc::CauseScope cause(fc::kMemexport);
    fc::ResolveReasonScope reason(fc::kResolveFirstCopy, 1);
    fc::AddFenceWait(99);  // another cause: not a resolve wait
  }
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

// (b)
void CheckPackets() {
  fc::MarkGpuThread();
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

// (c)
void CheckCost() {
  const fc::TimerCost cost = fc::EstimatePacketTimerCost(2048);
  Expect(std::isfinite(cost.inside_ticks) && std::isfinite(cost.outside_ticks) && cost.inside_ticks >= 0 &&
             cost.outside_ticks >= 0,
         "the timer's cost is finite and not negative");
  Expect(cost.inside_ticks + cost.outside_ticks > 0, "the timer costs something");
  Expect(cost.inside_ticks + cost.outside_ticks < 100000, "and not absurdly much (ticks per timer)");
  Expect(fc::EstimatePacketTimerCost(0).inside_ticks == 0, "no rounds, no estimate");
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
  Expect(Has(lines, "INDIRECT_BUFFER own 1.0 ms/s in 2/s with 20.0 timed packets nested each, of which about "
                    "0.0 ms/s is their timers"),
         "the INDIRECT_BUFFER's figures");
  // Float constants: 300,000 x 2 / 2 s = 0.3 ms/s in 3,000/s, 100 ns each.
  Expect(Has(lines, "float constants 0.3 ms/s in 3000/s (100 ns each)"), "type 0 float constants");
  // Process: 1,000,000 x 8 / 2 s = 4 ms/s; ConfigurePipeline 2 ms/s.
  Expect(Has(lines, "primitives: Process 4.0") && Has(lines, "; pipeline: ConfigurePipeline 2.0"), "the sub-parts");
  Expect(Has(lines, "Frame time resolve waits (GPU thread, fix 142f): 30.0 ms/s; full mode 30.0 ms/s in 1.0/s "
                    "(64 KiB each)"),
         "the resolve waits");
  Expect(std::count(lines.begin(), lines.end(), '\n') == 2, "three lines");
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
  // The lines, after fix 142d's.
  const size_t bindings = main.find("frame_cost_bindings.Lines(double(summary_samples))");
  const size_t probe = main.find("frame_cost_probe.Lines(double(summary_samples))");
  Expect(bindings != std::string::npos && probe > bindings, "the lines right after fix 142d's");
}

// (f)
void CheckPatch(const std::string& fixes) {
  const std::string patch = Read(fixes + "/142f-gpu-thread-probe.patch");
  Expect(!patch.empty(), "the fix's patch is there");
  std::istringstream lines(patch);
  std::string line;
  int removed = 0;
  while (std::getline(lines, line)) {
    if (line.rfind("--- ", 0) == 0) continue;
    if (!line.empty() && line[0] == '-') ++removed;
  }
  Expect(removed == 0, "the patch only adds lines: what the emulator does is unchanged");
}

}  // namespace

int main(int argc, char** argv) {
  CheckResolveReasons();
  CheckPackets();
  CheckCost();
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
