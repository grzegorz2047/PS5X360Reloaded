// SPDX-License-Identifier: MIT
// Fix 192 (tools/source-1.0.2/fixes/192-reserved-store-mismatch.patch): GTA V (545408A7) died
// after 4-8 minutes of driving with SIGTRAP in the core's reserved-store helper. Its lock-free list
// pop (834710D8) reserves the head with ldarx and, when the list is empty, drops the reservation
// with a stdcx. of 0 to a slot on its own stack (83471168: li r4,0; addi r3,r1,0x60). PowerPC
// allows a store-conditional to another address (whether it stores is undefined), but the x64
// backend's reserved_store helper treated it as impossible and ran int3
// (reservation_isnt_for_our_addr); the load-and-reserve helper did the same for a second
// lwarx/ldarx while a reservation was held (already_has_a_reservation). Now:
//  - a store-conditional to another 64 KiB block than the one reserved gives the reservation back,
//    stores nothing and fails;
//  - a load-and-reserve while one is held gives the old block back and takes the new one.
// The check has two parts:
//  (a) a model of the helpers' reservation bitmap (one bit per 64 KiB block, the per-thread
//      has-reserve flag, the cached block): the GTA V pop, a store to the reserved address,
//      another thread blocked by a held reservation, a second reserve replacing the first, the
//      old behaviour trapping where the new one fails;
//  (b) the assembled x64_backend.cc: both helpers' former traps now give the block back (lock
//      btr of the cached offset and bit), the store-conditional returns failure without storing,
//      the load-and-reserve jumps back to take the new block, rax is left alone in the
//      load-and-reserve helper (its callers read through it), and the double-clear trap stays.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

// ---- (a) the model --------------------------------------------------------------------------

constexpr unsigned kBlockShift = 16;  // RESERVE_BLOCK_SHIFT
constexpr size_t kBlocks = (4ull << 30) >> kBlockShift;

struct Bitmap {
  std::vector<uint64_t> words = std::vector<uint64_t>(kBlocks / 64, 0);
  bool Test(uint32_t word, uint32_t bit) const { return (words[word] >> bit) & 1; }
};

struct Thread {
  bool has_reserve = false;
  uint32_t cached_word = 0, cached_bit = 0;
  uint64_t cached_value = 0;
  bool trapped = false;  // the old helpers' int3
};

struct Memory {
  std::vector<uint64_t> qwords = std::vector<uint64_t>(1 << 16, 0);  // guest qwords, 512 KiB
  uint64_t& At(uint32_t guest) { return qwords[(guest >> 3) % qwords.size()]; }
};

void Locate(uint32_t guest, uint32_t& word, uint32_t& bit) {
  const uint32_t block = guest >> kBlockShift;
  word = block / 64;
  bit = block % 64;
}

// EmitTryAcquireReservationHelper, then the load.
uint64_t ReserveLoad(Bitmap& map, Memory& mem, Thread& t, uint32_t guest, bool fixed) {
  if (t.has_reserve) {
    t.has_reserve = false;
    if (!fixed) {
      t.trapped = true;
      return 0;
    }
    map.words[t.cached_word] &= ~(1ull << t.cached_bit);  // give the old block back
  }
  uint32_t word, bit;
  Locate(guest, word, bit);
  const bool was_set = map.Test(word, bit);
  map.words[word] |= 1ull << bit;
  t.has_reserve = !was_set;
  t.cached_word = word;
  t.cached_bit = bit;
  t.cached_value = mem.At(guest);
  return t.cached_value;
}

// EmitReservedStoreHelper: true when the store-conditional succeeds.
bool ReservedStore(Bitmap& map, Memory& mem, Thread& t, uint32_t guest, uint64_t value,
                   bool fixed) {
  if (!t.has_reserve) return false;
  t.has_reserve = false;
  uint32_t word, bit;
  Locate(guest, word, bit);
  if (word != t.cached_word || bit != t.cached_bit) {
    if (!fixed) {
      t.trapped = true;
      return false;
    }
    map.words[t.cached_word] &= ~(1ull << t.cached_bit);  // give the reservation back
    return false;                                        // nothing stored
  }
  uint64_t& slot = mem.At(guest);
  const bool same = slot == t.cached_value;
  if (same) slot = value;
  map.words[word] &= ~(1ull << bit);
  return same;
}

