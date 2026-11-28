// SPDX-License-Identifier: MIT
// Fix 198 (tools/source-1.0.2/fixes/198-osk-odd-buffer.patch): XamShowKeyboardUI accepts a text
// buffer at an odd address. Culdcept Saga (424107DD) asks for its Cepter's name with the buffer at
// 82C71DD9; 1.0.2's PS5 path refused any odd buffer ("the text buffer is not writable guest
// memory" in fix 114's log, 14 times on 1.0.7_candidate_2), so the keyboard never opened and only
// LT/RT (turning the figure) still did anything.
//  (a) controller_keyboard.h, the real header: WriteBE writes big-endian UTF-16 byte by byte, so an
//      odd start writes exactly capacity * 2 bytes from there (the byte before and the byte after
//      untouched); ValidRange at an odd address checks every 4 KiB page the range touches;
//  (b) the assembled xam_ui.cc, XamShowKeyboardUI's PS5 path: no "& 1" test on the buffer's
//      address; a buffer longer than 65536 characters and one not writable are still refused, each
//      with its own reason; the XOVERLAPPED keeps its 4-byte alignment test.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#include "xenia/ui/controller_keyboard.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace kb = xe::ui::controller_keyboard;
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b - a);
}

void Header() {
  // Culdcept's name: 9 characters and the NUL (10 units) from an odd byte.
  std::vector<uint8_t> memory(32, 0xEE);
  const size_t at = 1, capacity = 10;
  kb::WriteBE(memory.data() + at, capacity, u"Cepter");
  Expect(memory[0] == 0xEE, "(a) WriteBE from an odd byte: the byte before is untouched");
  Expect(memory[at + capacity * 2] == 0xEE, "(a) WriteBE from an odd byte: the byte after the buffer is untouched");
  const char16_t want[] = u"Cepter";
  bool same = true;
  for (size_t i = 0; i < capacity; ++i) {
    const uint16_t c = i < 6 ? uint16_t(want[i]) : 0;
    same = same && memory[at + 2 * i] == uint8_t(c >> 8) && memory[at + 2 * i + 1] == uint8_t(c);
  }
  Expect(same, "(a) WriteBE from an odd byte: big-endian text, zeros to the end");

  std::vector<uint32_t> pages;
  Expect(kb::ValidRange(0x82C71DD9u, 20, [&](uint32_t page) { pages.push_back(page); return true; }) &&
             pages.size() == 1,
         "(a) ValidRange at Culdcept's 82C71DD9 for 20 bytes: one page checked");
  pages.clear();
  Expect(kb::ValidRange(0x82C71FF7u, 20, [&](uint32_t page) { pages.push_back(page); return true; }) &&
             pages.size() == 2,
         "(a) ValidRange from an odd address across a page boundary: both pages checked");
  Expect(!kb::ValidRange(0x82C71FF7u, 20, [](uint32_t page) { return page < 0x82C72000u; }),
         "(a) ValidRange from an odd address: the second page not writable refuses");
}

void Tree() {
  const std::string xam = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_ui.cc");
  Expect(!xam.empty(), "(b) the assembled xam_ui.cc read (run from the tree's root)");
  const std::string entry = Between(xam, "dword_result_t XamShowKeyboardUI_entry(", "const auto read_bounded");
  Expect(!entry.empty() && Has(entry, "#if XE_PLATFORM_PS5"), "(b) XamShowKeyboardUI's PS5 path found");
  Expect(!Has(entry, "buffer.guest_address() & 1"), "(b) no alignment test on the text buffer's address");
  Expect(Has(entry, "if (buffer_length > kMaximumUnits)\n    return refuse(\"the text buffer is longer than 65536 characters\""),
         "(b) a buffer longer than 65536 characters is refused with its own reason");
  Expect(Has(entry, "if (!valid_range(buffer.guest_address(), uint64_t(buffer_length) * 2, kMemoryProtectWrite))\n"
                    "    return refuse(\"the text buffer is not writable guest memory\""),
         "(b) a buffer that is not writable is still refused");
  Expect(Has(entry, "overlapped.guest_address() & 3"), "(b) the XOVERLAPPED keeps its alignment test");
  Expect(Has(entry, "Fix 198"), "(b) fix 198's note is in the code");
}
}  // namespace

int main() {
  Header();
  Tree();
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: the keyboard takes a text buffer at an odd address (Culdcept Saga)\n");
  return 0;
}
