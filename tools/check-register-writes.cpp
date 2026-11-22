// SPDX-License-Identifier: MIT
// Fix 103 (tools/source-1.0.2/fixes/103-register-writes.patch): a run of registers (type 0
// packets, SET_CONSTANT, LOAD_ALU_CONSTANT and the like) is written a block at a time. Grand Theft
// Auto IV (545407F2) on 1.0.5-work-fix93e: 1.36 million type 0 packets a second (about 7 per
// draw), 132 ms/s of the GPU command thread, 97 ns each (Modern Warfare 2: 55 ns); each register
// was read by an out-of-line RingBuffer::Read and written through two out-of-line WriteRegister
// calls. Checked:
//  (a) xenia/gpu/register_run.h, the code the console runs, against a register-by-register model
//      of VulkanCommandProcessor::WriteRegister (and the base class's) for the shader constants:
//      random runs across the float, fetch and bool/loop ranges and the registers around them,
//      with repeated values, vulkan_skip_redundant_constants on and off, a frame open or not and
//      random constant maps: the same register file, the same constant buffers outdated, the same
//      texture bindings out of sync, the same other registers written in the same order; a run
//      split where the ring wraps gives the same as the whole run (with fix 105, an unchanged
//      fetch constant skipped as an unchanged float constant is);
//  (b) include/xbox360ps5/frame_cost.hpp: the type 0 packets counted by register group and first
//      register, and the summary line naming the registers written most;
//  (c) the assembled tree: VulkanCommandProcessor's WriteRegister still has the effects the model
//      copies; the console build writes runs from the ring in place (and from memory) through
//      register_run.h, its ranges checked against register_table.inc; the type 0 packets are
//      counted and the summary gives the line after fix 96's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"
#include "xenia/gpu/register_run.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;
// Fix 105 (on top of this one, unless a build leaves it out): an unchanged fetch constant is
// skipped. Set from the tree before the runs are compared.
bool fix105 = false;
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
namespace rr = xe::gpu::register_run;
namespace fc = xbox360ps5::frame_cost;

constexpr uint32_t kRegisterCount = 0x5003;
constexpr uint32_t kFloatVertex = 1u << 0, kFloatPixel = 1u << 1, kFetch = 1u << 4, kBoolLoop = 1u << 2,
                   kTessellation = 1u << 5, kClip = 1u << 6;

struct Processor {
  std::vector<uint32_t> regs = std::vector<uint32_t>(kRegisterCount, 0);
  bool skip = true, frame_open = true;
  uint64_t map_vertex[4] = {}, map_pixel[4] = {};
  uint32_t up_to_date = ~0u;
  uint32_t texture_in_sync = ~0u;
  std::vector<std::pair<uint32_t, uint32_t>> others;  // other registers, in order

  // TextureCache (texture_cache.h).
  void TextureFetchConstantWritten(uint32_t index) { texture_in_sync &= ~(UINT32_C(1) << index); }
  void TextureFetchConstantsWritten(uint32_t first_index, uint32_t last_index) {
    uint32_t res = ((1U << first_index) - 1) ^ static_cast<uint32_t>((1ULL << (last_index + 1)) - 1ULL);
    texture_in_sync &= ~res;
  }

