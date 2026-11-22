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
// Fix 136b: 1.0.5-work-fix136 in the boxing round: 12-16x fewer waits, but the GPU thread's
// draws took 330-386 ms/s against 148-206 and the output fell from about 58 to 50 frames a
// second (minimums 3-11). The merge mapped the whole 32 MiB buffer each time.
//  (f) "max N" (0 to 4096, the rest not understood; on/off unchanged); the bytes used;
//  (g) the tree: the buffer mapped once and only the bytes used invalidated, the check before a
//      draw or a resolve timed, "max N" merged right after the export that reaches it, the
//      parts timed in the 30 s line, the mapping undone before the buffer's memory is freed.
// Fix 136c: Modern Warfare 2 with batching on drew lasting noise over static pillars: a merge,
// late, invalidated pages that a later GPU write (a resolve, an export not read back) had
// written, and they were uploaded again from guest memory, which lacks that write.
//  (h) Overlaps (page granular, either side, empty ranges);
//  (i) a model of the GPU's copy, guest memory, the pages' validity and the uploads: a batched
//      export, then a resolve over the same page, then the merge and a draw reading the page.
//      Without the merge before the resolve (fix 136) the resolve's data is lost for good; with
//      it (136c) the GPU's copy and guest memory end as with each export read back at once.
//      Two batched exports over one page merge in order with the same result; an export that
//      does not fit (buffer full) is read back after the batch, in order;
//  (j) the tree: the hook set at the shared memory's creation and called at the start of
//      RangeWrittenByGpu, a batched export's own write exempt, the "GPU write" reason.
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

// Fix 136b.
void CheckSettings() {
  bool on = false;
  uint32_t max_pending = 7;
  Expect(mb::ParseSettings("on\nmax 4\n", on, max_pending) == 0 && on && max_pending == 4, "on, max 4");
  Expect(mb::ParseSettings("max 0 # no limit\r\n", on, max_pending) == 0 && max_pending == 0, "max 0: no limit");
  Expect(mb::ParseSettings("max 4097\nmax\nmax 2 3\nmax -1\n", on, max_pending) == 4 && max_pending == 0,
         "out of range or malformed max lines are not understood and change nothing");
  Expect(mb::ParseSwitch("off\nmax 3\n", on) == 0 && !on, "ParseSwitch still reads the file with a max line");
  mb::Batch batch;
  Expect(batch.used() == 0, "an empty batch uses nothing");
  mb::Pending p;
  p.base = batch.Reserve(1000, 1000, 1u << 20);
  p.before_offset = 1000;
  p.total = 1000;
  batch.Add(p);
  Expect(batch.used() == 2048 && batch.used() % mb::kAlign == 0,
         "the bytes used, aligned (" + std::to_string(batch.used()) + ")");
  Expect(std::string(mb::ReasonName(mb::kCap)) == "at most N" && std::string(mb::ReasonName(mb::kOther)) == "other",
         "the new reason's name, the others kept");
}

// Fix 136c: Overlaps.
void CheckOverlaps() {
  mb::Batch batch;
  Expect(!batch.Overlaps(0, 4096, 12), "an empty batch overlaps nothing");
  mb::Pending p;
  p.ranges = {{0x2010, 16}, {0x9000, 0}};
  batch.Add(p);
  Expect(batch.Overlaps(0x2FF0, 4, 12), "the same 4 KiB page, other bytes: overlap");
  Expect(batch.Overlaps(0x1000, 0x1001, 12), "a range ending in the page: overlap");
  Expect(!batch.Overlaps(0x1000, 0x1000, 12) && !batch.Overlaps(0x3000, 64, 12), "the pages next to it: none");
  Expect(!batch.Overlaps(0x9000, 64, 12), "an empty pending range overlaps nothing");
  Expect(!batch.Overlaps(0x2010, 0, 12), "an empty write overlaps nothing");
}

