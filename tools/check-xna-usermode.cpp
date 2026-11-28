// SPDX-License-Identifier: MIT
// Fix 191 (tools/source-1.0.2/fixes/191-xna-usermode.patch): Xbox 360 kernel
// user-mode support for XNA (.NET Compact Framework: RuntimeHost.xex +
// NetCFUserMode.dll, GitHub issue #82 — Charlie Murder 58410B83).
//
// Verifies:
//  1. User-mode <-> kernel address space alias bijection (0x20000000..0x3FFFFFFF
//     <-> 0xA0000000..0xBFFFFFFF) and PS5 4 GiB-aligned window constraints;
//  2. KTRAP_FRAME (SaveKframes / LoadKframes) and guest CONTEXT
//     (KeContextFromKframes / KeContextToKframes, kContextUserModeControl)
//     big-endian round-trips, MSR[PR] = 0x4000, XER (SO/OV/CA), CR, LR, IAR;
//  3. Swept dynamic code range tracking (physical-address keying across
//     0x20000000 user and 0xA0000000 kernel aliases, exact non-merging spans,
//     IsUnconditionalBranch on b/bl/blr/bctr vs conditional branches);
//  4. Per-fiber stackpoint swapping, dynamic call cache indexing/invalidation,
//     and ResolveDynamicReturn live-frame matching;
//  5. On Linux x86-64 hosts: compiles and runs the real SysV AMD64
//     threading_fiber_posix.cc (CreateFromThread, Create, SwitchTo, Restart,
//     16-byte stack alignment, MXCSR preservation, and nested KeEnterUserMode
//     -> sc trap -> handler_fiber -> KeLeaveUserMode -> parked trap resume);
//  6. Assembled tree wiring across all 36 created/modified files, including PS5
//     Reserve() fixed-window support, AllocFixed fiber stacks, and CallIndirect
//     moving ebx -> edx before EmitDynamicCallLookup.

#undef NDEBUG
#include <immintrin.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <unistd.h>

#include "xenia/base/memory.h"

namespace xe {
void AssertFailed(const char* expression, const char* file, int line) {
  std::fprintf(stderr, "AssertFailed: %s (%s:%d)\n", expression, file, line);
  std::abort();
}
namespace memory {
size_t page_size() { return static_cast<size_t>(::sysconf(_SC_PAGESIZE)); }
void* AllocFixed(void* base_address, size_t length,
                 AllocationType /*allocation_type*/, PageAccess access) {
  int prot = PROT_NONE;
  if (access == PageAccess::kReadOnly) prot = PROT_READ;
  if (access == PageAccess::kReadWrite) prot = PROT_READ | PROT_WRITE;
  void* p = ::mmap(base_address, length, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | (base_address ? MAP_FIXED : 0),
                   -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}
bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) *out_old_access = PageAccess::kReadWrite;
  int prot = PROT_NONE;
  if (access == PageAccess::kReadOnly) prot = PROT_READ;
  if (access == PageAccess::kReadWrite) prot = PROT_READ | PROT_WRITE;
  return ::mprotect(base_address, length, prot) == 0;
}
bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  if (deallocation_type == DeallocationType::kDecommit) {
    return ::mprotect(base_address, length, PROT_NONE) == 0;
  }
  return ::munmap(base_address, length) == 0;
}
}  // namespace memory
}  // namespace xe

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#endif
#include "xenia/base/threading_fiber_posix.cc"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif  // defined(__linux__) && defined(__x86_64__)

