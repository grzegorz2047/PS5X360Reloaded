// SPDX-License-Identifier: MIT
// Fix 87 (tools/source-1.0.2/fixes/87-unwatched-chunk.patch): with "Optimized video memory"
// (memory_boost) on, vertex data the guest rewrites every frame is left unwatched (fix 36,
// SharedMemory::FilterUnwatched): at each request the whole unit is hashed and uploaded when it
// changed. Modern Warfare 2 on 1.0.5_candidate_61 with memory_boost on: the draws' vertex part went
// from 52 to 368 ms/s, a hash of the unit at every draw. Every draw in a part of the ring the guest
// has published (ExecutePrimaryBuffer) was written, with the data it reads, before the write
// pointer was; so an unwatched unit checked once in that part needs no second hash for its other
// draws there. Checked:
//  (a) a model of the per-part check: hashes once per unit and part, uploads what changed since
//      the previous part, and a write made after the part was published but before its second
//      draw is not the second draw's (it belongs to a later part);
//  (b) the assembled tree: ExecutePrimaryBuffer starts a part before its first packet (on the
//      console; a counter in frame_cost.hpp, since the base command processor compiles that code too), FilterUnwatched answers a unit checked in this part before hashing it and stamps
//      a unit when it checks it, the stamps are sized with the other per-unit arrays, and the
//      summary counts the answers.
// Run from the assembled tree's root (run-fix-checks.sh).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
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

// The filter for unwatched units, as FilterUnwatched does it with fix 87.
struct Model {
  std::map<uint32_t, uint64_t> guest, gpu, hashed;  // guest bytes, GPU copy, hash at the last upload
  std::map<uint32_t, uint32_t> chunk_of;
  uint32_t chunk = 1;
  int hashes = 0, uploads = 0, skips = 0;
  void BeginChunk() { ++chunk; }
  void Request(uint32_t unit) {
    if (chunk_of[unit] == chunk) {
      ++skips;
      return;
    }
    chunk_of[unit] = chunk;
    ++hashes;
    if (hashed.count(unit) && hashed[unit] == guest[unit]) return;
    hashed[unit] = guest[unit];
    gpu[unit] = guest[unit];
    ++uploads;
  }
};

void CheckModel() {
  Model m;
  // A part with 1,000 draws reading unit 7 (and 3 every 10th): one hash each.
  m.guest[7] = 1;
  m.guest[3] = 5;
  m.BeginChunk();
  for (int d = 0; d < 1000; ++d) {
    m.Request(7);
    if (d % 10 == 0) m.Request(3);
  }
  Expect(m.hashes == 2 && m.uploads == 2 && m.skips == 999 + 99, "one hash per unit and part (" + std::to_string(m.hashes) + ")");
  // The guest rewrites unit 7 for the next frame while this part is processed: data for later
  // draws, which are in a later part. The draws in this part read what was there before.
  m.guest[7] = 2;
  m.Request(7);
  Expect(m.gpu[7] == 1, "a write after the part was published is not this part's");
  // The next part checks again and uploads the change; an unchanged unit is not uploaded.
  m.BeginChunk();
  m.Request(7);
  m.Request(3);
  Expect(m.gpu[7] == 2 && m.uploads == 3 && m.hashes == 4, "the next part uploads what changed, once");
}

void CheckTree() {
  const std::string G = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string pm4 = Read(G + "pm4_command_processor_implement.h");
  const size_t fn = pm4.find("uint32_t COMMAND_PROCESSOR::ExecutePrimaryBuffer(uint32_t read_index,");
  const size_t start = pm4.find("#if XE_PLATFORM_PS5\n  ++xbox360ps5::frame_cost::command_chunk_epoch;  // fix 87: a new part of the ring\n#endif", fn);
  const size_t first = pm4.find("if (!COMMAND_PROCESSOR::ExecutePacket()) {", fn);
  Expect(fn != std::string::npos && start != std::string::npos && start < first, "a part starts before its first packet, on the console");
  const std::string sm = Read(G + "shared_memory.cc");
  const size_t filter = sm.find("unsigned int SharedMemory::FilterUnwatched(");
  const size_t skip = sm.find("if (UnitBit(unwatched_bits_, unit) && unit_chunk_[unit] == chunk) {", filter);
  const size_t stamp = sm.find("unit_chunk_[unit] = chunk;", skip);
  const size_t hash = sm.find("const uint64_t hash = XXH3_64bits(", skip);
  Expect(filter != std::string::npos && skip != std::string::npos && stamp < hash && hash != std::string::npos,
         "FilterUnwatched answers a unit checked in this part before hashing, and stamps a unit it checks");
  Expect(sm.find("unit_chunk_.assign(kBufferSize >> page_size_log2_, 0);  // fix 87") != std::string::npos,
         "a stamp for every unit");
  Expect(sm.find("Performance unwatched (fix 87): {} requests answered by an earlier check in the same part of the ring") !=
             std::string::npos,
         "the summary counts them");
  const std::string fc = Read("include/xbox360ps5/frame_cost.hpp");
  Expect(fc.find("inline uint32_t command_chunk_epoch = 1;") != std::string::npos &&
             sm.find("const uint32_t chunk = xbox360ps5::frame_cost::command_chunk_epoch;", filter) < skip,
         "the parts counted where both the command processor and shared memory see them");
}

}  // namespace

int main() {
  CheckModel();
  CheckTree();
  if (failures) {
    std::printf("check-unwatched-chunk: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-unwatched-chunk: ok\n");
  return 0;
}
