// SPDX-License-Identifier: MIT
// Fix 82 (tools/source-1.0.2/fixes/82-enumerator-buffers.patch): Battlefield 3 still filled the
// guest's 4 KiB heap with fix 80 (1.0.5_candidate_57): its enumerators were freed, but the about
// 21 KiB XamAlloc buffer the title library makes with each (about 15 a second) was kept, 3,768
// allocated and 5 freed. On a console the enumerator's close message reaches its XAM app, which
// frees that buffer; Xenia has no such handler. The XamAlloc blocks an enumerator's object points
// to are now freed with the enumerator.
//  (a) include/xbox360ps5/xam_buffers.hpp: a live XamAlloc block an enumerator's words point to
//      is taken (once, with its offset), unless another live enumerator's words point to it too;
//      XamFree of a block freed that way does nothing, XamFree of any other address frees as
//      before, and a new XamAlloc at a freed block's address is the title's again;
//  (b) the assembled tree: XamAlloc notes its blocks, XamFree asks before freeing, and the
//      enumerator's destructor frees what it held before forgetting the enumerator.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/xam_buffers.hpp"

#include <cstdio>
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
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
namespace xb = xbox360ps5::xam_buffers;

void CheckBuffers() {
  xb::Allocated(0x31000000, 0x5401);  // the title's buffer for enumerator A
  xb::Allocated(0x31010000, 0x5401);  // for enumerator B, shared with C
  xb::Allocated(0x31020000, 64);      // a buffer no enumerator holds
  // A's words: its fields, then its private area with the buffer at +0x1C.
  const std::vector<uint32_t> a = {0xFB, 0x58039, 0x5803A, 0, 8, 0, 0, 0x31000000, 0x31000000};
  const std::vector<uint32_t> b = {0xFB, 0x58039, 0x5803A, 0, 8, 0, 0, 0x31010000};
  const std::vector<uint32_t> c = {0xFB, 0x58039, 0x5803A, 0, 8, 0, 0x31010000};
  const auto held = xb::TakeHeld(a, b);
  Expect(held.size() == 1 && held[0].address == 0x31000000 && held[0].size == 0x5401 && held[0].offset == 0x1C,
         "A's buffer taken once, with its size and offset");
  Expect(xb::TakeHeld(b, c).empty(), "B's buffer is also C's: kept");
  Expect(!xb::Freeing(0x31000000), "XamFree of A's buffer after A went: nothing to free");
  Expect(xb::Freeing(0x31000000), "and only once: a later XamFree of that address frees as before");
  Expect(xb::Freeing(0x31020000), "XamFree of a buffer no enumerator held frees it");
  Expect(xb::Freeing(0x3FFFF000), "XamFree of an address it does not know frees it, as before");
  // A freed block's address given out again by XamAlloc is the title's.
  const auto again = xb::TakeHeld({0x31030000}, {});
  Expect(again.empty(), "a word that is no XamAlloc block is not taken");
  xb::Allocated(0x31040000, 32);
  Expect(xb::TakeHeld({0x31040000}, {}).size() == 1, "taken");
  xb::Allocated(0x31040000, 48);
  Expect(xb::Freeing(0x31040000), "allocated again at that address: XamFree frees the new block");
  Expect(xb::TakeHeld(c, {}).size() == 1, "C's buffer once B no longer holds it");
}

void CheckTree() {
  const std::string K = ".deps/xenia-canary/src/xenia/kernel/";
  const std::string info = Read(K + "xam/xam_info.cc");
  const size_t alloc = info.find("uint32_t ptr = kernel_state()->memory()->SystemHeapAlloc(size);");
  const size_t noted = info.find("xbox360ps5::xam_buffers::Allocated(ptr, size);  // fix 82", alloc);
  const size_t free_fn = info.find("dword_result_t XamFree_entry(lpdword_t ptr) {");
  const size_t asked = info.find("if (!xbox360ps5::xam_buffers::Freeing(ptr.guest_address())) {", free_fn);
  const size_t freed = info.find("kernel_state()->memory()->SystemHeapFree(ptr.guest_address());", free_fn);
  Expect(alloc != std::string::npos && noted != std::string::npos && noted - alloc < 120 && asked != std::string::npos &&
             asked < freed,
         "XamAlloc notes its blocks, XamFree asks before freeing");
  const std::string en = Read(K + "xenumerator.cc");
  const size_t dtor = en.find("XEnumerator::~XEnumerator() {");
  const size_t take = en.find("xbox360ps5::xam_buffers::TakeHeld(", dtor);
  const size_t heap_free = en.find("memory()->SystemHeapFree(held.address);", take);
  const size_t forget = en.find("OwnedEnumerators().erase(it);", dtor);
  Expect(dtor != std::string::npos && take != std::string::npos && heap_free != std::string::npos && heap_free < forget,
         "the destructor frees what the enumerator held, before forgetting it");
  Expect(en.find("const auto theirs = words_of(at, enumerator->extra_size_);", dtor) < take,
         "the other live enumerators' words are left out");
}

}  // namespace

int main() {
  CheckBuffers();
  CheckTree();
  if (failures) {
    std::printf("check-enumerator-buffers: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-enumerator-buffers: ok\n");
  return 0;
}
