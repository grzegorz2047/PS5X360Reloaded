// SPDX-License-Identifier: MIT
// Fix 136 (xenia/gpu/memexport_batch.h, vulkan_command_processor.cc, the command processor's
// sync points): memory exports read back once per sync period. Fix 133's counters on the console:
// Kinect Sports' boxing 42,103-55,894 waits in 3,458-3,516 periods per 30 s (129-192 ms/s of
// waiting), Modern Warfare 2 3.9-8.4 waits per period (56-83 ms/s).
//  (a) memexport-batching.txt: on, off, comments, lines not understood;
//  (b) the batch: each export's place (aligned, after the previous one), full and too big read
//      back at once, a pending range the CPU wrote (invalid) seen;
//  (c) the merge, deferred, with fix 30's ApplyExports: each export's dwords in export order (a
//      later export of the same dword wins), the dwords the CPU wrote meanwhile and the shader did
//      not kept;
//  (d) a model of the boxing round's pattern: 15 exports per period, merged in one wait each;
//  (e) the assembled tree: the cvar (off), the sync points in the command processor (MEM_WRITE,
//      EVENT_WRITE_SHD, COND_WRITE, REG_TO_MEM, the scratch write-back, INTERRUPT, WAIT_REG_MEM on
//      memory, the end of a part of the ring, XE_SWAP), the check before a draw and a resolve, the
//      batched path's place and GPU-only marks, the batch merged before an export read back at
//      once, the 30 s line.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xenia/gpu/memexport_batch.h"
#include "xenia/gpu/memexport_merge.h"