// Fix 136c: a model of shared memory's pages. 2 pages of 4 dwords (16 bytes, page_log2 4).
struct PageModel {
  static constexpr int kDwords = 8;
  uint32_t gpu[kDwords] = {}, guest[kDwords] = {};
  bool valid[2] = {true, true};
  // The GPU writes dwords [first, first + count) (RangeWrittenByGpu: valid afterwards).
  void GpuWrite(int first, int count, uint32_t value) {
    for (int n = first; n < first + count; ++n) gpu[n] = value + n;
    for (int page = first / 4; page <= (first + count - 1) / 4; ++page) valid[page] = true;
  }
  // A merge (or any guest write by the emulator): guest memory written, its pages invalidated (the
  // write watch).
  void GuestWrite(int dword, uint32_t value) {
    guest[dword] = value;
    valid[dword / 4] = false;
  }
  // A draw reading the pages: an invalid page is uploaded from guest memory.
  void Read() {
    for (int page = 0; page < 2; ++page) {
      if (!valid[page]) {
        for (int n = page * 4; n < page * 4 + 4; ++n) gpu[n] = guest[n];
        valid[page] = true;
      }
    }
  }
};
struct ModelExport {
  int first, count;
  uint32_t value;
  uint32_t before[PageModel::kDwords], after[PageModel::kDwords];
};
// The export's draw: the copies before and after (the batch buffer), the GPU's write.
ModelExport Export(PageModel& m, int first, int count, uint32_t value) {
  ModelExport e{first, count, value, {}, {}};
  std::memcpy(e.before, m.gpu, sizeof(m.gpu));
  m.GpuWrite(first, count, value);
  std::memcpy(e.after, m.gpu, sizeof(m.gpu));
  return e;
}
void Merge(PageModel& m, const ModelExport& e) {
  for (int n = e.first; n < e.first + e.count; ++n)
    if (e.after[n] != e.before[n]) m.GuestWrite(n, e.after[n]);  // fix 30: the dwords written
}

