// SPDX-License-Identifier: MIT
// Fix 177 (docs/design/memexport-readback.md, D1): memory export readbacks queued and merged at the
// next point where the CPU could learn that the GPU went on; off unless memexport-deliver.txt says
// "on" ("verify" keeps today's immediate merge and counts what the delayed one would keep).
//  The real header (xenia/gpu/memexport_deliver.h):
//  (a) the switch file's first word;
//  (b) the delayed merge stores what the shader wrote only where guest memory still holds the
//      value from before the draw: a CPU write made since the draw is kept (today's ApplyExports
//      overwrites it), also with a CPU thread writing during the merge;
//  (c) overlaps, size classes, the verify records, the line.
//  The assembled tree:
//  (d) a sync point before every packet that lets the CPU learn the GPU went on (fix 133's kinds),
//      an interrupt, the ring's read pointer, a scratch write-back before its store, a swap, the
//      idle wait, and WAIT_REG_MEM only when the GPU is about to wait;
//  (e) with "on" a readback draw's copies go to its own buffer and are queued instead of the wait;
//      without it the code is today's; "verify" records the immediate merge's stores;
//  (f) queued merges go first before fix 30's refresh and before an upload over their ranges;
//  (g) delivery in draw order with the keeping merge; no wait from inside EndSubmission's start;
//      the queue's buffers destroyed at shutdown.
// Runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if __has_include("xenia/gpu/memexport_deliver.h")
#include "xenia/gpu/memexport_deliver.h"
#include "xenia/gpu/memexport_merge.h"
#define HAVE_DELIVER 1
#endif

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
bool Has(const std::string& text, const std::string& part) {
  const bool found = text.find(part) != std::string::npos;
  if (!found) std::printf("  (not found: %s)\n", part.substr(0, 100).c_str());
  return found;
}
std::string Body(const std::string& text, const std::string& signature) {
  const size_t at = text.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t end = text.find("\n}\n", at);
  return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

#ifdef HAVE_DELIVER
namespace md = xbox360ps5::memexport_deliver;

void Put(std::vector<uint8_t>& bytes, size_t dword, uint32_t value) {
  std::memcpy(bytes.data() + dword * 4, &value, 4);
}
uint32_t Get(const std::vector<uint8_t>& bytes, size_t dword) {
  uint32_t value;
  std::memcpy(&value, bytes.data() + dword * 4, 4);
  return value;
}

void CheckMode() {
  // (a)
  Expect(md::ParseMode("on\n", md::Mode::kOff) == md::Mode::kOn &&
             md::ParseMode("  verify # check\n", md::Mode::kOff) == md::Mode::kVerify &&
             md::ParseMode("0", md::Mode::kOn) == md::Mode::kOff &&
             md::ParseMode("", md::Mode::kOff) == md::Mode::kOff &&
             md::ParseMode("maybe", md::Mode::kVerify) == md::Mode::kVerify,
         "(a) on, verify, off; nothing or nonsense keeps the default");
}

void CheckMerge() {
  // (b) Before the draw the range held 0..15; the shader wrote dwords 2, 5 and 9; since the draw
  // the CPU changed dword 5 (to 0xC0FFEE) and dword 7 (one the shader did not write).
  std::vector<uint8_t> before(64), after(64), guest(64);
  for (size_t i = 0; i < 16; ++i) {
    Put(before, i, uint32_t(i));
    Put(after, i, uint32_t(i));
    Put(guest, i, uint32_t(i));
  }
  Put(after, 2, 0xA2);
  Put(after, 5, 0xA5);
  Put(after, 9, 0xA9);
  Put(guest, 5, 0xC0FFEE);
  Put(guest, 7, 0x77);
  std::vector<uint8_t> today = guest;
  const md::Merged merged = md::MergeKeepingCpu(guest.data(), before.data(), after.data(), 64);
  Expect(Get(guest, 2) == 0xA2 && Get(guest, 9) == 0xA9 && Get(guest, 5) == 0xC0FFEE &&
             Get(guest, 7) == 0x77 && merged.written == 2 && merged.kept == 1,
         "(b) the delayed merge stores the shader's dwords and keeps the CPU's write since the "
         "draw");
  const auto immediate = xbox360ps5::memexport_merge::ApplyExports(today.data(), before.data(),
                                                                   after.data(), 64);
  Expect(Get(today, 5) == 0xA5 && immediate.differed == 1,
         "(b) today's immediate merge overwrites that CPU write (why the delayed one cannot use "
         "it)");
  // A tail shorter than a dword, byte by byte.
  std::vector<uint8_t> b6 = {1, 2, 3, 4, 5, 6}, a6 = {1, 2, 3, 4, 9, 6}, g6 = b6;
  Expect(md::MergeKeepingCpu(g6.data(), b6.data(), a6.data(), 6).written == 1 && g6[4] == 9,
         "(b) a tail byte is merged");
  // A CPU thread writing during the merge: its dwords end with its value, the others the GPU's.
  constexpr size_t kDwords = 1 << 16;
  std::vector<uint8_t> big_before(kDwords * 4), big_after(kDwords * 4);
  alignas(64) static uint32_t big_guest[kDwords];
  for (size_t i = 0; i < kDwords; ++i) {
    Put(big_before, i, uint32_t(i));
    Put(big_after, i, uint32_t(i) ^ 0x80000000u);  // the shader wrote every dword
    big_guest[i] = uint32_t(i);
  }
  std::atomic<bool> go{false};
  std::thread cpu([&] {
    while (!go.load()) {
    }
    for (size_t i = 1; i < kDwords; i += 2) {
      __atomic_store_n(&big_guest[i], 0xC0DE0000u | uint32_t(i & 0xFFFF), __ATOMIC_SEQ_CST);
    }
  });
  go = true;
  const md::Merged concurrent = md::MergeKeepingCpu(
      reinterpret_cast<uint8_t*>(big_guest), big_before.data(), big_after.data(), kDwords * 4);
  cpu.join();
  bool cpu_won = true, gpu_stored = true;
  for (size_t i = 0; i < kDwords; ++i) {
    if (i & 1) {
      cpu_won &= big_guest[i] == (0xC0DE0000u | uint32_t(i & 0xFFFF));
    } else {
      gpu_stored &= big_guest[i] == (uint32_t(i) ^ 0x80000000u);
    }
  }
  Expect(cpu_won && gpu_stored && concurrent.written + concurrent.kept == kDwords,
         "(b) with a CPU thread writing during the merge, every CPU write wins and every other "
         "dword gets the GPU's value");
}

void CheckParts() {
  // (c)
  std::vector<md::Range> ranges = {{0x1000, 0x100}, {0x8000, 0x60000}};
  Expect(md::AnyOverlap(ranges, 0x10F0, 0x20) && !md::AnyOverlap(ranges, 0x1100, 0x100) &&
             md::AnyOverlap(ranges, 0x67FFF, 1) && !md::AnyOverlap(ranges, 0x68000, 0x1000),
         "(c) overlaps, ends exclusive");
  Expect(md::SizeClass(1) == 65536 && md::SizeClass(65536) == 65536 &&
             md::SizeClass(65537) == 131072 && md::SizeClass(393216 * 2) == 1048576,
         "(c) size classes: powers of two from 64 KiB");
  std::vector<uint8_t> before(16, 0), after(16, 0);
  after[4] = 1;
  after[12] = 2;
  std::vector<md::Stored> stored;
  md::RecordStores(0x2000, before.data(), after.data(), 16, stored);
  uint32_t raw4, raw12;
  std::memcpy(&raw4, after.data() + 4, 4);
  std::memcpy(&raw12, after.data() + 12, 4);
  const uint64_t changed = md::CountChanged(stored, [&](uint32_t address) {
    return address == 0x2004 ? raw4 : 0xFFFFFFFFu;  // the CPU changed 0x200C since
  });
  Expect(stored.size() == 2 && stored[0].address == 0x2004 && stored[1].address == 0x200C &&
             changed == 1,
         "(c) verify: the stored dwords, and the ones the CPU changed since");
  md::Stats s;
  s.queued = 10;
  s.waits = 3;
  s.waits_by[md::kWaitRegMem] = 2;
  s.waits_by[md::kSwap] = 1;
  s.kept = 4;
  const std::string line = md::Line(md::Mode::kOn, s, 12.5);
  Expect(Has(line, "Memexport delivery, last 30 s (fix 177, on): 10 readbacks queued") &&
             Has(line, "3 waits (12.5 ms/s): WAIT_REG_MEM 2, swaps 1") &&
             Has(line, "kept 4 (the CPU wrote them since the draw)"),
         "(c) the line");
}
#endif

void CheckTree() {
  const std::string g = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string cp = Read(g + "vulkan/vulkan_command_processor.cc");
  const std::string base = Read(g + "command_processor.cc");
  const std::string pm4 = Read(g + "pm4_command_processor_implement.h");
  const std::string shared = Read(g + "shared_memory.cc");
  Expect(!cp.empty() && !base.empty() && !pm4.empty() && !shared.empty(),
         "the tree is there (run from the assembled tree's root)");
  // (d)
  const char* const kinds[] = {"Kind::kRegToMem, mem_addr,", "Kind::kMemWrite, addr,",
                               "Kind::kCondWrite,", "Kind::kEventShd, address,",
                               "Global().Count(xbox360ps5::cp_trace::Kind::kEventExt);"};
  bool synced = true;
  for (const char* kind : kinds) {
    const size_t at = pm4.find(kind);
    const size_t sync = pm4.find("GuestSyncPoint(0);  // fix 177", at);
    if (at == std::string::npos || sync == std::string::npos || sync - at > 300) {
      std::printf("  (no sync point right after %s)\n", kind);
      synced = false;
    }
  }
  Expect(synced, "(d) a sync point at each packet that lets the CPU learn the GPU went on");
  Expect(Has(pm4, "if (cpu_mask) GuestSyncPoint(2);") &&
             Has(pm4, "      frame_cost_wait.Missed();  // fix 77\n      // Fix 177: the GPU about to wait") &&
             Has(base, "GuestSyncPoint(3);  // fix 177: the ring's read pointer") &&
             base.find("GuestSyncPoint(0);  // fix 177: a scratch write-back") <
                 base.find("xe::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);") &&
             Has(base, "GuestSyncPoint(5);  // fix 177: out of commands") &&
             Has(cp, "GuestSyncPoint(xbox360ps5::memexport_deliver::kSwap);  // fix 177"),
         "(d) interrupts, WAIT_REG_MEM only when it waits, the read pointer, the scratch write-back "
         "before its store, the idle wait, the swap");
  // (e)
  Expect(Has(cp, "if (memexport_readback && fix177::mode == xbox360ps5::memexport_deliver::Mode::kOn) {") &&
             Has(cp, "fix177_queue ? fix177_entry.buffer") &&
             cp.find("        if (fix177_queue) {") < cp.find("        // Wait for GPU to finish (SYNCHRONIZATION STALL)") &&
             Has(cp, "fix177::pending.push_back(std::move(fix177_entry));") &&
             Has(cp, "        }  // fix 177"),
         "(e) with on, the draw's own buffer and the queue instead of the wait; otherwise today's");
  Expect(cp.find("xbox360ps5::memexport_deliver::RecordStores(") > cp.find("const auto merged = mm::ApplyExports("),
         "(e) verify records the immediate merge's stores");
  // (f)
  const size_t refresh_first = cp.find("if (fix177_overlap) Fix177Deliver(true, xbox360ps5::memexport_deliver::kRefresh);");
  Expect(refresh_first != std::string::npos &&
             refresh_first < cp.find("xbox360ps5::memexport_merge::RefreshRanges(*shared_memory_, ranges);"),
         "(f) queued merges into a range before fix 30's refresh of it");
  Expect(Has(shared, "before_upload_hook_(before_upload_context_, uploads, current_upload_range, page_size_log2_);") &&
             shared.find("before_upload_hook_(") < shared.find("bool usable = !current_upload_range || UploadRanges(") &&
             Has(cp, "shared_memory_->SetBeforeUploadHook(&VulkanCommandProcessor::Fix177BeforeUpload,"),
         "(f) queued merges before an upload over their ranges");
  // (g)
  const std::string deliver = Body(cp, "void VulkanCommandProcessor::Fix177Deliver(bool wait, uint32_t point) {");
  Expect(Has(deliver, "while (!fix177::pending.empty() && fix177::pending.front().submission <= completed) {") &&
             Has(deliver, "md::MergeKeepingCpu(memory_->TranslatePhysical(range.start),") &&
             Has(deliver, "if (fix177::pending.empty() || fix177::delivering) return;"),
         "(g) delivery in draw order with the keeping merge, never nested");
  const std::string refresh = Body(cp, "void VulkanCommandProcessor::Fix177Refresh() {");
  Expect(!refresh.empty() && refresh.find("Fix177Deliver(") == std::string::npos &&
             Has(cp, "if (!fix177::delivering && !fix177::pending.empty()) Fix177Deliver(false, 0);") &&
             Has(cp, "XBOX360PS5_DATA_ROOT \"/memexport-deliver.txt\"") &&
             Has(cp, "md::Mode next = md::Mode::kOff;"),
         "(g) off by default; no wait from the start of EndSubmission (only made copies, no wait)");
  Expect(cp.find("Fix177DestroyBuffers();  // fix 177") > cp.find("void VulkanCommandProcessor::ShutdownContext() {"),
         "(g) the queue's buffers destroyed at shutdown");
}

}  // namespace

int main() {
#ifdef HAVE_DELIVER
  CheckMode();
  CheckMerge();
  CheckParts();
#else
  std::printf("FAIL: xenia/gpu/memexport_deliver.h is not in the tree (fix 177)\n");
  ++failures;
#endif
  CheckTree();
  if (failures) {
    std::printf("check-memexport-deliver: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-memexport-deliver: ok\n");
  return 0;
}
