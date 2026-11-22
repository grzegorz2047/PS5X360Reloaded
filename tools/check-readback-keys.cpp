// SPDX-License-Identifier: MIT
// Fix 91 diagnostic (issue 14): the resolve readback mode per key from readback-keys.txt
// (include/xbox360ps5/readback_keys.hpp) and the keys in the log.
//
//   check-readback-keys        run from an assembled tree's root
//
// The file's lines (default, address, address/length, comments, nonsense), which line wins, and
// the command processor's use: read again when the file changes, only the fast and full settings
// changed (never readback off or its automatic copy), the keys logged.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/readback_keys.hpp"

namespace rk = xbox360ps5::readback_keys;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main() {
  // Nothing in the file: every key as set.
  {
    const rk::Rules none = rk::Parse("");
    CHECK(none.default_mode == rk::Mode::kAsSet && none.keys.empty() && none.not_understood == 0);
    CHECK(rk::Lookup(none, 0x0FE7C000, 245760) == rk::Mode::kAsSet);
  }
  // One key full (the first bisect step), the rest as set.
  {
    const rk::Rules one = rk::Parse("0FE7C000 full\n");
    CHECK(rk::Lookup(one, 0x0FE7C000, 245760) == rk::Mode::kFull);
    CHECK(rk::Lookup(one, 0x0FE7C000, 4096) == rk::Mode::kFull);  // any length
    CHECK(rk::Lookup(one, 0x0E338000, 4227072) == rk::Mode::kAsSet);
  }
  // "All but one": default full, one key fast; a length line wins over the address line.
  {
    const rk::Rules rules = rk::Parse(
        "# Tekken, step 3\r\n"
        "default full\n"
        "0E338000 fast   # the 4 MiB one\n"
        "0x0FD7C000/4096 fast\n"
        "0FD7C000 full\n"
        "10020000/0x1000 fast\n"
        "\n"
        "   \t\n");
    CHECK(rules.default_mode == rk::Mode::kFull && rules.keys.size() == 4 && rules.not_understood == 0);
    CHECK(rk::Lookup(rules, 0x0E338000, 4227072) == rk::Mode::kFast);
    CHECK(rk::Lookup(rules, 0x0FD7C000, 4096) == rk::Mode::kFast);
    CHECK(rk::Lookup(rules, 0x0FD7C000, 65536) == rk::Mode::kFull);
    CHECK(rk::Lookup(rules, 0x10020000, 4096) == rk::Mode::kFast);
    CHECK(rk::Lookup(rules, 0x10020000, 16384) == rk::Mode::kFull);  // the default
    CHECK(rk::Lookup(rules, 0x0D0F0000, 3768320) == rk::Mode::kFull);
  }
  // Lines that are not rules are counted and ignored.
  {
    const rk::Rules bad = rk::Parse("0FE7C000\nzzz full\n0FE7C000 slow\n0FE7C000/0 full\n0FE7C000 full extra\n"
                                    "default\n1FFFFFFFF full\n0D0F0000/3768320 fast\n");
    CHECK(bad.not_understood == 7 && bad.keys.size() == 1);
    CHECK(rk::Lookup(bad, 0x0D0F0000, 3768320) == rk::Mode::kFast);
  }
  CHECK(std::string(rk::Name(rk::Mode::kFull)) == "full" && std::string(rk::Name(rk::Mode::kFast)) == "fast");
  // 1.0.5-work-fix91g: the read watch forced on keys, and the simpler rule switched on.
  {
    const rk::Rules rules = rk::Parse("watch 0F05C000\nwatch 0x0E338000\noccasional own\ndefault fast\n");
    CHECK(rules.not_understood == 0 && rules.watch.size() == 2 && rules.occasional_own &&
          rules.default_mode == rk::Mode::kFast && rules.keys.empty());
    CHECK(rk::Watched(rules, 0x0F05C000) && rk::Watched(rules, 0x0E338000) && !rk::Watched(rules, 0x10020000));
    // 1.0.5-work-fix91k: the rule is the default; "occasional off" goes back to fix 91i's.
    CHECK(!rk::Parse("occasional off\n").occasional_own && rk::Parse("").occasional_own &&
          rk::Rules().occasional_own);
    const rk::Rules bad = rk::Parse("watch\nwatch zz\nwatch 0F05C000 full\noccasional\noccasional maybe\n");
    CHECK(bad.not_understood == 5 && bad.watch.empty() && bad.occasional_own);  // left as default
  }
  // 1.0.5-work-fix91h: fast copies kept out of guest memory, for one key or for every key the
  // CPU is not seen reading.
  {
    const rk::Rules one = rk::Parse("nocopy 0F05C000\n");
    CHECK(one.not_understood == 0 && one.nocopy.size() == 1 && !one.nocopy_unread);
    CHECK(rk::NoCopy(one, 0x0F05C000, false) && rk::NoCopy(one, 0x0F05C000, true));  // listed: always
    CHECK(!rk::NoCopy(one, 0x100AD000, false));
    const rk::Rules unread = rk::Parse("nocopy unread\n");
    CHECK(unread.nocopy_unread && unread.nocopy.empty());
    CHECK(rk::NoCopy(unread, 0x0F05C000, false) && !rk::NoCopy(unread, 0x100AD000, true));  // read: copied
    CHECK(!rk::NoCopy(rk::Parse(""), 0x0F05C000, false));
    const rk::Rules bad = rk::Parse("nocopy\nnocopy zz\nnocopy 0F05C000 full\n");
    CHECK(bad.not_understood == 3 && bad.nocopy.empty() && !bad.nocopy_unread);
  }

