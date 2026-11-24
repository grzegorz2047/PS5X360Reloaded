// SPDX-License-Identifier: MIT
// Fix 156j (tools/source-1.0.2/fixes/156j-dll-keeps-heap.patch; Kinect Sports Season Two
// 4D5309D6, issue 63): only the title's executable resets its heap when its image is read.
// XexModule::ReadImage reset the heap of every module it loaded. A DLL a title loads later
// (Season Two's sports, flags TITLE | DLL_MODULE = 0x9, at 89640000/899E0000) shares the
// 80000000 heap with the executable (flags 0x1): the reset cleared the executable's page-table
// entries while its memory stayed mapped and in use, so the camera refused the NUI library's
// read requests in the executable's data (fix 156h's line: "region 00000000+64E0000 state 0"
// from 83500000 to the DLL) and the sport's loading screen never ended.
//
//   check-xex-dll-heap        run from an assembled tree's root
//
//  (a) a model of the heap's table: the executable's pages committed, then a DLL read the old
//      way (reset) leaves them free; the fixed way keeps them; a DLL's second attempt (another
//      key) gives back only its own first attempt's image;
//  (b) the tree: ReadImage resets only for a module without XEX_MODULE_DLL_MODULE; a DLL's
//      retry releases the region only when it starts at the DLL's base and is reserved; the
//      attempts are counted per module (xex_module.h); a DLL read is logged (8 lines).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  std::string out;
  for (char c : s.str())
    if (c != '\r') out += c;
  return out;
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

// The 80000000 heap's table, one entry per 64 KiB page: the region base page, 0 = free.
struct Heap {
  static constexpr uint32_t kBase = 0x80000000, kPage = 0x10000;
  std::vector<uint32_t> region = std::vector<uint32_t>(0x1000, 0);
  uint32_t Page(uint32_t a) const { return (a - kBase) / kPage; }
  void Alloc(uint32_t base, uint32_t size) {
    for (uint32_t p = Page(base); p < Page(base) + size / kPage; ++p) region[p] = Page(base) + 1;
  }
  void Reset() { std::fill(region.begin(), region.end(), 0); }
  void Release(uint32_t base) {
    const uint32_t id = Page(base) + 1;
    for (auto& r : region) if (r == id) r = 0;
  }
  bool Committed(uint32_t a) const { return region[Page(a)] != 0; }
};
constexpr uint32_t kExeBase = 0x82000000, kExeSize = 0x1650000;
constexpr uint32_t kDllBase = 0x899E0000, kDllSize = 0xE0000;
constexpr uint32_t kRequest = 0x83507EF8;

// ReadImage's start, the old way or the fixed one, for one attempt.
static void ReadImageStart(Heap& heap, bool dll, uint32_t base, uint32_t& attempts, bool fixed) {
  if (!fixed || !dll) {
    heap.Reset();
  } else if (attempts > 0 && heap.Committed(base) && heap.region[heap.Page(base)] == heap.Page(base) + 1) {
    heap.Release(base);
  }
  ++attempts;
}

static void CheckModel() {
  for (bool fixed : {false, true}) {
    Heap heap;
    uint32_t exe_attempts = 0, dll_attempts = 0;
    ReadImageStart(heap, false, kExeBase, exe_attempts, fixed);
    heap.Alloc(kExeBase, kExeSize);
    CHECK(heap.Committed(kRequest));
    ReadImageStart(heap, true, kDllBase, dll_attempts, fixed);   // the darts module
    heap.Alloc(kDllBase, kDllSize);
    CHECK(heap.Committed(kRequest) == fixed);  // the old way: the executable's pages free
    CHECK(heap.Committed(kDllBase));
    if (fixed) {
      // A second attempt with another key: only the DLL's own first image goes.
      ReadImageStart(heap, true, kDllBase, dll_attempts, fixed);
      CHECK(!heap.Committed(kDllBase));
      CHECK(heap.Committed(kRequest) && heap.Committed(kExeBase));
    }
  }
}

static void CheckTree() {
  const std::string cc = ReadText(".deps/xenia-canary/src/xenia/cpu/xex_module.cc");
  const std::string h = ReadText(".deps/xenia-canary/src/xenia/cpu/xex_module.h");
  if (cc.empty() || h.empty()) {
    std::printf("FAIL: no assembled tree here (xex_module.cc, xex_module.h)\n");
    ++failures;
    return;
  }
  CHECK(!Has(cc, "  memory()->LookupHeap(base_address_)->Reset();\n"));
  const size_t at = cc.find("const bool dll = (xex_header()->module_flags & XEX_MODULE_DLL_MODULE) != 0;");
  CHECK(at != std::string::npos);
  const std::string block = at == std::string::npos ? "" : cc.substr(at, 900);
  CHECK(Has(block, "    if (!dll) {\n      heap->Reset();\n    } else if (read_image_attempts_ > 0) {"));
  CHECK(Has(block, "info.allocation_base == base_address_ &&\n          (info.state & kMemoryAllocationReserve)) {\n        heap->Release(base_address_);"));
  CHECK(Has(block, "++read_image_attempts_;"));
  CHECK(Has(block, "XEX: DLL {} read at {:08X} without resetting its heap (fix 156j)"));
  CHECK(Has(h, "  uint32_t read_image_attempts_ = 0;"));
  // Still at the start of ReadImage, after the patch case.
  const size_t read = cc.find("int XexModule::ReadImage(");
  const size_t patch = cc.find("if (is_patch()) {", read);
  CHECK(read != std::string::npos && patch != std::string::npos && patch < at);
}

int main() {
  CheckModel();
  CheckTree();
  if (failures) {
    std::printf("check-xex-dll-heap: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-xex-dll-heap: ok\n");
  return 0;
}