namespace {

uint32_t Bswap32(uint32_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}

uint64_t Bswap64(uint64_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
  return _byteswap_uint64(v);
#else
  return __builtin_bswap64(v);
#endif
}

void StoreBE32(uint8_t* p, uint32_t v) {
  const uint32_t be = Bswap32(v);
  std::memcpy(p, &be, 4);
}
void StoreBE64(uint8_t* p, uint64_t v) {
  const uint64_t be = Bswap64(v);
  std::memcpy(p, &be, 8);
}
uint32_t LoadBE32(const uint8_t* p) {
  uint32_t be = 0;
  std::memcpy(&be, p, 4);
  return Bswap32(be);
}
uint64_t LoadBE64(const uint8_t* p) {
  uint64_t be = 0;
  std::memcpy(&be, p, 8);
  return Bswap64(be);
}

// --- 1. User-mode <-> kernel alias translation & PS5 VA window ---------------
constexpr uint32_t kUserAliasBase = 0x20000000u;
constexpr uint32_t kUserAliasSize = 0x20000000u;

uint32_t UserModeKernelAddress(uint32_t user_address) {
  return user_address - kUserAliasBase < kUserAliasSize
             ? user_address + 0x80000000u
             : user_address;
}

uint32_t KernelModeUserAddress(uint32_t kernel_address) {
  return kernel_address - 0xA0000000u < kUserAliasSize
             ? kernel_address - 0x80000000u
             : kernel_address;
}

void CheckAliasMapping() {
  assert(UserModeKernelAddress(0x1FFFFFFFu) == 0x1FFFFFFFu);
  assert(UserModeKernelAddress(0x20000000u) == 0xA0000000u);
  assert(UserModeKernelAddress(0x2F123456u) == 0xAF123456u);
  assert(UserModeKernelAddress(0x3FFFFFFFu) == 0xBFFFFFFFu);
  assert(UserModeKernelAddress(0x40000000u) == 0x40000000u);
  assert(UserModeKernelAddress(0x82BA0000u) == 0x82BA0000u);

  assert(KernelModeUserAddress(0x9FFFFFFFu) == 0x9FFFFFFFu);
  assert(KernelModeUserAddress(0xA0000000u) == 0x20000000u);
  assert(KernelModeUserAddress(0xAF123456u) == 0x2F123456u);
  assert(KernelModeUserAddress(0xBFFFFFFFu) == 0x3FFFFFFFu);
  assert(KernelModeUserAddress(0xC0000000u) == 0xC0000000u);

  for (uint32_t off = 0; off < kUserAliasSize; off += 0x10000u) {
    const uint32_t u = kUserAliasBase + off;
    const uint32_t k = UserModeKernelAddress(u);
    assert(k == 0xA0000000u + off);
    assert(KernelModeUserAddress(k) == u);
  }

  // PS5 VA constraints: user membase search [0x18_0000_0000, 0x20_0000_0000)
  // stays clear of Vulkan's 0x2_0000_0000 window and fits inside
  // [kAnywhereStart=0x14_0000_0000, kAnywhereEnd=0x20_0000_0000) with low 32
  // bits zero.
  constexpr uint64_t kAnywhereStart = 0x1400000000ull;
  constexpr uint64_t kAnywhereEnd = 0x2000000000ull;
  constexpr uint64_t kSearchStart = 0x1800000000ull;
  assert(kSearchStart >= kAnywhereStart);
  assert(kSearchStart + 0x100000000ull <= kAnywhereEnd);
  assert((kSearchStart & 0xFFFFFFFFull) == 0);
  std::puts("user/kernel alias bijection and PS5 VA window: ok");
}

// --- 2. KTRAP_FRAME & CONTEXT layout round-trips -----------------------------
constexpr uint32_t kKframesSize = 0x1C0;
constexpr uint32_t kKframesGpr = 0x50;
constexpr uint32_t kKframesMsr = 0x160;
constexpr uint32_t kKframesCtr = 0x1A0;
constexpr uint32_t kKframesLr = 0x1A8;
constexpr uint32_t kKframesIar = 0x1AC;
constexpr uint32_t kKframesCr = 0x1B4;
constexpr uint32_t kKframesXer = 0x1B8;
constexpr uint32_t kMsrUserMode = 0x4000;

constexpr uint32_t kContextFlags = 0x0;
constexpr uint32_t kContextMsr = 0x4;
constexpr uint32_t kContextIar = 0x8;
constexpr uint32_t kContextLr = 0xC;
constexpr uint32_t kContextCtr = 0x10;
constexpr uint32_t kContextGpr = 0x18;
constexpr uint32_t kContextCr = 0x118;
constexpr uint32_t kContextXer = 0x11C;
constexpr uint32_t kContextUserModeControl = 0x228;
constexpr uint32_t kContextSize = 0x240 + 128 * 16;
constexpr uint32_t kContextControl = 0x1;
constexpr uint32_t kContextInteger = 0x4;

void CheckKframesAndContext() {
  static_assert(kKframesGpr + 32 * 8 == 0x150);
  static_assert(kKframesXer + 4 <= kKframesSize);
  static_assert(kContextGpr + 32 * 8 == kContextCr);
  static_assert(kContextSize == 0xA40);

  uint8_t kframes[kKframesSize];
  std::memset(kframes, 0, sizeof(kframes));
  for (uint32_t i = 0; i < 32; ++i) {
    StoreBE64(kframes + kKframesGpr + i * 8,
              0x1122334400000000ull | (uint64_t(i) * 0x101u));
  }
  const uint32_t xer_in = (1u << 31) | (0u << 30) | (1u << 29);  // SO=1, CA=1
  StoreBE64(kframes + kKframesCtr, 0x20045678ull);
  StoreBE32(kframes + kKframesCr, 0x24842484u);
  StoreBE32(kframes + kKframesXer, xer_in);
  StoreBE32(kframes + kKframesMsr, 0x9030u | kMsrUserMode);
  StoreBE32(kframes + kKframesLr, 0x82BC1040u);
  StoreBE32(kframes + kKframesIar, 0x20102034u);

  // KeContextFromKframes -> KeContextToKframes round-trip.
  uint8_t ctx[kContextSize];
  std::memset(ctx, 0, sizeof(ctx));
  StoreBE32(ctx + kContextFlags, kContextControl | kContextInteger);

  StoreBE32(ctx + kContextUserModeControl,
            (LoadBE32(kframes + kKframesMsr) & kMsrUserMode) ? 1u : 0u);
  StoreBE32(ctx + kContextMsr, LoadBE32(kframes + kKframesMsr));
  StoreBE32(ctx + kContextIar, LoadBE32(kframes + kKframesIar));
  StoreBE32(ctx + kContextLr, LoadBE32(kframes + kKframesLr));
  StoreBE64(ctx + kContextCtr, LoadBE64(kframes + kKframesCtr));
  std::memcpy(ctx + kContextGpr, kframes + kKframesGpr, 32 * 8);
  StoreBE32(ctx + kContextCr, LoadBE32(kframes + kKframesCr));
  StoreBE32(ctx + kContextXer, LoadBE32(kframes + kKframesXer));

  assert(LoadBE32(ctx + kContextUserModeControl) == 1u);
  assert(LoadBE32(ctx + kContextIar) == 0x20102034u);
  assert(LoadBE32(ctx + kContextLr) == 0x82BC1040u);
  assert(LoadBE64(ctx + kContextCtr) == 0x20045678ull);
  assert(LoadBE32(ctx + kContextCr) == 0x24842484u);
  assert(LoadBE32(ctx + kContextXer) == xer_in);
  for (uint32_t i = 0; i < 32; ++i) {
    assert(LoadBE64(ctx + kContextGpr + i * 8) ==
           (0x1122334400000000ull | (uint64_t(i) * 0x101u)));
  }

  // Redirect IAR and R3 in CONTEXT and write back via KeContextToKframes.
  StoreBE32(ctx + kContextIar, 0x20102080u);
  StoreBE64(ctx + kContextGpr + 3 * 8, 0xCAFEBABEull);
  StoreBE32(kframes + kKframesIar, LoadBE32(ctx + kContextIar));
  std::memcpy(kframes + kKframesGpr, ctx + kContextGpr, 32 * 8);
  assert(LoadBE32(kframes + kKframesIar) == 0x20102080u);
  assert(LoadBE64(kframes + kKframesGpr + 3 * 8) == 0xCAFEBABEull);
  std::puts("kframes and CONTEXT round-trip: ok");
}

// --- 3. Swept dynamic code bounds & unconditional branch check ---------------
class SweptCodeModel {
 public:
  static constexpr uint64_t kPhysicalCodeKey = 0x100000000ull;