void Model() {
  constexpr uint32_t kHead = 0x40061000;   // a list head in the guest heap
  constexpr uint32_t kStack = 0x7056E8B0;  // r1+0x60 on a guest thread's stack
  // GTA V's pop on an empty list: ldarx the head (0), stdcx. 0 to the stack slot.
  for (const bool fixed : {false, true}) {
    Bitmap map;
    Memory mem;
    Thread t;
    mem.At(kStack) = 0x1234;
    const uint64_t head = ReserveLoad(map, mem, t, kHead, fixed);
    Expect(head == 0, "the empty list's head reads 0");
    const bool ok = ReservedStore(map, mem, t, kStack, 0, fixed);
    if (!fixed) {
      Expect(t.trapped, "old helper: the stack store-conditional trapped (the GTA V crash)");
      continue;
    }
    Expect(!t.trapped, "fixed: no trap on the stack store-conditional");
    Expect(!ok, "fixed: the store-conditional to another block fails");
    Expect(mem.At(kStack) == 0x1234, "fixed: nothing is stored to the stack slot");
    uint32_t word, bit;
    Locate(kHead, word, bit);
    Expect(!map.Test(word, bit), "fixed: the head's block is given back");
    Expect(!t.has_reserve, "fixed: the thread holds no reservation afterwards");
    // Another thread can now reserve and update the head.
    Thread other;
    ReserveLoad(map, mem, other, kHead, fixed);
    Expect(other.has_reserve, "another thread reserves the head after it");
    Expect(ReservedStore(map, mem, other, kHead, 0x40070000, fixed) &&
               mem.At(kHead) == 0x40070000,
           "and its store-conditional to the head succeeds");
  }

  // The normal pair is unchanged: reserve, store to the same address.
  {
    Bitmap map;
    Memory mem;
    Thread t;
    mem.At(kHead) = 7;
    ReserveLoad(map, mem, t, kHead, true);
    Expect(ReservedStore(map, mem, t, kHead, 8, true) && mem.At(kHead) == 8,
           "reserve then store to the same address succeeds and stores");
    uint32_t word, bit;
    Locate(kHead, word, bit);
    Expect(!map.Test(word, bit), "the block is free after the store");
  }

  // A store whose value changed in between fails (another writer), as before.
  {
    Bitmap map;
    Memory mem;
    Thread t;
    mem.At(kHead) = 1;
    ReserveLoad(map, mem, t, kHead, true);
    mem.At(kHead) = 2;  // someone else wrote it
    Expect(!ReservedStore(map, mem, t, kHead, 3, true) && mem.At(kHead) == 2,
           "a changed value makes the store-conditional fail and store nothing");
  }

  // A reservation held by one thread blocks another thread's reservation of that block.
  {
    Bitmap map;
    Memory mem;
    Thread a, b;
    ReserveLoad(map, mem, a, kHead, true);
    ReserveLoad(map, mem, b, kHead + 8, true);
    Expect(a.has_reserve && !b.has_reserve, "the second thread does not get a held block");
    Expect(!ReservedStore(map, mem, b, kHead + 8, 5, true), "so its store-conditional fails");
    Expect(ReservedStore(map, mem, a, kHead, 6, true), "the holder's store still succeeds");
  }

  // A second load-and-reserve replaces the first (old: trap).
  for (const bool fixed : {false, true}) {
    Bitmap map;
    Memory mem;
    Thread t;
    constexpr uint32_t kOther = 0x40210000;  // another 64 KiB block
    ReserveLoad(map, mem, t, kHead, fixed);
    ReserveLoad(map, mem, t, kOther, fixed);
    if (!fixed) {
      Expect(t.trapped, "old helper: a second reserve trapped");
      continue;
    }
    uint32_t w1, b1, w2, b2;
    Locate(kHead, w1, b1);
    Locate(kOther, w2, b2);
    Expect(!t.trapped, "fixed: a second reserve does not trap");
    Expect(!map.Test(w1, b1), "fixed: the first block is given back");
    Expect(map.Test(w2, b2) && t.has_reserve, "fixed: the second block is held");
    Expect(ReservedStore(map, mem, t, kOther, 9, true) && mem.At(kOther) == 9,
           "fixed: the store-conditional to the second address succeeds");
    // The same block reserved twice: given back and taken again.
    ReserveLoad(map, mem, t, kHead, true);
    ReserveLoad(map, mem, t, kHead + 0x100, true);
    Expect(t.has_reserve, "fixed: the same block reserved twice is still held");
  }
}

