// SPDX-License-Identifier: MIT
// Fix 147b (tools/source-1.0.2/fixes/147b-smartglass-title-keys.patch): the title key slots.
// Dance Central 3 stayed on "Loading": it sets a key of its own in slot 0xE0 with XeKeysSetKey
// and decrypts one block with XeKeysAesCbc on that slot; neither export existed ("undefined extern
// call"), so its output buffer was never written.
//  (a) title_keys.hpp: slots 0xE0..0xE7 only, 16-byte keys only, a key read back as set, an unset
//      slot unknown, whole 16-byte blocks for CBC;
//  (b) the core (xboxkrnl_crypt.cc): XeKeysSetKey and XeKeysAesCbc are exports, store and read the
//      slots through the Store, refuse what the Store refuses, run XeCryptAesCbc with the slot's key
//      (a missing feed: a zero IV) and log their first call with "Title keys (fix 147b)".
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/title_keys.hpp"

namespace {
namespace fs = std::filesystem;
namespace tk = xbox360ps5::title_keys;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void Slots() {
  Expect(tk::Slot(0xE0) == 0 && tk::Slot(0xE7) == 7, "(a) 0xE0..0xE7 are slots 0..7");
  Expect(tk::Slot(0xDF) == -1 && tk::Slot(0xE8) == -1 && tk::Slot(0x19) == -1,
         "(a) other key numbers are no title slot");
  tk::Store store;
  uint8_t key[16], back[16] = {};
  for (int n = 0; n < 16; ++n) key[n] = uint8_t(0x10 + n);
  Expect(!store.Get(0xE0, back), "(a) an unset slot is unknown");
  Expect(store.Set(0xE0, key, 16) && store.Get(0xE0, back) && !std::memcmp(key, back, 16),
         "(a) a key reads back as set");
  Expect(!store.Get(0xE1, back), "(a) other slots stay unset");
  Expect(!store.Set(0xE1, key, 15) && !store.Set(0xE1, key, 32) && !store.Set(0xE1, nullptr, 16),
         "(a) a key that is not 16 bytes is refused");
  Expect(!store.Set(0x19, key, 16) && !store.Get(0x19, back), "(a) a console key is not settable");
  key[0] = 0xAA;
  Expect(store.Set(0xE0, key, 16) && store.Get(0xE0, back) && back[0] == 0xAA,
         "(a) a slot can be set again");
  Expect(tk::CbcSize(16) && tk::CbcSize(4096) && !tk::CbcSize(0) && !tk::CbcSize(15) &&
             !tk::CbcSize(24),
         "(a) CBC takes whole 16-byte blocks");
}

void Tree() {
  const std::string crypt = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_crypt.cc");
  Expect(crypt.find("xbox360ps5/title_keys.hpp") != std::string::npos,
         "(b) xboxkrnl_crypt.cc includes title_keys.hpp");
  const size_t set = crypt.find("dword_result_t XeKeysSetKey_entry(");
  const size_t cbc = crypt.find("dword_result_t XeKeysAesCbc_entry(");
  Expect(set != std::string::npos && crypt.find("DECLARE_XBOXKRNL_EXPORT1(XeKeysSetKey,", set) != std::string::npos,
         "(b) XeKeysSetKey is an export");
  Expect(cbc != std::string::npos && crypt.find("DECLARE_XBOXKRNL_EXPORT1(XeKeysAesCbc,", cbc) != std::string::npos,
         "(b) XeKeysAesCbc is an export");
  Expect(set != std::string::npos && crypt.find("title_key_slots.Set(", set) < cbc,
         "(b) XeKeysSetKey stores through the Store");
  Expect(cbc != std::string::npos && crypt.find("title_key_slots.Get(", cbc) != std::string::npos &&
             crypt.find("CbcSize(", cbc) != std::string::npos,
         "(b) XeKeysAesCbc reads the slot and checks the size");
  Expect(cbc != std::string::npos && crypt.find("XeCryptAesCbc_entry(", cbc) != std::string::npos &&
             crypt.find("zero_feed", cbc) != std::string::npos,
         "(b) AES-CBC with the slot's key, a zero IV without a feed");
  Expect(crypt.find("Title keys (fix 147b)", set) != std::string::npos,
         "(b) the first calls are logged with fix 147b");
}

}  // namespace

int main() {
  Slots();
  if (fs::exists(".deps/xenia-canary")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("title keys: all checks passed\n");
  return 0;
}