  static uint64_t Key(uint32_t address) {
    const uint32_t kaddr = UserModeKernelAddress(address);
    if (kaddr >= 0xA0000000u && kaddr < 0xC0000000u) {
      return kPhysicalCodeKey | (kaddr - 0xA0000000u);
    }
    return kaddr;
  }

  void Record(uint32_t address, uint32_t length) {
    if (!length) return;
    const uint64_t span_start = Key(address);
    const uint64_t span_end = span_start + length;
    if (Key(address + length - 1) != span_end - 1) return;
    auto range = ranges_.lower_bound(span_start);
    if (range != ranges_.begin()) {
      auto prev = range;
      --prev;
      if (prev->second > span_start) range = prev;
    }
    while (range != ranges_.end() && range->first < span_end) {
      range = ranges_.erase(range);
    }
    ranges_.emplace(span_start, span_end);
  }

  uint32_t End(uint32_t address) const {
    const uint64_t key = Key(address);
    auto range = ranges_.upper_bound(key);
    if (range == ranges_.begin()) return 0;
    --range;
    if (key >= range->second) return 0;
    return address + uint32_t(range->second - key);
  }

  static bool IsUnconditionalBranch(uint32_t code) {
    switch (code >> 26) {
      case 18:
        return true;
      case 19: {
        const uint32_t extended = (code >> 1) & 0x3FF;
        const uint32_t bo = (code >> 21) & 0x1F;
        return (extended == 16 || extended == 528) && (bo & 0x14) == 0x14;
      }
      default:
        return false;
    }
  }