namespace {

namespace mb = xe::gpu::memexport_batch;
namespace mm = xbox360ps5::memexport_merge;
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

void CheckSwitch() {
  bool on = false;
  Expect(mb::ParseSwitch("on\n", on) == 0 && on, "on");
  Expect(mb::ParseSwitch("# batching\noff\r\n", on) == 0 && !on, "off, a comment, CRLF");
  bool kept = true;
  Expect(mb::ParseSwitch("yes\non off\n", kept) == 2 && kept, "lines not understood leave the setting");
}

void CheckBatch() {
  mb::Batch batch;
  Expect(batch.empty() && batch.Reserve(1024, 1000, 1u << 20) == 0, "an empty batch starts at 0");
  mb::Pending first;
  first.base = 0;
  first.before_offset = 1024;
  first.total = 1000;
  first.ranges = {{0x1000, 1000}};
  batch.Add(first);
  const uint32_t second = batch.Reserve(256, 200, 1u << 20);
  Expect(second == 2048, "the next export after the previous one, aligned (" + std::to_string(second) + ")");
  Expect(batch.Reserve(256, (1u << 20) - 2048, 1u << 20) == UINT32_MAX, "an export that does not fit: at once");
  Expect(batch.Reserve(256, mb::kMaxBytes, mb::kMaxBytes * 2) == UINT32_MAX, "one bigger than kMaxBytes: at once");
  mb::Batch fresh;
  Expect(fresh.Reserve(0, mb::kMaxBytes + 4, mb::kMaxBytes * 2) == UINT32_MAX, "also into an empty batch");
  Expect(!batch.AnyInvalid([](uint32_t, uint32_t) { return true; }), "valid ranges: nothing to merge early");
  Expect(batch.AnyInvalid([](uint32_t start, uint32_t) { return start != 0x1000; }),
         "a pending range the CPU wrote is seen");
  batch.Clear();
  Expect(batch.empty() && batch.Reserve(256, 200, 1u << 20) == 0, "after the merge it starts at 0 again");
}

// Two exports deferred, then merged in order. Guest memory: 16 dwords.
void CheckMerge() {
  uint8_t guest[64];
  for (int n = 0; n < 16; ++n) {
    const uint32_t v = 0x1000 + n;
    std::memcpy(guest + 4 * n, &v, 4);
  }
  // The readback buffer: export A (dwords 0-7) at base 0, its before copy at 64; export B
  // (dwords 4-7) at base 256, before at 256 + 64.
  uint8_t buffer[512] = {};
  std::memcpy(buffer + 64, guest, 32);   // A before: the GPU copy = guest
  std::memcpy(buffer + 0, guest, 32);    // A after
  for (int n : {1, 5}) {                 // the shader writes dwords 1 and 5
    const uint32_t v = 0xA000 + n;
    std::memcpy(buffer + 4 * n, &v, 4);
  }
  // B sees A's result in the GPU copy before it.
  std::memcpy(buffer + 256 + 64, buffer + 16, 16);
  std::memcpy(buffer + 256, buffer + 16, 16);
  const uint32_t b5 = 0xB005;
  std::memcpy(buffer + 256 + 4, &b5, 4);  // B writes dword 5 again
  // The CPU writes dwords 2 and 9 between the exports and the merge.
  const uint32_t cpu = 0xC0C0;
  std::memcpy(guest + 8, &cpu, 4);
  std::memcpy(guest + 36, &cpu, 4);
  mb::Batch batch;
  mb::Pending a, b;
  a.base = 0;
  a.before_offset = 64;
  a.total = 32;
  a.ranges = {{0, 32}};
  b.base = 256;
  b.before_offset = 64;
  b.total = 16;
  b.ranges = {{16, 16}};
  batch.Add(a);
  batch.Add(b);
  for (const mb::Pending& p : batch.pending()) {
    const uint8_t* after = buffer + p.base;
    const uint8_t* before = after + p.before_offset;
    for (const mb::Range& range : p.ranges) {
      mm::ApplyExports(guest + range.start, before, after, range.size);
      after += range.size;
      before += range.size;
    }
  }
  const auto word = [&](int n) {
    uint32_t v;
    std::memcpy(&v, guest + 4 * n, 4);
    return v;
  };
  Expect(word(1) == 0xA001, "the first export's dword stored");
  Expect(word(5) == 0xB005, "a dword both exports wrote: the later one's value");
  Expect(word(2) == 0xC0C0 && word(9) == 0xC0C0, "the dwords the CPU wrote meanwhile and the shader did not, kept");
  Expect(word(0) == 0x1000 && word(15) == 0x100F, "the others unchanged");
}

void CheckModel() {
  // The boxing round: per period 15 exporting draws read back, then a sync point.
  mb::Batch batch;
  uint64_t waits_before = 0, waits_after = 0, merged = 0;
  for (int period = 0; period < 100; ++period) {
    for (int draw = 0; draw < 15; ++draw) {
      ++waits_before;  // fix 30: a wait per exporting draw
      const uint32_t base = batch.Reserve(512, 4096, mb::kMaxBytes);
      mb::Pending p;
      p.base = base;
      p.before_offset = 512;
      p.total = 4096;
      p.ranges = {{uint32_t(0x100000 + draw * 4096), 4096}};
      batch.Add(p);
    }
    // The sync point (FlushDeferredReadbacks): one wait, every pending export merged.
    ++waits_after;
    merged += batch.pending().size();
    batch.Clear();
  }
  std::printf("model: %llu exports, waits %llu -> %llu\n", (unsigned long long)merged,
              (unsigned long long)waits_before, (unsigned long long)waits_after);
  Expect(merged == 1500 && waits_after * 15 == waits_before, "15 exports a period merged in one wait each");
}

void CheckTree() {
  const std::string gpu = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string vk = Read(gpu + "vulkan/vulkan_command_processor.cc");
  const std::string pm4 = Read(gpu + "pm4_command_processor_implement.h");
  const std::string cp = Read(gpu + "command_processor.cc"), cph = Read(gpu + "command_processor.h");
  Expect(vk.find("DEFINE_bool(readback_memexport_batching, false,") != std::string::npos, "the cvar, off");
  Expect(cph.find("virtual void FlushDeferredReadbacks(uint32_t reason) {}") != std::string::npos,
         "the hook, empty in the base command processor");
  const char* const handlers[] = {"ExecutePacketType3_MEM_WRITE(", "ExecutePacketType3_COND_WRITE(",
                                  "ExecutePacketType3_REG_TO_MEM(", "ExecutePacketType3_EVENT_WRITE_SHD(",
                                  "ExecutePacketType3_INTERRUPT(", "ExecutePacketType3_XE_SWAP("};
  for (const char* handler : handlers) {
    const size_t fn = pm4.find(std::string("bool COMMAND_PROCESSOR::") + handler);
    const size_t flush = pm4.find("FlushDeferredReadbacks(memexport_batch::", fn);
    const size_t next = pm4.find("\nbool COMMAND_PROCESSOR::", fn + 1);
    Expect(fn != std::string::npos && flush < next, std::string("a sync point at ") + handler);
  }
  const size_t wait = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_WAIT_REG_MEM(");
  Expect(pm4.find("FlushDeferredReadbacks(memexport_batch::kWaitRegMem);", wait) <
             pm4.find("\nbool COMMAND_PROCESSOR::", wait + 1),
         "a sync point at a WAIT_REG_MEM on memory");
  Expect(pm4.find("FlushDeferredReadbacks(memexport_batch::kRingPart);\n  COMMAND_PROCESSOR::OnPrimaryBufferEnd();") !=
             std::string::npos,
         "a sync point at the end of a part of the ring");
  const size_t scratch = cp.find("uint32_t mem_addr = scratch_addr + (scratch_reg * 4);");
  Expect(scratch != std::string::npos && cp.find("FlushDeferredReadbacks(memexport_batch::kCpWrite);", scratch) <
                                             cp.find("xe::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);", scratch),
         "a sync point before a scratch register's write-back");
  const size_t draw = vk.find("bool VulkanCommandProcessor::IssueDrawInner(");
  const size_t copy = vk.find("bool VulkanCommandProcessor::IssueCopy() {");
  Expect(vk.find("FlushMemexportBatch(memexport_batch::kInvalidated);", draw) < vk.find("BeginSubmission(true)", draw) &&
             vk.find("FlushMemexportBatch(memexport_batch::kInvalidated);", copy) != std::string::npos,
         "pending exports the CPU wrote are merged before a draw or a resolve");
  const size_t batched = vk.find("if (memexport_batch_base != UINT32_MAX) {\n          memexport_batch::Pending pending;");
  const size_t classic = vk.find("FlushMemexportBatch(memexport_batch_full ? memexport_batch::kFull");
  const size_t classic_wait = vk.find("        if (AwaitAllQueueOperationsCompletion()) {\n          // Map staging buffer and copy to guest memory.");
  Expect(batched != std::string::npos && batched < classic && classic < classic_wait,
         "batched: pending and no wait; else the batch merged first, then the export's own wait");
  Expect(vk.find("shared_memory_->MarkGpuOnly(memexport_range.base_address_dwords << 2,", batched) < classic &&
             vk.find("shared_memory_->ClearGpuOnly(range.start, range.size);") != std::string::npos,
         "pending units GPU-only until merged");
  Expect(vk.find("XBOX360PS5_DATA_ROOT \"/memexport-batching.txt\"") != std::string::npos &&
             vk.find("\"Memexport batching, last 30 s (fix 136, {}): ") != std::string::npos,
         "the live file and the 30 s line");
}

}  // namespace

int main(int argc, char** argv) {
  CheckSwitch();
  CheckBatch();
  CheckMerge();
  CheckModel();
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-memexport-batching: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-memexport-batching: ok\n");
  return 0;
}
