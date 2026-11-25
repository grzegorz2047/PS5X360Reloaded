// SPDX-License-Identifier: MIT
// Fix 155b (tools/source-1.0.2/fixes/155b-xma-progress.patch): "XMA hardware-like progress" behind
// <data>/xma-progress.txt, for Grand Theft Auto V's audio freeze.
//  (a) xma_progress.hpp: the switch file's text ("on" in any case, else off); a pass keeps one block
//      free with the switch on and none off; a context stays enabled only with the switch and the
//      decoder thread, still allocated, not locked meanwhile, with input and valid output;
//  (b) both behaviours on a model of the new decoder's fill: off, a 12-block ring kicked empty fills
//      in one pass, the write offset lands back on the read offset and output-valid is cleared (what
//      GTA V's stalled stream showed); on, the pass stops one frame short, output stays valid, and a
//      later pass with the game's space freed goes on;
//  (c) the assembled tree: the new decoder takes its free blocks through UsableBlocks, keeps a context
//      enabled through StayEnabled unless Disable() counted a lock meanwhile; the decoder reads the
//      switch once in Setup and logs the mode, and its thread waits 2 ms when contexts kept enabled
//      made no progress.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/xma_progress.hpp"

namespace {
namespace fs = std::filesystem;
namespace xp = xbox360ps5::xma_progress;

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

void Policy() {
  Expect(xp::ParseSwitch("on") && xp::ParseSwitch("  ON\r\n") && xp::ParseSwitch("\nOn\nanything") &&
             !xp::ParseSwitch("off") && !xp::ParseSwitch("") && !xp::ParseSwitch("onx") && !xp::ParseSwitch("1"),
         "(a) the switch: \"on\" in any case, else off");
  Expect(xp::ReserveBlocks(true) == 1 && xp::ReserveBlocks(false) == 0 && xp::UsableBlocks(12, true) == 11 &&
             xp::UsableBlocks(12, false) == 12 && xp::UsableBlocks(0, true) == 0,
         "(a) one block kept free with the switch, none without");
  Expect(xp::StayEnabled(true, true, false, true, true) && !xp::StayEnabled(false, true, false, true, true) &&
             !xp::StayEnabled(true, false, false, true, true) && !xp::StayEnabled(true, true, true, true, true) &&
             !xp::StayEnabled(true, true, false, false, true) && !xp::StayEnabled(true, true, false, true, false),
         "(a) a context stays enabled only in continuous mode, allocated, not locked, with input and output");
}

void Model() {
  const xp::Fill off = xp::ModelFill(12, 0, 0, true, 4, 100, false);
  Expect(off.written == 12 && off.write == 0 && !off.valid,
         "(b) off: one pass fills the 12-block ring, write back on read, output-valid cleared (GTA V's ctx 21)");
  const xp::Fill on = xp::ModelFill(12, 0, 0, true, 4, 100, true);
  Expect(on.written == 8 && on.write == 8 && on.valid, "(b) on: the pass stops a frame short, output stays valid");
  const xp::Fill later = xp::ModelFill(12, on.write, 6, on.valid, 4, 100, true);
  Expect(later.written == 8 && later.write == 4 && later.valid,
         "(b) on: when the game has read 6 blocks the next pass goes on, still one block short");
  const xp::Fill tight = xp::ModelFill(12, 0, 5, true, 4, 100, true);
  Expect(tight.written == 4 && tight.write == 4 && tight.valid, "(b) on: never onto the read offset");
  const xp::Fill short_input = xp::ModelFill(12, 0, 0, true, 4, 1, false);
  Expect(short_input.written == 4 && short_input.valid, "(b) input for one pass only: no full signal either way");
}

void Tree() {
  const std::string apu = ".deps/xenia-canary/src/xenia/apu/";
  const std::string ctx = Read(apu + "xma_context_new.cc"), h = Read(apu + "xma_context_new.h");
  Expect(ctx.find("remaining_subframe_blocks_in_output_buffer_ = xbox360ps5::xma_progress::UsableBlocks(") !=
             std::string::npos,
         "(c) the new decoder's free blocks go through UsableBlocks");
  const size_t work = ctx.find("bool XmaContextNew::Work() {");
  const size_t guard = ctx.find("struct StayEnabled155b {", work);
  Expect(work != std::string::npos && guard != std::string::npos && guard < ctx.find("XMA_CONTEXT_DATA data(context_ptr);", work) &&
             ctx.find("context->disables_155b_.load(std::memory_order_acquire) != disables", guard) != std::string::npos &&
             ctx.find("disables_155b_.fetch_add(1, std::memory_order_acq_rel);") != std::string::npos &&
             h.find("std::atomic<uint32_t> disables_155b_{0};") != std::string::npos,
         "(c) a pass keeps its context enabled through StayEnabled unless a lock came meanwhile");
  const std::string dec = Read(apu + "xma_decoder.cc");
  Expect(dec.find("std::string(XBOX360PS5_DATA_ROOT) + \"/xma-progress.txt\"") != std::string::npos &&
             dec.find("XELOGW(\"XMA progress (fix 155b): {}{}\"") != std::string::npos &&
             dec.find("xbox360ps5::xma_progress::continuous = progress && hardware_decoder && dedicated_worker_;") !=
                 std::string::npos,
         "(c) the switch read once in Setup and logged");
  Expect(dec.find("xe::threading::Wait(work_event_.get(), false, std::chrono::milliseconds(2));  // fix 155b") !=
                 std::string::npos &&
             dec.find("if (did_work && (!continuous || progressed)) {") != std::string::npos,
         "(c) the decoder thread waits 2 ms when kept contexts made no progress");
  Expect(dec.find("if (auto line = trace155.Write0601(value, Trace155Ms())) XELOGW(\"{}\", *line);") !=
             std::string::npos,
         "(c) 0601h writes go to the trace");
}

}  // namespace

int main() {
  Policy();
  Model();
  if (fs::exists(".deps/xenia-canary/src/xenia/apu/xma_decoder.cc")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("xma progress: all checks passed\n");
  return 0;
}
