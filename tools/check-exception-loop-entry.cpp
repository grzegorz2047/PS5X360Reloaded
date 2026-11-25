// SPDX-License-Identifier: MIT
// Fix 147 (tools/source-1.0.2/fixes/147-exception-loop-entry.patch): Dance Central 3 enters its game
// loop through a deliberate write to address 0 after installing an unhandled-exception filter; the
// emulator stopped there ("guest crash" right after the logo). exception_loop_entry.hpp finds that
// shape and the null store becomes a call of the filter.
//  (a) Dance Central 3's own code words (373307D9, executable hash 6B15C22CA94CC2E1, at their
//      addresses): one site, call 82311A88, null store 82311A94, filter 82311A40, new word
//      "bl 82311A40";
//  (b) shapes left alone: no null store after the call, a store of 2 or to 4(r0), a branch between
//      the call and the store, a filter that reads r3 first, a filter given in a register, a
//      SetUnhandledExceptionFilter with another body, no executable;
//  (c) the core (kernel_state.cc): the scan runs for the title's executable after the game patches
//      and before Precompile, writes through the heap's protection like the patcher and logs
//      "fix 147" with the addresses.
// Optional: check-exception-loop-entry --image <unpacked executable> [base] scans a real image.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/exception_loop_entry.hpp"

namespace {
namespace fs = std::filesystem;
namespace el = xbox360ps5::exception_loop;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

constexpr uint32_t kBase = 0x82300000;
constexpr uint32_t kSize = 0x00500000;
// 82311A40..82311AA8: the filter 82311A40 and the function that installs it (82311A68).
const uint32_t kDc3Code[] = {
    0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x3D6082D6, 0x806BC794, 0x484EA22D, 0x3D6082D6,
    0x806BC784, 0x4BFFF8D9, 0x00000000, 0x7D8802A6, 0x9181FFF8, 0x9421FFA0, 0x3D4082D6,
    0x7C6B1B78, 0x3D208231, 0x38691A40, 0x916AC784, 0x484EA1F9, 0x3D6082D6, 0x39400001,
    0x91400000, 0x906BC794, 0x38210060, 0x8181FFF8, 0x7D8803A6, 0x4E800020};
// 827FBC80: the C runtime's SetUnhandledExceptionFilter.
const uint32_t kDc3SetFilter[] = {0x3D4082F5, 0x7C6B1B78, 0x806A57C8, 0x916A57C8, 0x4E800020};

struct Image {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kSize, 0);
  void Put(uint32_t address, uint32_t word) {
    uint8_t* p = bytes.data() + (address - kBase);
    p[0] = uint8_t(word >> 24), p[1] = uint8_t(word >> 16), p[2] = uint8_t(word >> 8), p[3] = uint8_t(word);
  }
  el::Code View() const { return {bytes.data(), bytes.size(), kBase}; }
};
Image Dc3() {
  Image image;
  for (size_t n = 0; n < std::size(kDc3Code); ++n) image.Put(0x82311A40 + uint32_t(n) * 4, kDc3Code[n]);
  for (size_t n = 0; n < std::size(kDc3SetFilter); ++n)
    image.Put(0x827FBC80 + uint32_t(n) * 4, kDc3SetFilter[n]);
  return image;
}

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void Dc3Site() {
  const Image image = Dc3();
  Expect(el::IsSetUnhandledExceptionFilter(image.View(), 0x827FBC80),
         "(a) 827FBC80 has SetUnhandledExceptionFilter's body");
  const el::Scan scan = el::Find(image.View());
  Expect(scan.sites.size() == 1, "(a) one site in Dance Central 3's code");
  Expect(scan.refused.empty(), "(a) nothing refused in Dance Central 3's code");
  if (scan.sites.size() != 1) return;
  const el::Site& site = scan.sites[0];
  Expect(site.set_filter == 0x827FBC80, "(a) the filter function is 827FBC80");
  Expect(site.call == 0x82311A88, "(a) the call that installs the filter is 82311A88");
  Expect(site.null_store == 0x82311A94, "(a) the null store is 82311A94");
  Expect(site.filter == 0x82311A40, "(a) the filter is 82311A40");
  Expect(site.old_word == 0x91400000, "(a) the old word is stw r10,0(r0)");
  Expect(site.new_word == 0x4BFFFFAD, "(a) the new word is bl 82311A40");
  uint32_t target = 0;
  Expect(el::RelativeCall(site.null_store, site.new_word, &target) && target == 0x82311A40,
         "(a) the new word calls the filter");
}

