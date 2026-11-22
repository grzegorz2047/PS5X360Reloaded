// SPDX-License-Identifier: MIT
// Fix 133 (xenia/gpu/vulkan/readback_batching.h, vulkan_command_processor.cc): fewer GPU waits for
// readbacks. Modern Warfare 2 on 1.0.5-work-fix130 resolved one ~4 MiB target every frame at
// starts from 1E91E000 to 1E93E000, all ending at 1ED1E000; each start was its own key, none
// steady, so fix 91k waited for almost every one (910-1068 own copies per 30 s, 42-138 ms/s),
// while the CPU read none of them.
//  (a) OwnCopy: a steady key never, an unsteady key not when its family is steady and the CPU
//      has not been seen reading it, and as before with the switch off;
//  (b) a model of 1200 frames with fix 91's presence (readback_slots.h): the moving-start target
//      (its starts spread as in the log), a portrait-like key resolved now and then, and a key
//      of the same family the CPU reads: own copies before and after;
//  (c) WouldBatch: waits within one period between sync points count once;
//  (d) the assembled tree: the cvar (on), the family's presence updated with the key's, OwnCopy
//      in place of fix 91k's rule, the waits noted for batching, the 30 s line.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>

#include "xenia/gpu/vulkan/readback_batching.h"
#include "xenia/gpu/vulkan/readback_slots.h"