  const std::string cp = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  if (cp.empty()) {
    std::printf("no assembled tree here; the command processor not checked\n");
  } else {
    CHECK(cp.find("#include \"xbox360ps5/readback_keys.hpp\"  // fix 91 diagnostic") != std::string::npos);
    // The file's places, read again when it changes.
    CHECK(cp.find("XBOX360PS5_DATA_ROOT \"/readback-keys.txt\"") != std::string::npos);
    CHECK(cp.find("\"/download0/xbox360ps5/readback-keys.txt\"") != std::string::npos);
    CHECK(cp.find("if (found != loaded_from || stamp != loaded_stamp) {") != std::string::npos);
    // Only the fast and full settings, never the automatic copy.
    CHECK(cp.find("if (!automatic && length &&\n      (mode == ReadbackResolveMode::kFast || mode == "
                  "ReadbackResolveMode::kFull)) {") != std::string::npos);
    // Called with the resolve's key before the mode is used.
    const size_t call = cp.find("ReadbackKeysDiagnostic(written_address, written_length,");
    const size_t auto_set = cp.find("readback_mode = ReadbackResolveMode::kFast;\n  }");
    const size_t used = cp.find("if (readback_mode == ReadbackResolveMode::kDisabled || !written_length) {");
    CHECK(auto_set != std::string::npos && auto_set < call && call < used && used != std::string::npos);
    CHECK(cp.find("first seen (fix 91") != std::string::npos);
    CHECK(cp.find("\"Readback keys, last 30 s: {}\"") != std::string::npos);
    // The switches reach the command processor (fix 91g).
    CHECK(cp.find("xbox360ps5::readback_keys::Rules g_readback_rules;") != std::string::npos);
    CHECK(cp.find("xbox360ps5::readback_keys::Watched(g_readback_rules, written_address);  // fix 91g") !=
          std::string::npos);
    // Fix 91j/k's rule, or fix 133's in its place (a key's family by its end address).
    CHECK(cp.find("? !readback_slots::SteadyPresence(rb.presence)") != std::string::npos ||
          (cp.find("const bool key_steady = readback_slots::SteadyPresence(rb.presence);") != std::string::npos &&
           cp.find("? readback_batching::OwnCopy(key_steady, family_steady, rb.cpu_reads,") != std::string::npos));
    // Fix 91h: the copy left out only in the fast mode (never the automatic copy), counted, and
    // the read watch still armed after it.
    const size_t leave = cp.find("xbox360ps5::readback_keys::NoCopy(g_readback_rules, written_address, rb.cpu_reads);");
    const size_t copy = cp.find("memory::vastcpy(memory_->TranslatePhysical(written_address),");
    const size_t arm = cp.find("memory_->EnablePhysicalMemoryAccessCallbacks(written_address, readback_length, false,");
    CHECK(leave != std::string::npos && leave < copy && copy < arm && arm != std::string::npos);
    CHECK(cp.find("readback_mode == ReadbackResolveMode::kFast && !auto_readback &&\n"
                  "            xbox360ps5::readback_keys::NoCopy(") != std::string::npos);
    CHECK(cp.find("++readback_copies_left_out_;") != std::string::npos);
    CHECK(cp.find("fast copies kept out of guest memory {} \"\n            \"(fix 91g/h/i)\"") !=
          std::string::npos);
    // Fix 91i: the reads that were a write first, in the 30 s line.
    CHECK(cp.find("xbox360ps5::read_watch_writes.exchange(0), readback_keys_flagged_,") != std::string::npos);
    std::printf("tree: the readback mode per key from readback-keys.txt, and the keys logged\n");
  }
  // Fix 91i: a write to a read-watched page reaches the read callbacks before the write path
  // clears the read watch (1.0.5-work-fix91g's watch never saw Tekken's costume target).
  const std::string mem = ReadText(".deps/xenia-canary/src/xenia/memory.cc");
  if (mem.empty()) {
    std::printf("no assembled tree here; the memory's write path not checked\n");
  } else {
    const size_t write_path = mem.find("  // Check if watching any page, whether need to call the callback at all.");
    const size_t told = mem.find("xbox360ps5::read_watch_writes.fetch_add(1, std::memory_order_relaxed);");
    const size_t invalidation = mem.find("       memory_->physical_memory_invalidation_callbacks_) {", told);
    const size_t cleared = mem.find("    system_page_flags_[i].notify_on_read &= mask;\n  }\n\n  return true;");
    CHECK(write_path != std::string::npos && told != std::string::npos && write_path < told);
    CHECK(invalidation != std::string::npos && told < invalidation && invalidation < cleared &&
          cleared != std::string::npos);
    CHECK(mem.find("[&](uint32_t block) { return system_page_flags_[block].notify_on_read; },\n"
                   "          system_page_first, system_page_last)) {") != std::string::npos);
    CHECK(mem.find("std::atomic<uint32_t> read_watch_writes{0};") != std::string::npos);
    std::printf("tree: a write to a read-watched page reaches the read callbacks\n");
  }
  // Fix 91i: AnyPage over 64-page blocks, its first and last block masked.
  {
    uint64_t blocks[3] = {0, 0, 0};
    auto bits = [&](uint32_t block) { return blocks[block]; };
    CHECK(!rk::AnyPage(bits, 0, 191));
    blocks[1] = uint64_t(1) << 5;  // page 69
    CHECK(rk::AnyPage(bits, 69, 69) && rk::AnyPage(bits, 0, 191) && rk::AnyPage(bits, 60, 70));
    CHECK(!rk::AnyPage(bits, 70, 191) && !rk::AnyPage(bits, 0, 68) && !rk::AnyPage(bits, 64, 68));
    blocks[1] = 0;
    blocks[2] = uint64_t(1) << 63;  // page 191, the last bit of a block
    CHECK(rk::AnyPage(bits, 191, 191) && rk::AnyPage(bits, 128, 191) && !rk::AnyPage(bits, 128, 190));
    blocks[2] = 0;
    blocks[0] = 1;  // page 0
    CHECK(rk::AnyPage(bits, 0, 0) && !rk::AnyPage(bits, 1, 191));
    CHECK(!rk::AnyPage(bits, 5, 4));  // an empty range
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-keys: all passed\n");
  return 0;
}
