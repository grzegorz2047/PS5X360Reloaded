// SPDX-License-Identifier: MIT
// Fix 111 (tools/source-1.0.2/fixes/111-unwatched-lock-free.patch): a request over units that are
// valid, or unwatched and already checked in this part of the ring, is answered without the global
// critical region. A unit left unwatched ("Optimized video memory", fix 36) is never valid, so
// every such request took the lock in SharedMemory::RequestRange to find its invalid pages, only
// for FilterUnwatched to drop those checked in this part of the ring (fix 87). Red Dead Redemption
// on 1.0.5_candidate_64, fix 88's line: caller 0x95fa28 (SharedMemory::RequestRange+0x338 in the
// build's symbols) 13,000 to 22,000 locks a second, 33 to 42 ms/s waiting. Checked:
//  (a) xenia/gpu/unwatched_lock_free.h, the code the console runs, against the locked path's
//      result (TryFindUploadRange's invalid units, then FilterUnwatched dropping the unwatched ones
//      checked in this part): 200,000 random ranges over random valid, unwatched and stamp bits,
//      across 64-unit words: nothing to upload exactly when the locked path uploads nothing; a
//      watched invalid unit, or an unwatched one not checked in this part, takes the locked path;
//  (b) the assembled tree: RequestRange asks it after fix 55's all-valid answer and before the
//      locked search, only for requests that allow unwatched units with the option on (so no
//      request that would pin its units skips the lock), with fix 87's stamps and part counter;
//      FilterUnwatched still stamps and drops as fix 87 made it; the summary counts the answers.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/gpu/unwatched_lock_free.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
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
namespace ulf = xe::gpu::unwatched_lock_free;

bool Bit(const std::vector<uint64_t>& words, uint32_t unit) { return (words[unit >> 6] >> (unit & 63)) & 1; }

// The locked path: the invalid units, then FilterUnwatched (fix 87): unwatched and checked in
// this part -> dropped; anything else -> uploaded (or hashed, then maybe uploaded).
bool LockedPathUploadsNothing(const std::vector<uint64_t>& valid, const std::vector<uint64_t>& unwatched,
                              const std::vector<uint32_t>& stamps, uint32_t chunk, uint32_t first, uint32_t last) {
  for (uint32_t unit = first; unit <= last; ++unit) {
    if (Bit(valid, unit)) continue;
    if (Bit(unwatched, unit) && stamps[unit] == chunk) continue;
    return false;
  }
  return true;
}

void CheckModel() {
  std::mt19937 random(111);
  const uint32_t units = 64 * 16;
  int agree = 0, lock_free = 0;
  for (int round = 0; round < 200000; ++round) {
    std::vector<uint64_t> valid(16), unwatched(16);
    std::vector<uint32_t> stamps(units);
    const uint32_t chunk = 5;
    // Mostly valid or mostly unwatched-and-checked memory, with a few exceptions.
    const int shape = random() % 4;
    for (uint32_t u = 0; u < units; ++u) {
      const uint32_t r = random() % 100;
      bool v = false, w = false;
      uint32_t s = chunk - 1;
      if (shape == 0) v = r < 97;
      else if (shape == 1) { w = r < 98; s = r < 96 ? chunk : chunk - 1; }
      else if (shape == 2) { v = r < 50; w = !v && r < 99; s = chunk; }
      else { v = r < 30; w = r >= 30 && r < 80; s = r % 2 ? chunk : 3; }
      if (v) valid[u >> 6] |= uint64_t(1) << (u & 63);
      if (w) unwatched[u >> 6] |= uint64_t(1) << (u & 63);
      stamps[u] = s;
    }
    const uint32_t first = random() % units;
    const uint32_t last = std::min(units - 1, first + (random() % 3 ? random() % 8 : random() % 300));
    const bool fast = ulf::NothingToUpload(valid.data(), unwatched.data(), stamps.data(), chunk, first, last);
    const bool locked = LockedPathUploadsNothing(valid, unwatched, stamps, chunk, first, last);
    if (fast == locked) ++agree;
    if (fast) ++lock_free;
  }
  std::printf("   %d of 200000 random requests agree with the locked path, %d answered without the lock\n", agree,
              lock_free);
  Expect(agree == 200000, "nothing to upload exactly when the locked path uploads nothing");
  Expect(lock_free > 20000, "the shapes give many lock-free answers");

  // Single cases across a word boundary (units 60..70).
  std::vector<uint64_t> valid(2, 0), unwatched(2, 0);
  std::vector<uint32_t> stamps(128, 9);
  for (uint32_t u = 60; u <= 70; ++u) unwatched[u >> 6] |= uint64_t(1) << (u & 63);
  Expect(ulf::NothingToUpload(valid.data(), unwatched.data(), stamps.data(), 9, 60, 70), "all unwatched and checked");
  stamps[66] = 8;
  Expect(!ulf::NothingToUpload(valid.data(), unwatched.data(), stamps.data(), 9, 60, 70), "one not checked in this part");
  valid[1] |= uint64_t(1) << (66 - 64);
  Expect(ulf::NothingToUpload(valid.data(), unwatched.data(), stamps.data(), 9, 60, 70), "that one valid instead");
  unwatched[0] &= ~(uint64_t(1) << 61);
  Expect(!ulf::NothingToUpload(valid.data(), unwatched.data(), stamps.data(), 9, 60, 70), "a watched invalid unit");
  Expect(ulf::NothingToUpload(valid.data(), unwatched.data(), stamps.data(), 9, 62, 63) &&
             ulf::NothingToUpload(valid.data(), unwatched.data(), stamps.data(), 9, 64, 64),
         "ranges that leave it out");
}