namespace {

namespace rb = xe::gpu::readback_batching;
namespace rs = xe::gpu::vulkan::readback_slots;
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

void CheckOwnCopy() {
  Expect(!rb::OwnCopy(true, false, true, true), "a steady key never gets its own copy");
  Expect(rb::OwnCopy(false, false, false, true), "an unsteady key of an unsteady family gets its own copy");
  Expect(!rb::OwnCopy(false, true, false, true), "an unsteady key of a steady family, not read: the late copy");
  Expect(rb::OwnCopy(false, true, true, true), "the same key read by the CPU keeps its own copy");
  Expect(rb::OwnCopy(false, true, false, false), "switched off: fix 91k's rule");
}

struct Key {
  uint64_t last = 0;
  float presence = 0;
};

// Own copies over `frames` frames with fix 91k's rule (by_end false) or fix 133's.
struct Counts {
  int moving = 0, moving_late = 0, portrait = 0, read = 0;
};
Counts Model(bool by_end, int frames) {
  std::mt19937 random(1234);
  // MW2's starts and their shares of the resolves (1E921000 x318, 1E924000 x203, ...).
  const uint32_t end = 0x1ED1E000;
  const uint32_t starts[] = {0x1E921000, 0x1E924000, 0x1E91E000, 0x1E923000, 0x1E920000, 0x1E91F000, 0x1E93E000};
  const int weights[] = {318, 203, 125, 74, 57, 90, 16};
  std::discrete_distribution<int> pick(std::begin(weights), std::end(weights));
  std::unordered_map<uint64_t, Key> keys;
  std::unordered_map<uint64_t, rb::Family> families;
  Counts counts;
  const auto resolve = [&](uint32_t address, uint32_t length, uint64_t frame, bool cpu_reads, int& counter) {
    Key& key = keys[(uint64_t(address) << 32) | length];
    const uint64_t previous = key.last;
    key.last = frame;
    key.presence = rs::Presence(previous, frame, key.presence);
    rb::Family& family = families[rb::FamilyKey(address, length)];
    const uint64_t family_previous = family.last_used_frame;
    family.last_used_frame = frame;
    family.presence = rs::Presence(family_previous, frame, family.presence);
    if (rb::OwnCopy(rs::SteadyPresence(key.presence), rs::SteadyPresence(family.presence), cpu_reads, by_end)) {
      ++counter;
    }
  };
  for (uint64_t frame = 1; frame <= uint64_t(frames); ++frame) {
    const uint32_t start = starts[pick(random)];
    const int moving_before = counts.moving;
    resolve(start, end - start, frame, false, counts.moving);
    if (frame > 600 && counts.moving != moving_before) ++counts.moving_late;
    if (frame % 200 == 0) resolve(0x0C1E0000, 262144, frame, false, counts.portrait);  // now and then
    // A key of the moving target's family that the CPU reads, every tenth frame.
    if (frame % 10 == 0) resolve(0x1E930000, end - 0x1E930000, frame, true, counts.read);
  }
  return counts;
}

void CheckModel() {
  const Counts before = Model(false, 1200), after = Model(true, 1200);
  std::printf("1200 frames: own copies of the moving-start target %d -> %d (after frame 600: %d -> %d), of the "
              "occasional key %d -> %d, of the read key %d -> %d\n",
              before.moving, after.moving, before.moving_late, after.moving_late, before.portrait, after.portrait,
              before.read, after.read);
  Expect(before.moving_late > 500, "fix 91k: most of the moving target's resolves wait");
  Expect(after.moving < 300 && after.moving_late == 0,
         "fix 133: only until the family is steady (about 260 frames of fix 91's presence), then none");
  Expect(after.portrait == before.portrait && after.portrait == 6, "an occasional key keeps its own copies");
  Expect(after.read == before.read && after.read == 120, "a key the CPU reads keeps its own copies");
}

void CheckBatch() {
  rb::WouldBatch batch;
  rb::SyncPoint a{10, 3, 1}, b{11, 3, 1}, c{11, 4, 1}, d{11, 4, 2};
  for (const rb::SyncPoint& at : {a, a, a, b, b, c, d, d}) batch.Note(at);
  Expect(batch.waits() == 8 && batch.periods() == 4, "waits within one period count once (" +
                                                          std::to_string(batch.periods()) + " periods)");
  batch.Reset();
  batch.Note(d);
  Expect(batch.waits() == 1 && batch.periods() == 0,
         "after a reset, a wait in the same period as before is not a new period");
}

void CheckTree() {
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string vh = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  Expect(vk.find("DEFINE_bool(readback_steady_by_end, true,") != std::string::npos, "the switch, on");
  Expect(vh.find("#include \"xenia/gpu/vulkan/readback_batching.h\"  // fix 133") != std::string::npos &&
             vh.find("std::unordered_map<uint64_t, readback_batching::Family> readback_families_;") != std::string::npos,
         "the families and counters");
  const size_t presence = vk.find("rb.presence = readback_slots::Presence(previous_use_frame, frame_current_, rb.presence);");
  const size_t family = vk.find("readback_families_[readback_batching::FamilyKey(written_address, written_length)];");
  const size_t own = vk.find("readback_batching::OwnCopy(key_steady, family_steady, rb.cpu_reads,");
  Expect(presence != std::string::npos && family > presence && own > family,
         "the family's presence with the key's, then OwnCopy in place of fix 91k's rule");
  Expect(vk.find("? !readback_slots::SteadyPresence(rb.presence)\n") == std::string::npos, "fix 91k's rule replaced");
  Expect(vk.find("readback_own_batches_.Note(ReadbackSyncPoint());") != std::string::npos &&
             vk.find("readback_memexport_batches_.Note(ReadbackSyncPoint());  // fix 133\n        if "
                     "(AwaitAllQueueOperationsCompletion()) {") != std::string::npos,
         "the own copies' and memory exports' waits noted");
  Expect(vk.find("\"Readback batching, last 30 s (fix 133, steady by end {}): ") != std::string::npos,
         "the 30 s line");
  Expect(vk.find("point.ring_part = xbox360ps5::frame_cost::command_chunk_epoch;") != std::string::npos &&
             vk.find("ct::Kind::kMemWrite, ct::Kind::kEventShd, ct::Kind::kCondWrite,") != std::string::npos,
         "the sync point: the command processor's writes, the part of the ring, the swaps");
}

}  // namespace

int main(int argc, char** argv) {
  CheckOwnCopy();
  CheckModel();
  CheckBatch();
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-readback-batching: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-batching: ok\n");
  return 0;
}