// ---- (b) the assembled tree -----------------------------------------------------------------

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

void Tree() {
  const std::string src = Read(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_backend.cc");
  if (src.empty()) {
    std::printf("note: no assembled tree here, tree checks skipped\n");
    return;
  }
  const std::string acquire =
      Between(src, "void* X64HelperEmitter::EmitTryAcquireReservationHelper()",
              "void* X64HelperEmitter::EmitReservedStoreHelper(");
  const std::string store = Between(src, "void* X64HelperEmitter::EmitReservedStoreHelper(",
                                    "void X64HelperEmitter::EmitSaveVolatileRegs()");
  Expect(!acquire.empty() && !store.empty(), "both helpers found in x64_backend.cc");

  // Load-and-reserve: the re-entry label before the block computation, the old block given back
  // through rdx/r9 only, then the jump back; no int3 left.
  const std::string held = Between(acquire, "L(already_has_a_reservation);", "code_offsets.prolog_stack_alloc");
  Expect(acquire.find("L(acquire_new_reservation);\n  shr(ecx, RESERVE_BLOCK_SHIFT);") != std::string::npos,
         "the load-and-reserve helper takes the new block at acquire_new_reservation");
  Expect(held.find("cached_reserve_offset") != std::string::npos &&
             held.find("cached_reserve_bit") != std::string::npos &&
             held.find("btr(qword[rdx], r9);") != std::string::npos &&
             held.find("jmp(acquire_new_reservation") != std::string::npos,
         "a held reservation is given back (lock btr of the cached block) before the new one");
  Expect(held.find("DebugBreak") == std::string::npos,
         "no trap left for a load-and-reserve while one is held");
  Expect(held.find("rax") == std::string::npos && held.find("eax") == std::string::npos,
         "the load-and-reserve helper leaves rax alone (its callers read through it)");

  // Store-conditional: the mismatch path gives the block back, stores nothing, returns failure.
  const std::string mismatch =
      Between(store, "L(reservation_isnt_for_our_addr);", "L(somehow_double_cleared);");
  Expect(mismatch.find("btr(qword[rdx], rax);") != std::string::npos &&
             mismatch.find("cached_reserve_offset") != std::string::npos &&
             mismatch.find("cached_reserve_bit") != std::string::npos,
         "a store-conditional to another block gives the reservation back");
  Expect(mismatch.find("cmpxchg") == std::string::npos,
         "a store-conditional to another block stores nothing");
  Expect(mismatch.find("xor_(eax, eax);") != std::string::npos &&
             mismatch.find("cmp(ax, 0x0101);") != std::string::npos &&
             mismatch.find("ret();") != std::string::npos,
         "it returns failure (ZF clear) like the done path with CF clear");
  Expect(mismatch.find("DebugBreak") == std::string::npos,
         "no trap left for a store-conditional to another block");
  Expect(Between(store, "L(somehow_double_cleared);", "code_offsets.prolog_stack_alloc")
                 .find("DebugBreak") != std::string::npos,
         "the double-clear trap (a real inconsistency) stays");
}

}  // namespace

int main() {
  Model();
  Tree();
  if (failures) {
    std::printf("check-reserved-store: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-reserved-store: ok\n");
  return 0;
}