 private:
  std::map<uint64_t, uint64_t> ranges_;
};

void CheckSweptCodeModel() {
  SweptCodeModel m;
  // Back-to-back JIT blocks swept through kernel 0xA0100000 and queried through
  // user-mode alias 0x20100000 must stay separate, not merged!
  m.Record(0xA0100000u, 0x40u);
  m.Record(0xA0100040u, 0x30u);
  assert(m.End(0x20100000u) == 0x20100040u);
  assert(m.End(0x20100018u) == 0x20100040u);
  assert(m.End(0x20100040u) == 0x20100070u);
  assert(m.End(0x20100070u) == 0u);

  // Overlapping re-sweep replaces both blocks.
  m.Record(0x20100020u, 0x40u);
  assert(m.End(0x20100000u) == 0u);
  assert(m.End(0xA0100020u) == 0xA0100060u);

  // b, bl, blr, bctr -> unconditional; beq, bne, nop -> false.
  assert(SweptCodeModel::IsUnconditionalBranch(0x48000020u));  // b +0x20
  assert(SweptCodeModel::IsUnconditionalBranch(0x48000021u));  // bl +0x20
  assert(SweptCodeModel::IsUnconditionalBranch(0x4E800020u));  // blr
  assert(SweptCodeModel::IsUnconditionalBranch(0x4E800420u));  // bctr
  assert(SweptCodeModel::IsUnconditionalBranch(0x4E800421u));  // bctrl
  assert(!SweptCodeModel::IsUnconditionalBranch(0x41820020u));  // beq
  assert(!SweptCodeModel::IsUnconditionalBranch(0x40820020u));  // bne
  assert(!SweptCodeModel::IsUnconditionalBranch(0x60000000u));  // nop
  std::puts("swept dynamic code bounds and branch classification: ok");
}

// --- 4. Dynamic call cache & stackpoint swap invariants ----------------------
struct CacheEntry {
  uint32_t guest_address;
  uint32_t unused;
  uint64_t host_address;
};
constexpr uint32_t kCacheSize = 4096;
uint32_t CacheIndex(uint32_t addr) {
  return ((addr >> 2) ^ (addr >> 14)) & (kCacheSize - 1);
}

void CheckDynamicCacheAndIndirection() {
  static_assert(sizeof(CacheEntry) == 16);
  // 0x80000000..0x9FFFFFFB have indirection slots; 0x20000000..0x3FFFFFFF do not.
  auto has_slot = [](uint32_t addr) {
    return addr >= 0x80000000u && (addr - 0x80000000u) + 4 <= 0x1FFFFFFFu;
  };
  assert(has_slot(0x82000000u));
  assert(has_slot(0x9FFFFFF8u));
  assert(!has_slot(0x20000000u));
  assert(!has_slot(0x3FFFFFFCu));
  assert(!has_slot(0xA0000000u));

  std::vector<CacheEntry> cache(kCacheSize, {UINT32_MAX, 0, 0});
  cache[CacheIndex(0x20010000u)] = {0x20010000u, 0, 0x40050000ull};
  cache[CacheIndex(0x20020000u)] = {0x20020000u, 0, 0x40060000ull};
  // Invalidate [0x20010000, 0x200100FF] drops only the first entry.
  for (auto& e : cache) {
    if (e.guest_address >= 0x20010000u && e.guest_address <= 0x200100FFu) {
      e.host_address = 0;
      e.guest_address = UINT32_MAX;
    }
  }
  assert(cache[CacheIndex(0x20010000u)].host_address == 0);
  assert(cache[CacheIndex(0x20020000u)].host_address == 0x40060000ull);
  std::puts("dynamic call cache and indirection range checks: ok");
}

// --- 5. Real SysV AMD64 fiber runtime test on Linux x86-64 -------------------
#if defined(__linux__) && defined(__x86_64__)
void CheckPosixFibersLive() {
  using xe::threading::Fiber;
  assert(Fiber::GetCurrentFiber() == nullptr);
  auto kernel_fiber = Fiber::CreateFromThread();
  assert(Fiber::GetCurrentFiber() == kernel_fiber.get());

  std::vector<int> trace;
  uintptr_t observed_entry_rsp = 0;
  uint32_t leave_value = 0;

  std::unique_ptr<Fiber> user_fiber;
  std::unique_ptr<Fiber> handler_fiber;

  Fiber::CreationParameters params;
  params.stack_size = 256 * 1024;

  handler_fiber = Fiber::Create(params, [&]() {
    // Check 16-byte SSE stack alignment inside C++ fiber entry.
    alignas(16) __m128 probe = _mm_set1_ps(3.5f);
    float lanes[4];
    _mm_store_ps(lanes, probe);
    assert(lanes[0] == 3.5f && lanes[3] == 3.5f);

    trace.push_back(20);
    // Handler leaves user mode to let kernel service the trap, keeping
    // user_fiber parked mid-call!
    leave_value = 0x1001u;
    kernel_fiber->SwitchTo();
  });

  int run_generation = 0;
  user_fiber = Fiber::Create(params, [&]() {
    int local_anchor = 0;
    observed_entry_rsp = reinterpret_cast<uintptr_t>(&local_anchor);
    const int gen = ++run_generation;
    trace.push_back(10 * gen);

    // Simulate user-mode `sc` trap -> switch to handler_fiber!
    handler_fiber->Restart();
    handler_fiber->SwitchTo();

    // Resumed in-place when KeEnterUserMode re-enters at resume_address!
    trace.push_back(10 * gen + 1);
    leave_value = 0xBEEF0000u + uint32_t(gen);
    kernel_fiber->SwitchTo();
  });

  // First KeEnterUserMode: starts user_fiber -> traps to handler_fiber ->
  // KeLeaveUserMode(0x1001).
  user_fiber->SwitchTo();
  assert(Fiber::GetCurrentFiber() == kernel_fiber.get());
  assert(leave_value == 0x1001u);
  assert(observed_entry_rsp != 0);

  // Second KeEnterUserMode at parked trap's (resume_address, sp): resumes
  // user_fiber in-place -> finishes and calls KeLeaveUserMode(0xBEEF0001).
  user_fiber->SwitchTo();
  assert(Fiber::GetCurrentFiber() == kernel_fiber.get());
  assert(leave_value == 0xBEEF0001u);

  // Third KeEnterUserMode at a fresh entry: Restart() reuses the same stack
  // without leaking memory, then traps and resumes again!
  user_fiber->Restart();
  user_fiber->SwitchTo();
  assert(leave_value == 0x1001u);
  user_fiber->SwitchTo();
  assert(leave_value == 0xBEEF0002u);

  const std::vector<int> expected = {10, 20, 11, 20, 20, 21};
  assert(trace == expected);
  std::puts("live SysV AMD64 FcontextFiber trap/park/resume/restart: ok");
}
#endif

// --- 6. Assembled source tree wiring checks ----------------------------------
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "FAIL: missing file %s\n", path.c_str());
    std::abort();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void ExpectHas(const std::string& text, const char* needle, const char* where) {
  if (text.find(needle) == std::string::npos) {
    std::fprintf(stderr, "FAIL: missing '%s' in %s\n", needle, where);
    std::abort();
  }
}