void CheckTree() {
  const std::string sm = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.cc");
  const size_t request = sm.find("bool SharedMemory::RequestRange(uint32_t start, uint32_t length,");
  const size_t all_valid = sm.find("++lock_free_hits_;", request);
  const size_t fast = sm.find("if (allow_unwatched && use_request_cache && UnwatchedPagesEnabled() &&\n"
                              "      xbox360ps5::gpu_diag::lock_free_valid.load(std::memory_order_relaxed) &&\n"
                              "      unwatched_lock_free::NothingToUpload(\n"
                              "          system_page_flags_valid_, unwatched_bits_.data(), unit_chunk_.data(),\n"
                              "          xbox360ps5::frame_cost::command_chunk_epoch, start >> page_size_log2_,\n"
                              "          (start + length - 1) >> page_size_log2_)) {\n"
                              "    ++unwatched_lock_free_;",
                              request);
  const size_t locked = sm.find("auto global_lock = global_critical_region_.Acquire();", request);
  const size_t search = sm.find("TryFindUploadRange(block_first, block_last, page_first, page_last,", request);
  Expect(request != std::string::npos && all_valid != std::string::npos && fast != std::string::npos &&
             locked != std::string::npos && search != std::string::npos && all_valid < fast && fast < locked &&
             locked < search,
         "RequestRange answers after fix 55's all-valid check, before the locked search, for requests allowing "
         "unwatched units with the option on");
  const size_t pin = sm.find("(written_by_gpu || !request_allows_unwatched_)) {\n      PinWatched(valid_page_first, valid_page_last);");
  Expect(pin != std::string::npos, "requests that do not allow unwatched units still pin theirs (never skipped)");
  const size_t filter = sm.find("unsigned int SharedMemory::FilterUnwatched(");
  const size_t chunk = sm.find("const uint32_t chunk = xbox360ps5::frame_cost::command_chunk_epoch;", filter);
  const size_t drop = sm.find("if (UnitBit(unwatched_bits_, unit) && unit_chunk_[unit] == chunk) {\n        upload = false;", chunk);
  const size_t stamp = sm.find("unit_chunk_[unit] = chunk;", drop);
  Expect(filter != std::string::npos && chunk != std::string::npos && drop != std::string::npos && stamp != std::string::npos,
         "FilterUnwatched still drops and stamps the units as fix 87 made it");
  Expect(sm.find("#include \"xenia/gpu/unwatched_lock_free.h\"  // fix 111") != std::string::npos &&
             sm.find("Performance unwatched (fix 111): {} requests answered without the global lock") != std::string::npos,
         "the summary counts the answers");
  const std::string sh = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.h");
  Expect(sh.find("uint64_t unwatched_lock_free_ = 0;  // fix 111") != std::string::npos &&
             sh.find("std::vector<uint32_t> unit_chunk_;") != std::string::npos &&
             sh.find("std::vector<uint64_t> unwatched_bits_;") != std::string::npos,
         "the counter, fix 87's stamps and the unwatched bits");
}

}  // namespace

int main() {
  CheckModel();
  CheckTree();
  if (failures) {
    std::printf("check-unwatched-lock-free: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-unwatched-lock-free: ok\n");
  return 0;
}