  // VulkanCommandProcessor::WriteRegister, register by register (the constants; any other
  // register is recorded, as the fast path hands it to WriteRegister).
  void WriteRegister(uint32_t index, uint32_t value) {
    const bool in_constants = (index >= 0x4000 && index <= 0x47FF) || (index >= 0x4800 && index <= 0x48BF) ||
                              (index >= 0x4900 && index <= 0x4927);
    if (!in_constants) {
      others.emplace_back(index, value);
      return;
    }
    if (fix105 && skip && index >= 0x4800 && index <= 0x48BF && regs[index] == value) return;
    if (skip && index >= 0x4000 && index <= 0x47FF && regs[index] == value) return;
    const bool constant_bits_changed = index < kRegisterCount && regs[index] != value;
    regs[index] = value;  // CommandProcessor::WriteRegister: nothing special in these ranges
    if (skip && !constant_bits_changed && !(index >= 0x4800 && index <= 0x48BF)) return;
    if (index >= 0x4000 && index <= 0x47FF) {
      if (frame_open) {
        uint32_t f = (index - 0x4000) >> 2;
        if (f >= 256) {
          f -= 256;
          if (map_pixel[f >> 6] & (1ull << (f & 63))) up_to_date &= ~kFloatPixel;
        } else {
          if (map_vertex[f >> 6] & (1ull << (f & 63))) up_to_date &= ~kFloatVertex;
        }
      }
    } else if (index >= 0x4900 && index <= 0x4927) {
      up_to_date &= ~kBoolLoop;
    } else if (index >= 0x4800 && index <= 0x48BF) {
      up_to_date &= ~kFetch;
      TextureFetchConstantWritten((index - 0x4800) / 6);
    }
  }
  void Slow(uint32_t start, const std::vector<uint32_t>& big_endian) {
    for (uint32_t i = 0; i < big_endian.size(); ++i) WriteRegister(start + i, __builtin_bswap32(big_endian[i]));
  }
  void Fast(uint32_t start, const uint32_t* big_endian, uint32_t count) {
    const rr::State state{regs.data(), skip, frame_open, map_vertex, map_pixel,
                          {kFloatVertex, kFloatPixel, kFetch, kBoolLoop}, &up_to_date};
    rr::Write(state, start, big_endian, count,
              [this](uint32_t first, uint32_t last) { TextureFetchConstantsWritten(first, last); },
              [this](uint32_t index, uint32_t value) { others.emplace_back(index, value); });
  }
  bool operator==(const Processor& o) const {
    return regs == o.regs && up_to_date == o.up_to_date && texture_in_sync == o.texture_in_sync && others == o.others;
  }
};

void CheckRuns() {
  std::mt19937 random(103);
  int runs = 0, mismatches = 0, split_mismatches = 0;
  const uint32_t starts[] = {0x3FF0, 0x4000, 0x4010, 0x43FC, 0x47F0, 0x47FE, 0x4800, 0x4806, 0x48BA,
                             0x48C0, 0x48FC, 0x4900, 0x4920, 0x4925, 0x4930, 0x2180, 0x0578};
  for (int round = 0; round < 6000; ++round) {
    Processor slow;
    slow.skip = random() % 4 != 0;
    slow.frame_open = random() % 5 != 0;
    for (int w = 0; w < 4; ++w) {
      slow.map_vertex[w] = (uint64_t(random()) << 32) | random();
      slow.map_pixel[w] = (uint64_t(random()) << 32) | random();
    }
    for (uint32_t r = 0x3F00; r < 0x4A00; ++r) slow.regs[r] = random() % 3;  // repeats likely
    slow.up_to_date = random() % 2 ? ~0u : (~0u & ~(kTessellation | kClip));
    slow.texture_in_sync = random();
    Processor fast = slow, split = slow;
    const uint32_t start = starts[random() % (sizeof(starts) / sizeof(starts[0]))] + random() % 8;
    const uint32_t count = 1 + random() % (random() % 3 ? 24 : 300);
    std::vector<uint32_t> words(count);
    for (uint32_t& w : words) w = __builtin_bswap32(random() % 3 ? random() % 3 : random());
    slow.Slow(start, words);
    fast.Fast(start, words.data(), count);
    const uint32_t cut = random() % (count + 1);  // where the ring wraps
    split.Fast(start, words.data(), cut);
    split.Fast(start + cut, words.data() + cut, count - cut);
    ++runs;
    if (!(fast == slow)) {
      if (!mismatches)
        std::printf("   first mismatch: start %04X count %u skip %d frame_open %d\n", start, count, slow.skip,
                    slow.frame_open);
      ++mismatches;
    }
    if (!(split == slow)) ++split_mismatches;
  }
  std::printf("   %d random runs compared with the register-by-register path\n", runs);
  Expect(mismatches == 0, "a run written a block at a time has every effect of WriteRegister per register (" +
                              std::to_string(mismatches) + " differ)");
  Expect(split_mismatches == 0, "a run split where the ring wraps gives the same");

  // Fix 105: a whole fetch constant rewritten with the same bits changes nothing with the option
  // on, and puts its texture out of sync with it off (as before fix 105).
  Processor p;
  p.texture_in_sync = ~0u;
  std::vector<uint32_t> same(6, 0);
  p.Fast(0x4800 + 6 * 5, same.data(), 6);
  if (fix105) {
    Expect(p.texture_in_sync == ~0u && (p.up_to_date & kFetch), "fetch constants rewritten unchanged: still in sync");
  } else {
    Expect(p.texture_in_sync == ~(1u << 5) && !(p.up_to_date & kFetch),
           "fetch constants rewritten unchanged: out of sync (without fix 105)");
  }
  p.texture_in_sync = ~0u;
  p.up_to_date = ~0u;
  p.skip = false;
  p.Fast(0x4800 + 6 * 5, same.data(), 6);
  Expect(p.texture_in_sync == ~(1u << 5) && !(p.up_to_date & kFetch),
         "fetch constants rewritten unchanged with the option off: out of sync");
  // Unchanged float constants with the option on mark nothing.
  Processor q;
  q.map_vertex[0] = ~0ull;
  std::vector<uint32_t> zeros(16, 0);
  q.Fast(0x4000, zeros.data(), 16);
  Expect(q.up_to_date == ~0u, "unchanged float constants mark nothing");
}