void CheckGpuWriteOrder() {
  // The reference: each export read back at once (fix 30).
  PageModel ref;
  Merge(ref, Export(ref, 0, 2, 0xE00));
  ref.Read();
  ref.GpuWrite(2, 2, 0x500);  // a resolve over the same page, not read back
  ref.Read();
  const uint32_t want_gpu_2 = ref.gpu[2], want_gpu_0 = ref.gpu[0];
  // Fix 136: the export batched, the resolve, then the merge at the next sync point.
  PageModel late;
  mb::Batch batch;
  ModelExport e = Export(late, 0, 2, 0xE00);
  mb::Pending pending;
  pending.ranges = {{0, 8}};
  batch.Add(pending);
  late.GpuWrite(2, 2, 0x500);
  Merge(late, e);
  batch.Clear();
  late.Read();
  Expect(late.gpu[2] != want_gpu_2, "fix 136's order loses the resolve's data (the model reproduces it)");
  // Fix 136c: the resolve's write overlaps the pending export's page: the batch merges first.
  PageModel fixed;
  ModelExport f = Export(fixed, 0, 2, 0xE00);
  batch.Add(pending);
  bool merged = false;
  if (batch.Overlaps(8, 8, 4)) {  // the resolve: dwords 2-3, bytes 8-15, page 0
    Merge(fixed, f);
    batch.Clear();
    merged = true;
  }
  fixed.GpuWrite(2, 2, 0x500);
  fixed.Read();
  Expect(merged && fixed.gpu[2] == want_gpu_2 && fixed.gpu[0] == want_gpu_0 &&
             std::memcmp(fixed.guest, ref.guest, sizeof(ref.guest)) == 0,
         "fix 136c: merged before the resolve, the GPU's copy and guest memory as read back at once");
  // A write over the other page only: no merge needed.
  batch.Add(pending);
  Expect(!batch.Overlaps(16, 16, 4), "a GPU write over another page merges nothing");
  batch.Clear();
  // Two batched exports over one page, merged in order at the sync point.
  PageModel two_ref;
  Merge(two_ref, Export(two_ref, 0, 3, 0xA00));
  Merge(two_ref, Export(two_ref, 1, 3, 0xB00));
  two_ref.Read();
  PageModel two;
  ModelExport a = Export(two, 0, 3, 0xA00);
  ModelExport b = Export(two, 1, 3, 0xB00);
  Merge(two, a);
  Merge(two, b);
  two.Read();
  Expect(std::memcmp(two.gpu, two_ref.gpu, sizeof(two.gpu)) == 0 &&
             std::memcmp(two.guest, two_ref.guest, sizeof(two.guest)) == 0,
         "two batched exports over one page: the same as each read back at once");
  // Buffer full: the export that does not fit is read back at once, after the batch's merge.
  mb::Batch full;
  uint32_t fitted = 0;
  while (full.Reserve(1u << 20, 1u << 20, mb::kMaxBytes) != UINT32_MAX) {
    mb::Pending q;
    q.base = full.Reserve(1u << 20, 1u << 20, mb::kMaxBytes);
    q.before_offset = 1u << 20;
    q.total = 1u << 20;
    full.Add(q);
    ++fitted;
  }
  bool full_flag = full.Reserve(1u << 20, 1u << 20, mb::kMaxBytes) == UINT32_MAX && !full.empty();
  Expect(fitted == 16 && full_flag, "16 exports of 1 MiB fit in 32 MiB, the next is read back at once (" +
                                       std::to_string(fitted) + ")");
  PageModel order_ref, order;
  Merge(order_ref, Export(order_ref, 0, 2, 0xC00));
  Merge(order_ref, Export(order_ref, 1, 2, 0xD00));
  ModelExport c = Export(order, 0, 2, 0xC00);   // batched
  ModelExport d = Export(order, 1, 2, 0xD00);   // full: at once, after the batch
  Merge(order, c);                              // FlushMemexportBatch(kFull)
  Merge(order, d);
  Expect(std::memcmp(order.guest, order_ref.guest, sizeof(order.guest)) == 0,
         "buffer full: the batch merged before the export read back at once");
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
  const size_t if_invalid = vk.find("void VulkanCommandProcessor::FlushMemexportBatchIfInvalid() {");
  Expect(vk.find("FlushMemexportBatchIfInvalid();  // fix 136b: timed", draw) < vk.find("BeginSubmission(true)", draw) &&
             vk.find("FlushMemexportBatchIfInvalid();  // fix 136b: timed", copy) != std::string::npos &&
             vk.find("FlushMemexportBatch(memexport_batch::kInvalidated);", if_invalid) != std::string::npos,
         "pending exports the CPU wrote are merged before a draw or a resolve");
  // Fix 136c.
  const std::string sm = Read(gpu + "shared_memory.cc"), smh = Read(gpu + "shared_memory.h");
  const size_t written = sm.find("void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {");
  Expect(written != std::string::npos &&
             sm.find("before_gpu_write_(start, length, page_size_log2_);  // fix 136c", written) <
                 sm.find("FireWatches(page_first, page_last, true);", written) &&
             smh.find("void SetBeforeGpuWrite(") != std::string::npos,
         "fix 136c: the hook runs at the start of RangeWrittenByGpu, before the write is noted");
  Expect(vk.find("shared_memory_->SetBeforeGpuWrite([this](uint32_t start, uint32_t length, uint32_t page_log2) {") >
                 vk.find("if (!shared_memory_->Initialize()) {"),
         "fix 136c: the hook set when the shared memory is made");
  const size_t own = vk.find("memexport_batch_own_write_ = memexport_batch_base != UINT32_MAX;");
  const size_t own_write = vk.find("shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,", own);
  const size_t own_end = vk.find("memexport_batch_own_write_ = false;  // fix 136c", own_write);
  Expect(own != std::string::npos && own_write != std::string::npos && own_end != std::string::npos &&
             own_end < vk.find("// CPU readback for memexport data", own_write),
         "fix 136c: a batched export's own write exempt, only around it");
  const size_t overlaps = vk.find("void VulkanCommandProcessor::FlushMemexportBatchIfOverlaps(");
  Expect(overlaps != std::string::npos &&
             vk.find("!memexport_batch_.Overlaps(start, length, page_log2)", overlaps) <
                 vk.find("FlushMemexportBatch(memexport_batch::kGpuWrite);", overlaps) &&
             std::string(mb::ReasonName(mb::kGpuWrite)) == "GPU write",
         "fix 136c: another GPU write over a pending export's pages merges the batch first");
  // Fix 136b.
  const size_t map = vk.find("void* VulkanCommandProcessor::MapMemexportBatch(uint32_t used) {");
  Expect(map != std::string::npos && vk.find("if (!memexport_batch_mapped_ &&", map) != std::string::npos &&
             vk.find("range.size = (VkDeviceSize(used) + atom - 1) / atom * atom;", map) != std::string::npos &&
             vk.find("MapReadbackMemory(memexport_batch_memory_") == std::string::npos &&
             vk.find("void* const mapped_data = MapMemexportBatch(memexport_batch_.used());") != std::string::npos,
         "fix 136b: the buffer mapped once, only the bytes used invalidated");
  const size_t unmap = vk.find("    dfn.vkUnmapMemory(device, memexport_batch_memory_);\n    memexport_batch_mapped_ = nullptr;");
  Expect(unmap != std::string::npos &&
             unmap < vk.find("DestroyAndNullHandle(dfn.vkFreeMemory, device, memexport_batch_memory_);"),
         "fix 136b: unmapped before its memory is freed");
  const size_t added = vk.find("memexport_batch_.Add(std::move(pending));");
  Expect(vk.find("FlushMemexportBatch(memexport_batch::kCap);", added) < vk.find("return true;", added) &&
             vk.find("memexport_batch_.pending().size() >= memexport_batching_max_", added) != std::string::npos,
         "fix 136b: max N merged right after the export that reaches it");
  Expect(vk.find("ParseSettings(text, on, max_pending);") != std::string::npos &&
             vk.find("checks before draws {:.1f} ms, batching {:.1f} ms, merges {:.1f} ms (longest {:.2f} ") !=
                 std::string::npos,
         "fix 136b: the settings and the parts timed in the 30 s line");
  const size_t batched = vk.find(
      "if (memexport_batch_base != UINT32_MAX) {\n          const auto batch_start = std::chrono::steady_clock::now();  // fix 136b\n"
      "          memexport_batch::Pending pending;");
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
  CheckSettings();
  CheckOverlaps();
  CheckGpuWriteOrder();
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
