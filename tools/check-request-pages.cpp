// SPDX-License-Identifier: MIT
// Fix 156g (tools/source-1.0.2/fixes/156g-request-pages.patch; Kinect Sports Season Two
// 4D5309D6, issue 63): the virtual camera answers a request block that is readable, and
// writes its replies through a window. On 1.0.6-work-fix156e Season Two's darts load failed 5
// frame reads with "request not mapped": its read requests (83507E78-83508080, in its image's
// data section) stopped being writable in the page table at the load, and the camera refused
// them at completion and every resubmission (INVALID_PARAMETER), so the title read no frame
// again. The console's driver writes its replies regardless of the caller's protection.
//
//   check-request-pages        run from an assembled tree's root
//
//  the tree: every check of a request block is IsRequestRange (readable; a page that is not
//  writable is logged, 8 lines); each reply written into a request (Handle's synchronous and
//  asynchronous answers, GetState without a camera, a color frame's settings generation) is
//  inside a RequestWriteWindow, which makes the pages writable and puts their protection
//  back; frame buffers and frame counters still have to be writable; NtProtectVirtualMemory
//  logs the title's changes (32 lines).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
static size_t Count(const std::string& text, const char* what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

int main() {
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  const std::string mem = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc");
  if (cam.empty() || mem.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_pscam.cc, xboxkrnl_memory.cc)\n");
    return 1;
  }
  // The request checks: readable only.
  CHECK(!Has(cam, "IsGuestRange(request_ptr"));
  CHECK(!Has(cam, "IsGuestRange(read.request_ptr"));
  CHECK(Has(cam, "if (!IsRequestRange(request_ptr, 8)) return X_STATUS_INVALID_PARAMETER;"));
  CHECK(Count(cam, "IsRequestRange(request_ptr, ") == 4);
  CHECK(Has(cam, "if (!IsRequestRange(read.request_ptr,sizeof(X_PSCAM_READ_FRAME_REQUEST)))"));
  const size_t def = cam.find("  bool IsRequestRange(uint32_t address, size_t size) const {");
  const std::string helper = def == std::string::npos ? "" : cam.substr(def, 900);
  CHECK(Has(helper, "if (!IsGuestRange(address, size, false)) return false;"));
  CHECK(Has(helper, "if (!IsGuestRange(address, size, true)) {"));
  CHECK(Has(helper, "if (shown.fetch_add(1) < 8)"));
  CHECK(Has(helper, "Virtual Kinect: request {:08X} is on a page that is not writable "));
  // The window: writable while a reply is written, protection put back after.
  const size_t win = cam.find("  class RequestWriteWindow {");
  const std::string window = win == std::string::npos ? "" : cam.substr(win, 1400);
  CHECK(Has(window, "(protection & kMemoryProtectWrite))\n          continue;"));
  CHECK(Has(window, "heap->Protect(uint32_t(at), 1, protection | kMemoryProtectWrite)"));
  CHECK(Has(window, "pages_[n].heap->Protect(pages_[n].address, 1, pages_[n].protection);"));
  // Every reply inside a window.
  CHECK(Count(cam, "RequestWriteWindow window(memory_, request_ptr, GetRequestSize(") == 3);
  const size_t sync = cam.find("RequestWriteWindow window(memory_, request_ptr, GetRequestSize(first_code));");
  CHECK(sync != std::string::npos &&
        cam.find("status = Handle(first_code, request_ptr, bytes);", sync) < sync + 200);
  const size_t async = cam.find("RequestWriteWindow window(memory_, request_ptr, GetRequestSize(code));  // fix 156g\n      status = Handle(code, request_ptr, bytes);");
  CHECK(async != std::string::npos);
  const size_t gen = cam.find("RequestWriteWindow window(memory_, read.request_ptr,");
  CHECK(gen != std::string::npos &&
        cam.find("request->settings_generation = settings_generation_;", gen) < gen + 200);
  // Buffers and counters: still writable.
  CHECK(Has(cam, "!IsGuestRange(buffer, copy_bytes)) {"));
  CHECK(Has(cam, "if (counter_ptr && IsGuestRange(counter_ptr,4)) {"));
  // The title's protection changes, logged.
  CHECK(Has(mem, "const bool protected_ok =\n      heap->Protect(adjusted_base, adjusted_size, protect, &tmp_old_protect);"));
  CHECK(Has(mem, "NtProtectVirtualMemory {:08X}+{:X} (pages {:08X}+{:X}) protect {:X} (was {:X}) "));
  CHECK(Has(mem, "if (shown.fetch_add(1) < 32)"));
  CHECK(Has(mem, "  if (!protected_ok) {\n    return X_STATUS_ACCESS_DENIED;\n  }"));
  if (failures) {
    std::printf("check-request-pages: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-request-pages: ok\n");
  return 0;
}