void CheckCounting() {
  fc::RegisterReporter reporter;
  (void)reporter.Line(1);
  fc::CountRegisterWrites(0x4000, 8);  // not the GPU thread: not counted
  std::thread([] {
    fc::MarkGpuThread();
    for (int n = 0; n < 10; ++n) fc::CountRegisterWrites(0x4000, 8);
    for (int n = 0; n < 4; ++n) fc::CountRegisterWrites(0x4806, 6);
    for (int n = 0; n < 6; ++n) fc::CountRegisterWrites(0x2180, 1);
    fc::CountRegisterWrites(0x4900, 2);
  }).join();
  Expect(fc::RegisterGroupOf(0x3FFF) == fc::kRegOther && fc::RegisterGroupOf(0x47FF) == fc::kRegFloat &&
             fc::RegisterGroupOf(0x48BF) == fc::kRegFetch && fc::RegisterGroupOf(0x48C0) == fc::kRegOther &&
             fc::RegisterGroupOf(0x4927) == fc::kRegBoolLoop && fc::RegisterGroupOf(0x4928) == fc::kRegOther,
         "register groups by register_table.inc");
  const std::string line = reporter.Line(2);
  std::printf("   %s\n", line.c_str());
  Expect(line == "Performance register writes (GPU thread, every type 0 packet): 10 packets/s, 56 registers/s "
                 "(5.3 per packet); float constants 5/s (40 registers/s), fetch constants 2/s (12 registers/s), "
                 "bool/loop constants 0/s (1 registers/s), other registers 3/s (3 registers/s); "
                 "first registers written most: 4000 5/s, 2180 3/s, 4806 2/s, 4900 0/s",
         "the line by group and by first register");
  Expect(reporter.Line(1) == "Performance register writes (GPU thread, every type 0 packet): 0 packets/s, 0 registers/s (0.0 per packet)",
         "only since the previous line");
}

size_t After(const std::string& text, const std::string& what, size_t from = 0) {
  const size_t at = text.find(what, from);
  return at == std::string::npos ? std::string::npos : at;
}

