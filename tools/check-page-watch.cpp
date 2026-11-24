// SPDX-License-Identifier: MIT
// Fix 156h (tools/source-1.0.2/fixes/156h-request-page-watch.patch; Kinect Sports Season Two
// 4D5309D6, issue 63): which heap operation changes the page table under a camera request.
// On 1.0.6-work-fix156g2 the darts load still failed 5 frame reads with "request not mapped"
// and fix 156g's "not writable" line never came: the requests' page (83507E78-83508100, the
// NUI library's data in the image) stopped being readable in the page table while the title
// kept using it. BaseHeap::Release clears the table and leaves the host mapping.
//
//   check-page-watch        run from an assembled tree's root
//
//  (a) page_watch.hpp: a page is added once (64 KiB aligned), up to 8; Hits covers a watched
//      page by any overlap and nothing else; Note calls the reporter only on a hit; Reset;
//  (b) the tree: memory.cc reports AllocFixed, AllocRange, Decommit, Release and Protect over
//      a watched page; the camera watches each accepted frame read's page, sets the reporter
//      while the camera is open (logged with thread and lr, 24 lines) and logs a refused
//      request's page state (8 lines).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/page_watch.hpp"

namespace pw = xbox360ps5::page_watch;

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

static int reported = 0;
static uint32_t last_address = 0;
static void Report(const char*, uint32_t address, uint32_t, uint32_t, uint32_t) {
  ++reported;
  last_address = address;
}

static void CheckWatch() {
  pw::Reset();
  CHECK(!pw::Hits(0x83500000, 0x10000));  // nothing watched
  CHECK(pw::Watch(0x83507E78));
  CHECK(!pw::Watch(0x83508100));  // the same 64 KiB page
  CHECK(pw::watched.load() == 1);
  CHECK(pw::Hits(0x83500000, 0x10000));
  CHECK(pw::Hits(0x82000000, 0x1650000));  // the whole image region
  CHECK(pw::Hits(0x8350FFFF, 1) && pw::Hits(0x834FFFFF, 2));
  CHECK(!pw::Hits(0x83510000, 0x10000) && !pw::Hits(0x834F0000, 0x10000));
  CHECK(!pw::Hits(0x83500000, 0));
  CHECK(!pw::Watch(0));
  for (uint32_t n = 1; n < pw::kSlots; ++n) CHECK(pw::Watch(0x40000000 + n * 0x10000));
  CHECK(!pw::Watch(0x50000000));  // full
  pw::reporter = &Report;
  pw::Note("Release", 0x82000000, 0x1650000, 0, 0x3);
  CHECK(reported == 1 && last_address == 0x82000000);
  pw::Note("Protect", 0x899E0000, 0xE0000, 0x2, 0x3);  // the darts module: no hit
  CHECK(reported == 1);
  pw::reporter = nullptr;
  pw::Note("Release", 0x83500000, 0x10000, 0, 0);  // no reporter: nothing
  CHECK(reported == 1);
  pw::Reset();
  CHECK(pw::watched.load() == 0 && !pw::Hits(0x83500000, 0x10000));
}

static void CheckTree() {
  const std::string mem = ReadText(".deps/xenia-canary/src/xenia/memory.cc");
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  if (mem.empty() || cam.empty()) {
    std::printf("FAIL: no assembled tree here (memory.cc, xboxkrnl_pscam.cc)\n");
    ++failures;
    return;
  }
  CHECK(Has(mem, "#include \"xbox360ps5/page_watch.hpp\""));
  for (const char* op : {"\"AllocFixed\"", "\"AllocRange\"", "\"Decommit\"", "\"Release\"", "\"Protect\""}) {
    const std::string call = std::string("xbox360ps5::page_watch::Note(");
    const size_t at = mem.find(op);
    CHECK(at != std::string::npos && mem.rfind(call, at) != std::string::npos &&
          at - mem.rfind(call, at) < 80);
  }
  // Release reports before the table entries are cleared.
  const size_t release = mem.find("\"Release\", base_address,");
  const size_t cleared = mem.find("page_entry.qword = 0;", release);
  CHECK(release != std::string::npos && cleared != std::string::npos);
  CHECK(Has(cam, "xbox360ps5::page_watch::Watch(request_ptr);  // fix 156h"));
  CHECK(Has(cam, "xbox360ps5::page_watch::reporter = &ReportRequestPageChange;  // fix 156h"));
  CHECK(Has(cam, "xbox360ps5::page_watch::reporter = nullptr;  // fix 156h"));
  CHECK(Has(cam, "covers a camera request "));
  CHECK(Has(cam, "if (shown.fetch_add(1) >= 24) return;"));
  CHECK(Has(cam, "NoteRequestPage(read.request_ptr);  // fix 156h"));
  CHECK(Has(cam, "Virtual Kinect: request {:08X}: heap {:08X} (page {:X}), protect {:X}, region "));
}

int main() {
  CheckWatch();
  CheckTree();
  if (failures) {
    std::printf("check-page-watch: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-page-watch: ok\n");
  return 0;
}