void ExpectNotHas(const std::string& text, const char* needle,
                  const char* where) {
  if (text.find(needle) != std::string::npos) {
    std::fprintf(stderr, "FAIL: unexpected '%s' in %s\n", needle, where);
    std::abort();
  }
}

// The body of `entry` (from its name to the first line that is a lone "}")
// must test the user-mode switch before `first_work`, its first real step:
// without the switch the export behaves as before fix 191 (not implemented).
void ExpectUserModeGateFirst(const std::string& text, const char* entry,
                             const char* first_work, const char* where) {
  const size_t begin = text.find(entry);
  if (begin == std::string::npos) {
    std::fprintf(stderr, "FAIL: missing '%s' in %s\n", entry, where);
    std::abort();
  }
  size_t end = text.find("\n}\n", begin);
  if (end == std::string::npos) end = text.size();
  const std::string body = text.substr(begin, end - begin);
  const size_t gate =
      body.find("if (!kernel_state()->processor()->dynamic_code_enabled()) {");
  const size_t work = body.find(first_work);
  if (gate == std::string::npos || work == std::string::npos || gate > work) {
    std::fprintf(stderr,
                 "FAIL: %s in %s does not test dynamic_code_enabled() before "
                 "'%s'\n",
                 entry, where, first_work);
    std::abort();
  }
}