void LeftAlone() {
  {
    Image image = Dc3();
    image.Put(0x82311A94, 0x60000000);  // nop instead of the null store
    Expect(el::Find(image.View()).sites.empty(), "(b) no null store after the call: no site");
  }
  {
    Image image = Dc3();
    image.Put(0x82311A90, 0x39400002);  // li r10,2
    Expect(el::Find(image.View()).sites.empty(), "(b) a store of 2: no site");
  }
  {
    Image image = Dc3();
    image.Put(0x82311A94, 0x91400004);  // stw r10,4(r0)
    Expect(el::Find(image.View()).sites.empty(), "(b) a store to 4(r0): no site");
  }
  {
    Image image = Dc3();
    image.Put(0x82311A8C, 0x48000008);  // b over the store
    Expect(el::Find(image.View()).sites.empty(), "(b) a branch between call and store: no site");
  }
  {
    Image image = Dc3();
    image.Put(0x82311A4C, 0x7C7F1B78);  // mr r31,r3 in the filter's prologue
    const el::Scan scan = el::Find(image.View());
    Expect(scan.sites.empty() && scan.refused.size() == 1,
           "(b) a filter that reads r3 first: refused, with a reason");
  }
  {
    Image image = Dc3();
    image.Put(0x82311A80, 0x7D635B78);  // mr r3,r11: the filter is not a constant
    const el::Scan scan = el::Find(image.View());
    Expect(scan.sites.empty() && scan.refused.size() == 1,
           "(b) a filter in a register: refused, with a reason");
  }
  {
    Image image = Dc3();
    image.Put(0x827FBC90, 0x4E800420);  // bctr instead of blr: not the C runtime's function
    Expect(!el::IsSetUnhandledExceptionFilter(image.View(), 0x827FBC80) &&
               el::Find(image.View()).sites.empty(),
           "(b) another body: not SetUnhandledExceptionFilter, no site");
  }
  {
    Image image = Dc3();
    image.Put(0x827FBC88, 0x806A57CC);  // load and store of different globals
    Expect(el::Find(image.View()).sites.empty(), "(b) a swap of two globals: no site");
  }
  Expect(el::Find(el::Code{}).sites.empty(), "(b) no executable: no site");
}

void Tree() {
  const std::string core = Read(".deps/xenia-canary/src/xenia/kernel/kernel_state.cc");
  Expect(core.find("xbox360ps5/exception_loop_entry.hpp") != std::string::npos,
         "(c) kernel_state.cc includes exception_loop_entry.hpp");
  const size_t hook = core.find("Xbox360PS5EnterExceptionLoops(");
  Expect(hook != std::string::npos, "(c) kernel_state.cc defines the fix 147 hook");
  const size_t finish = core.find("X_RESULT KernelState::FinishLoadingUserModule(");
  const size_t call = core.find("Xbox360PS5EnterExceptionLoops(memory_", finish);
  const size_t patches = core.find("ApplyPatchesForTitle(", finish);
  const size_t precompile = core.find("Precompile()", finish);
  Expect(finish != std::string::npos && call != std::string::npos && patches < call &&
             call < precompile,
         "(c) the hook runs after the game patches and before Precompile");
  Expect(core.find("is_executable()", hook) != std::string::npos ||
             core.find("GetExecutableModule()", hook) != std::string::npos,
         "(c) only the title's executable is scanned");
  Expect(core.find("kMemoryProtectRead | kMemoryProtectWrite", hook) != std::string::npos,
         "(c) the word is written through the heap's protection");
  Expect(core.find("Exception loop (fix 147)", hook) != std::string::npos,
         "(c) the rewrite is logged with fix 147");
}

int ScanImage(const char* path, uint32_t base) {
  std::ifstream file(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const el::Scan scan = el::Find({bytes.data(), bytes.size(), base});
  for (const el::Site& site : scan.sites)
    std::printf("site: SetUnhandledExceptionFilter %08X, call %08X, null store %08X -> bl %08X (%08X -> %08X)\n",
                site.set_filter, site.call, site.null_store, site.filter, site.old_word, site.new_word);
  for (const std::string& why : scan.refused) std::printf("refused: %s\n", why.c_str());
  std::printf("%zu site(s), %zu refused\n", scan.sites.size(), scan.refused.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && !std::strcmp(argv[1], "--image"))
    return ScanImage(argv[2], argc >= 4 ? uint32_t(std::strtoul(argv[3], nullptr, 16)) : 0x82000000);
  Dc3Site();
  LeftAlone();
  if (fs::exists(".deps/xenia-canary/src/xenia/kernel/kernel_state.cc")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("exception loop entry: all checks passed\n");
  return 0;
}