void CheckTree() {
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  // The model's source: VulkanCommandProcessor::WriteRegister.
  const size_t wr = After(vk, "void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {");
  const size_t wr_end = After(vk, "void VulkanCommandProcessor::WriteRegistersFromMem(", wr);
  const std::string body = wr != std::string::npos && wr_end != std::string::npos ? vk.substr(wr, wr_end - wr) : "";
  for (const char* line : {
           "if (cvars::vulkan_skip_redundant_constants &&\n      index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&\n"
           "      index <= XE_GPU_REG_SHADER_CONSTANT_511_W &&\n      register_file_->values[index] == value) {\n    return;",
           "if (cvars::vulkan_skip_redundant_constants && !constant_bits_changed &&\n"
           "      !((index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) &&\n"
           "        (index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5))) {\n    return;",
           "if (frame_open_) {",
           "current_constant_buffers_up_to_date_ &= ~(\n              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);",
           "current_constant_buffers_up_to_date_ &= ~(\n              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);",
           "current_constant_buffers_up_to_date_ &=\n        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);",
           "current_constant_buffers_up_to_date_ &=\n        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);\n"
           "    if (texture_cache_) {\n      texture_cache_->TextureFetchConstantWritten(\n"
           "          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6);"}) {
    Expect(body.find(line) != std::string::npos, std::string("WriteRegister still has the effect the model copies: ") +
                                                      std::string(line).substr(0, 60));
  }
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/command_processor.cc");
  Expect(cp.find("unsigned expr = (index - XE_GPU_REG_SCRATCH_REG0 < 8) |\n                    (index == XE_GPU_REG_COHER_STATUS_HOST) |\n"
                 "                    ((index - XE_GPU_REG_DC_LUT_RW_INDEX) <=\n"
                 "                     (XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX));") != std::string::npos,
         "the base class's special registers are outside the constants (0578, 0A31, 1922-1925)");
  const std::string tex = Read(".deps/xenia-canary/src/xenia/gpu/texture_cache.h");
  Expect(tex.find("uint32_t res = ((1U << first_index) - 1) ^\n                   static_cast<uint32_t>((1ULL << (last_index + 1)) - 1ULL);") !=
             std::string::npos,
         "TextureFetchConstantsWritten is the mask the model uses");
  // The console build: runs from the ring in place, and from memory.
  const size_t inc = After(vk, "#if XE_PLATFORM_PS5\n#include \"xenia/gpu/register_run.h\"  // fix 103\n#endif");
  const size_t asserts = After(vk, "static_assert(register_run::kFloatFirst == XE_GPU_REG_SHADER_CONSTANT_000_X &&");
  const size_t ring = After(vk, "void VulkanCommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,");
  const size_t begin = After(vk, "xe::RingBuffer::ReadRange range = ring->BeginRead(bytes);", ring);
  const size_t fallback = After(vk, "CommandProcessor::WriteRegisterRangeFromRing(ring, base, num_registers);\n    return;", begin);
  const size_t endread = After(vk, "ring->EndRead(range);", begin);
  const size_t write = After(vk, "register_run::Write(\n      state, start_index, source, num_registers,", endread);
  const size_t other = After(vk, "VulkanCommandProcessor::WriteRegister(index, value);", write);
  Expect(inc != std::string::npos && asserts != std::string::npos && ring != std::string::npos && begin != std::string::npos &&
             fallback != std::string::npos && endread != std::string::npos && write != std::string::npos &&
             other != std::string::npos && fallback < endread,
         "the console writes a run from the ring in place through register_run.h, other registers through WriteRegister");
  Expect(vk.find("#if XE_PLATFORM_PS5\n  WriteRegistersSwapped(start_index, base, num_registers);  // fix 103\n#else") !=
             std::string::npos,
         "runs from memory too");
  const std::string vh = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  Expect(vh.find("  void WriteRegisterRangeFromRing(xe::RingBuffer* ring, uint32_t base,\n                                  uint32_t num_registers) override;") !=
             std::string::npos,
         "an override, so ExecutePacketType0's qualified call takes it");
  const std::string pm4 = Read(".deps/xenia-canary/src/xenia/gpu/pm4_command_processor_implement.h");
  const size_t t0 = After(pm4, "bool COMMAND_PROCESSOR::ExecutePacketType0(uint32_t packet) XE_RESTRICT {");
  const size_t count = After(pm4, "xbox360ps5::frame_cost::CountRegisterWrites(base_index, count);  // fix 103", t0);
  const size_t call = After(pm4, "COMMAND_PROCESSOR::WriteRegisterRangeFromRing(&reader_, base_index,", t0);
  Expect(t0 != std::string::npos && count != std::string::npos && call != std::string::npos && count < call,
         "every type 0 packet is counted");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t events = main_code.find("XELOGW(\"{}\", frame_cost_events.Line(double(summary_samples)));");
  const size_t registers = main_code.find("XELOGW(\"{}\", frame_cost_registers.Line(double(summary_samples)));");
  Expect(events != std::string::npos && registers != std::string::npos && events < registers,
         "the summary gives the line after fix 96's");
}

}  // namespace

int main() {
  {
    const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
    const std::string run = Read(".deps/xenia-canary/src/xenia/gpu/register_run.h");
    const bool in_write_register =
        vk.find("#if XE_PLATFORM_PS5\n  // Fix 105: a fetch constant written with the bits it has changes nothing") !=
        std::string::npos;
    const bool in_runs = run.find("if (state.skip_redundant && regs[fetch] == value) {") != std::string::npos;
    Expect(in_write_register == in_runs, "fix 105 is in both WriteRegister and register_run.h, or in neither");
    fix105 = in_write_register && in_runs;
    std::printf("   fix 105 %s\n", fix105 ? "applied: unchanged fetch constants skipped" : "not applied");
  }
  CheckRuns();
  CheckCounting();
  CheckTree();
  if (failures) {
    std::printf("check-register-writes: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-register-writes: ok\n");
  return 0;
}