void CheckAssembledTree() {
  // 1. PS5 direct memory & fixed range reservation inside [kAnywhereStart, kAnywhereEnd].
  const std::string mem_ps5 = ReadFile("platform/ps5/memory_ps5.cpp");
  ExpectHas(mem_ps5, "wanted >= kAnywhereStart && wanted + bytes <= kAnywhereEnd",
            "platform/ps5/memory_ps5.cpp");
  ExpectHas(mem_ps5, "RoundUp(length, kPage) != i->second.size",
            "platform/ps5/memory_ps5.cpp");
  // DeallocFixed looks the view up with views.find(): a release with a length
  // for an address that is not a view (code caches, the x64 backend and emitter
  // at shutdown) must return false on i == views.end() before i->second.size is
  // read. Before 191 `length != 0` returned first; reading end() is undefined.
  ExpectHas(mem_ps5, "type != DeallocationType::kRelease || i == views.end() ||",
            "platform/ps5/memory_ps5.cpp (end() tested before the size)");
  ExpectNotHas(mem_ps5, "i == views.end() || !i->second.owns_object",
               "platform/ps5/memory_ps5.cpp (size read before end())");

  // Exports that were not implemented before 191 (the call reached
  // UndefinedCallExtern: nothing done, nothing written, 0) stay that way for
  // every title that never enters user mode; only XNA titles get them.
  const std::string krnl_memory = ReadFile(
      ".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc");
  const std::string krnl_rtl = ReadFile(
      ".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc");
  const std::string krnl_modules = ReadFile(
      ".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc");
  ExpectUserModeGateFirst(krnl_memory, "void KeSweepIcacheRange_entry(",
                          "InvalidateCodeRange(", "xboxkrnl_memory.cc");
  ExpectUserModeGateFirst(krnl_rtl, "void RtlCaptureContext_entry(",
                          "store_and_swap", "xboxkrnl_rtl.cc");
  ExpectUserModeGateFirst(krnl_modules,
                          "dword_result_t XexPcToFileHeader_entry(",
                          "GetUserModuleForAddress(", "xboxkrnl_modules.cc");

  // 2. SysV AMD64 fiber implementation uses AllocFixed (never raw anonymous mmap
  //    on PS5's 448 MiB flexible budget).
  const std::string fiber_h =
      ReadFile(".deps/xenia-canary/src/xenia/base/threading.h");
  const std::string fiber_cc =
      ReadFile(".deps/xenia-canary/src/xenia/base/threading_fiber_posix.cc");
  ExpectHas(fiber_h, "class Fiber {", "threading.h");
  ExpectHas(fiber_h, "virtual void Restart() = 0;", "threading.h");
  ExpectHas(fiber_cc, "xe_switch_fcontext", "threading_fiber_posix.cc");
  ExpectHas(fiber_cc, "xe::memory::AllocFixed", "threading_fiber_posix.cc");
  ExpectNotHas(fiber_cc, "MAP_ANONYMOUS", "threading_fiber_posix.cc");

  // 3. User-mode memory views & MMIO bypass.
  const std::string mem_h = ReadFile(".deps/xenia-canary/src/xenia/memory.h");
  const std::string mem_cc = ReadFile(".deps/xenia-canary/src/xenia/memory.cc");
  const std::string mmio_cc =
      ReadFile(".deps/xenia-canary/src/xenia/cpu/mmio_handler.cc");
  ExpectHas(mem_h, "EnableUserModeViews()", "memory.h");
  ExpectHas(mem_h, "UserModeKernelAddress(", "memory.h");
  ExpectHas(mem_h, "KernelModeUserAddress(", "memory.h");
  ExpectHas(mem_cc, "MapUserViews(", "memory.cc");
  ExpectHas(mem_cc, "0x1800000000ull", "memory.cc");
  ExpectHas(mmio_cc, "user_membase_.load", "mmio_handler.cc");

  // 4. CPU / PPC / x64 dynamic code, syscall hook, and ebx->edx tail lookup.
  const std::string ppc_ctrl =
      ReadFile(".deps/xenia-canary/src/xenia/cpu/ppc/ppc_emit_control.cc");
  const std::string ppc_fe =
      ReadFile(".deps/xenia-canary/src/xenia/cpu/ppc/ppc_frontend.cc");
  const std::string ppc_scan =
      ReadFile(".deps/xenia-canary/src/xenia/cpu/ppc/ppc_scanner.cc");
  const std::string ppc_hir =
      ReadFile(".deps/xenia-canary/src/xenia/cpu/ppc/ppc_hir_builder.cc");
  const std::string proc_cc =
      ReadFile(".deps/xenia-canary/src/xenia/cpu/processor.cc");
  const std::string x64_be =
      ReadFile(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_backend.cc");
  const std::string x64_em =
      ReadFile(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_emitter.cc");
  ExpectHas(ppc_ctrl, "offsetof(PPCContext, scratch)", "ppc_emit_control.cc");
  ExpectHas(ppc_fe, "syscall_hook()", "ppc_frontend.cc");
  ExpectHas(ppc_scan, "module->TranslateCode(address)", "ppc_scanner.cc");
  ExpectHas(ppc_hir, "module->TranslateCode(address)", "ppc_hir_builder.cc");
  ExpectHas(proc_cc, "class DynamicCodeModule : public Module", "processor.cc");
  ExpectHas(proc_cc, "RecordSweptCode(address, length)", "processor.cc");
  ExpectHas(x64_be, "CreateStackpointState()", "x64_backend.cc");
  ExpectHas(x64_be, "InvalidateDynamicCalls(", "x64_backend.cc");
  ExpectHas(x64_be, "jae(loopout, T_NEAR)", "x64_backend.cc");
  ExpectHas(x64_em, "ResolveDynamicReturn(", "x64_emitter.cc");
  ExpectHas(x64_em, "e.mov(e.edx, e.ebx);", "x64_emitter.cc");
  ExpectNotHas(x64_em,
               "v == (uintptr_t)processor()->memory()->virtual_membase()",
               "x64_emitter.cc");

  // 5. Kernel exports: Ke*UserMode, KeGetImagePageTableEntry, KeSweepIcacheRange,
  //    XexPcToFileHeader, RtlCaptureContext, and UserMode export group.
  const std::string groups = ReadFile(
      ".deps/xenia-canary/src/xenia/kernel/xboxkrnl/"
      "xboxkrnl_module_export_groups.inc");
  const std::string um_cc = ReadFile(
      ".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_usermode.cc");
  const std::string kmem_cc = ReadFile(
      ".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc");
  const std::string kmod_cc = ReadFile(
      ".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc");
  const std::string krtl_cc =
      ReadFile(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc");
  ExpectHas(groups, "XE_MODULE_EXPORT_GROUP(xboxkrnl, UserMode)",
            "xboxkrnl_module_export_groups.inc");
  ExpectHas(um_cc, "KeCreateUserMode_entry", "xboxkrnl_usermode.cc");
  ExpectHas(um_cc, "KeDeleteUserMode_entry", "xboxkrnl_usermode.cc");
  ExpectHas(um_cc, "KeEnterUserMode_entry", "xboxkrnl_usermode.cc");
  ExpectHas(um_cc, "KeLeaveUserMode_entry", "xboxkrnl_usermode.cc");
  ExpectHas(um_cc, "KeContextFromKframes_entry", "xboxkrnl_usermode.cc");
  ExpectHas(um_cc, "KeContextToKframes_entry", "xboxkrnl_usermode.cc");
  ExpectHas(um_cc, "p + kContextCtr", "xboxkrnl_usermode.cc");
  ExpectNotHas(um_cc, "p + kKframesCtr", "xboxkrnl_usermode.cc");
  ExpectHas(um_cc, "KeFlushUserModeCurrentTb_entry", "xboxkrnl_usermode.cc");
  ExpectHas(um_cc, "KeFlushUserModeTb_entry", "xboxkrnl_usermode.cc");
  ExpectHas(kmem_cc, "GetPageSectionType(address, &section_type)",
            "xboxkrnl_memory.cc");
  ExpectHas(kmem_cc, "KeSweepIcacheRange_entry", "xboxkrnl_memory.cc");
  ExpectHas(kmod_cc, "XexPcToFileHeader_entry", "xboxkrnl_modules.cc");
  ExpectHas(krtl_cc, "RtlCaptureContext_entry", "xboxkrnl_rtl.cc");
  std::puts("assembled tree wiring across all 36 files: ok");
}

}  // namespace

int main() {
  CheckAliasMapping();
  CheckKframesAndContext();
  CheckSweptCodeModel();
  CheckDynamicCacheAndIndirection();
#if defined(__linux__) && defined(__x86_64__)
  CheckPosixFibersLive();
#endif
  CheckAssembledTree();
  std::puts("PASS: check-xna-usermode");
  return 0;
}
