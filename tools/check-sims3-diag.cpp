// SPDX-License-Identifier: MIT
// Fix 142h (research/parallel-gpu-commands/STAGE2.md, section 16): a diagnostic build for The Sims
// 3's resolve waits. Nothing changes unless content-checked.txt is there.
//  (a) content-checked.txt in the title folder sets fix 125 (gpu_content_checked_units) for the
//      whole run: read in SharedMemory::InitializeCommon, before any unit can be checked, and
//      logged; fix 125's own test of the setting is unchanged;
//  (b) the big resolve readback keys (1 MiB or more) are counted where fix 91k and fix 133 decide
//      on a copy, with the same conditions as the decision, and logged every 30 s after fix 133's
//      line, then forgotten.
// Runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
bool Has(const std::string& text, const std::string& part) {
  const bool found = text.find(part) != std::string::npos;
  if (!found) std::printf("  (not found: %s)\n", part.substr(0, 100).c_str());
  return found;
}

void CheckContentChecked() {
  const std::string sm = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.cc");
  Expect(!sm.empty(), "shared_memory.cc is there (run from the assembled tree's root)");
  const size_t init = sm.find("bool SharedMemory::InitializeCommon() {");
  const size_t read = sm.find("XBOX360PS5_DATA_ROOT \"/content-checked.txt\"");
  const size_t init_end = sm.find("\n}\n", init);
  Expect(init != std::string::npos && read > init && read < init_end,
         "content-checked.txt is read in InitializeCommon, at the start of the run");
  Expect(Has(sm, "if (word == \"1\" || word == \"on\" || word == \"0\" || word == \"off\") {") &&
             Has(sm, "cvars::gpu_content_checked_units = word == \"1\" || word == \"on\";"),
         "on/1 and off/0 set fix 125's setting; anything else keeps it");
  Expect(Has(sm, "return UnwatchedPagesEnabled() && cvars::gpu_content_checked_units;"),
         "fix 125's own test of the setting is unchanged");
  Expect(sm.find("content-checked.txt", read + 10) == std::string::npos ||
             sm.find("content-checked.txt", read + 10) < init_end,
         "read nowhere else (not while the game runs)");
}

void CheckBigKeys() {
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const size_t own_copy = cp.find("const bool own_copy =");
  const size_t big = cp.find("if (readback_length >= (UINT32_C(1) << 20)) {  // fix 142h");
  const size_t late = cp.find("++readback_family_late_;");
  Expect(own_copy != std::string::npos && big > own_copy && big < late,
         "the big keys are counted right after fix 91k's own-copy decision");
  const std::string block = cp.substr(big, late - big);
  Expect(Has(block, "big.own += fast && !auto_readback && own_copy && newest_copy >= 0;") &&
             Has(block, "big.no_earlier += fast && newest_copy < 0;") &&
             Has(block, "big.family_late += g_readback_rules.occasional_own && !key_steady && !own_copy && fast &&"),
         "the counts use the decision's own conditions");
  const size_t batching = cp.find("XELOGW(\"Readback batching, last 30 s (fix 133");
  const size_t line = cp.find("XELOGW(\"Readback big keys, last 30 s (fix 142h, 1 MiB or more): {}\"");
  Expect(batching != std::string::npos && line > batching && cp.find("fix142h::big_keys.clear();", line) != std::string::npos,
         "the line after fix 133's, every 30 s, the keys forgotten after it");
}

}  // namespace

int main() {
  CheckContentChecked();
  CheckBigKeys();
  if (failures) {
    std::printf("check-sims3-diag: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-sims3-diag: ok\n");
  return 0;
}
