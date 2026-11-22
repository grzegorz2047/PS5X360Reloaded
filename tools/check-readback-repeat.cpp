// SPDX-License-Identifier: MIT
// Fix 91 (tools/source-1.0.2/fixes/91-readback-repeat.patch): the fast resolve readback gives a
// resolve the previous frame's copy of its own pass. Tekken Tag Tournament 2 (issue 14) resolves
// several keys twice a frame through its fights (two passes drawn into the same place); the fast
// mode gave each pass the copy one resolve late, the other pass's, and King's and Bob's clothes
// came out cyan and blue, right with the full mode.
//
// Drives xe::gpu::vulkan::readback_slots (the assembled core's readback_slots.h) as the fast
// mode in vulkan_command_processor.cc does, each copy labelled with its frame and pass:
//  * twice a frame: pass A gets the previous frame's A, pass B the previous B (before: A got
//    the previous B, B the same frame's A); each pass's first frame gets its own copy;
//  * three passes a frame fit the four slots the same way; five degrade to own copies (never
//    another pass's copy);
//  * once a frame, every other frame and a few times at load: exactly what fix 35 gave;
//  * and the assembled vulkan_command_processor.cc counts the pass, stamps and clears it, and
//    asks the pass-aware choices.
// Built against the assembled tree and run from its root (run-fix-checks.sh).
#include "xenia/gpu/vulkan/readback_slots.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace rs = xe::gpu::vulkan::readback_slots;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// A copy's label: frame * 10 + pass.
constexpr int Label(uint64_t frame, uint32_t pass) { return int(frame) * 10 + int(pass); }

// One key in fast mode (the GPU keeping up). Resolve() returns the label of the copy the guest
// gets; `fix91` false runs fix 35's choices instead.
struct Key {
  uint64_t submissions[rs::kCount] = {};
  uint32_t valid[rs::kCount] = {};
  uint32_t passes[rs::kCount] = {};
  uint64_t frames[rs::kCount] = {};
  int label[rs::kCount] = {};
  uint64_t submission = 0, pass_frame = 0;
  uint32_t pass = 0;
  bool fix91 = true;
  int Resolve(uint64_t frame, uint32_t length = 4096) {
    if (pass_frame != frame) {
      pass_frame = frame;
      pass = 0;
    }
    const uint32_t this_pass = ++pass;
    ++submission;
    const uint64_t completed = submission - 1;
    uint32_t write;
    int newest;
    if (fix91) {
      write = rs::ChooseWrite(submissions, valid, passes, rs::kCount, length);
      newest = rs::ChooseRead(submissions, valid, passes, frames, rs::kCount, int(write), length, this_pass,
                              frame, completed, false);
    } else {
      write = rs::ChooseWrite(submissions, valid, rs::kCount, length, completed);
      newest = rs::ChooseRead(submissions, valid, rs::kCount, int(write), length, completed, false);
    }
    submissions[write] = submission;
    valid[write] = length;
    passes[write] = this_pass;
    frames[write] = frame;
    label[write] = Label(frame, this_pass);
    return newest < 0 ? Label(frame, this_pass) : label[newest];  // a miss: its own copy
  }
};

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main() {
  // Twice a frame: each pass the previous frame's copy of itself.
  {
    Key fixed, before;
    before.fix91 = false;
    CHECK(fixed.Resolve(1) == Label(1, 1) && fixed.Resolve(1) == Label(1, 2));  // first frame: own
    for (uint64_t f = 2; f <= 40; ++f) {
      CHECK(fixed.Resolve(f) == Label(f - 1, 1));
      CHECK(fixed.Resolve(f) == Label(f - 1, 2));
    }
    before.Resolve(1);
    before.Resolve(1);
    for (uint64_t f = 2; f <= 10; ++f) {
      CHECK(before.Resolve(f) == Label(f - 1, 2));  // the bug: A got the previous B
      CHECK(before.Resolve(f) == Label(f, 1));      // and B the same frame's A
    }
  }
  // Three passes a frame fit the four slots.
  {
    Key k;
    for (uint32_t p = 1; p <= 3; ++p) CHECK(k.Resolve(1) == Label(1, p));
    for (uint64_t f = 2; f <= 30; ++f)
      for (uint32_t p = 1; p <= 3; ++p) CHECK(k.Resolve(f) == Label(f - 1, p));
  }
  // Five passes (more than the slots): some get their own copy, none another pass's.
  {
    Key k;
    int own = 0;
    for (uint64_t f = 1; f <= 30; ++f)
      for (uint32_t p = 1; p <= 5; ++p) {
        const int given = k.Resolve(f);
        CHECK(given % 10 == int(p));
        CHECK(given == Label(f, p) || given / 10 < int(f));
        own += given == Label(f, p);
      }
    CHECK(own > 0);
  }
  // Once a frame, every other frame, and Tekken's costume key at load (one resolve a frame,
  // frames apart): exactly what fix 35 gave.
  {
    for (uint64_t step : {1u, 2u, 3u}) {
      Key fixed, before;
      before.fix91 = false;
      for (uint64_t f = 100; f <= 160; f += step) CHECK(fixed.Resolve(f, 245760) == before.Resolve(f, 245760));
    }
  }

  // 1.0.5-work-fix91f: occasional keys (Tekken's costume target 0F05C000, about every 25 frames)
  // against keys resolved every frame or twice a frame.
  {
    float average = 0;
    CHECK(rs::Occasional(0, 100, average) && average == rs::kOccasionalFrames);  // new: own copy
    bool occasional = true;
    for (uint64_t frame = 125; frame <= 400; frame += 25) occasional = rs::Occasional(frame - 25, frame, average);
    CHECK(occasional && average > 20);
    float every = 0;
    rs::Occasional(0, 1, every);
    for (uint64_t frame = 2; frame < 10; ++frame) occasional = rs::Occasional(frame - 1, frame, every);
    CHECK(!occasional && every < 2);
    float twice = 0;
    rs::Occasional(0, 1, twice);
    for (uint64_t frame = 1; frame < 20; ++frame) {
      rs::Occasional(frame, frame, twice);                 // the second pass, the same frame
      occasional = rs::Occasional(frame, frame + 1, twice);  // the next frame's first
    }
    CHECK(!occasional && twice < 1);
    // A burst at load (a resolve a frame) then every 25 frames: occasional again within a few.
    float burst = 0;
    rs::Occasional(0, 1, burst);
    for (uint64_t frame = 2; frame <= 10; ++frame) rs::Occasional(frame - 1, frame, burst);
    CHECK(burst < rs::kOccasionalFrames);
    int resolves = 0;
    for (uint64_t frame = 35; !rs::Occasional(frame - 25, frame, burst); frame += 25) ++resolves;
    CHECK(resolves <= 1);
  }
  // 1.0.5-work-fix91j: steady keys. A run of a resolve a frame (Tekken's costume parts at a
  // match's load) is not steady until kSteadyFrames frames; repeats within a frame keep the run;
  // a skipped frame restarts it. A key resolved every frame for long (HDR luminance) is steady.
  {
    CHECK(rs::Streak(0, 100, 0) == 1);
    uint32_t streak = rs::Streak(0, 1, 0);
    for (uint64_t frame = 2; frame <= 40; ++frame) streak = rs::Streak(frame - 1, frame, streak);
    CHECK(streak == 40 && !rs::Steady(streak));  // a 40-frame run at load: its own copies
    CHECK(rs::Streak(40, 40, streak) == 40);      // a second pass in frame 40
    CHECK(rs::Streak(40, 42, streak) == 1);       // frame 41 had none
    uint32_t every = rs::Streak(0, 1, 0);
    for (uint64_t frame = 2; frame <= 200; ++frame) {
      every = rs::Streak(frame - 1, frame, every);
      every = rs::Streak(frame, frame, every);  // two passes a frame (MW2's multi-pass keys)
    }
    CHECK(rs::Steady(every) && every == 200);
    CHECK(rs::Streak(25, 50, 7) == 1 && !rs::Steady(1));  // every 25 frames: never steady
    CHECK(rs::Streak(50, 49, 9) == 1);                    // a frame counter that went back
    CHECK(rs::Steady(rs::kSteadyFrames) && !rs::Steady(rs::kSteadyFrames - 1));
  }
  // 1.0.5-work-fix91k: steady as a share of recent frames. FIFA Street's frame-size targets,
  // resolved in about 3 of 4 frames (35/s at 48 fps on 91j), become steady within some 10 s; a key
  // every other frame too; a run of costume parts at load stays unsteady for 200 frames in a row;
  // a key every 25 frames never; repeats within a frame count once.
  {
    float p = rs::Presence(0, 1, 0);
    uint64_t last = 1;
    CHECK(p == rs::kPresenceRate && !rs::SteadyPresence(p));
    for (uint64_t frame = 2; frame <= 200; ++frame) {
      p = rs::Presence(last, frame, p);
      p = rs::Presence(frame, frame, p);  // a second resolve in the frame: counted once
      last = frame;
    }
    CHECK(!rs::SteadyPresence(p) && p > 0.3f);  // 200 frames in a row: still its own copies
    for (uint64_t frame = 201; frame <= 600; ++frame) {
      p = rs::Presence(last, frame, p);
      last = frame;
    }
    CHECK(rs::SteadyPresence(p) && p > 0.65f);  // a key every frame for 10 s: steady
    float three_of_four = rs::Presence(0, 1, 0);
    last = 1;
    for (uint64_t frame = 2; frame <= 600; ++frame) {
      if (frame % 4 == 0) continue;
      three_of_four = rs::Presence(last, frame, three_of_four);
      last = frame;
    }
    CHECK(rs::SteadyPresence(three_of_four));
    float other = rs::Presence(0, 2, 0);
    last = 2;
    for (uint64_t frame = 4; frame <= 4000; frame += 2) {
      other = rs::Presence(last, frame, other);
      last = frame;
    }
    CHECK(rs::SteadyPresence(other) && other > 0.45f && other < 0.55f);  // every other frame: ~0.5
    float rare = rs::Presence(0, 25, 0);
    last = 25;
    for (uint64_t frame = 50; frame <= 25000; frame += 25) {
      rare = rs::Presence(last, frame, rare);
      last = frame;
    }
    CHECK(!rs::SteadyPresence(rare) && rare < 0.06f);  // every 25 frames: never steady
    // After a steady key stops for 10 s it is unsteady again (a new run at the next load).
    CHECK(!rs::SteadyPresence(rs::Presence(1000, 1600, 0.9f)));
    CHECK(rs::Presence(50, 40, 0.7f) == rs::kPresenceRate);  // a frame counter that went back
  }
  CHECK(rs::Overlaps(0x0F05C000, 4194304, 0x0F05D000, 4096) && !rs::Overlaps(0x0F05C000, 4096, 0x0F05D000, 4096) &&
        !rs::Overlaps(0x0F05C000, 0, 0x0F05C000, 4096));
  // The fault handler's ring: reads in order, a full ring drops the newest until taken.
  {
    rs::ReadRing ring;
    ring.Push(0x1000, 16);
    ring.Push(0x2000, 32);
    int n = 0;
    ring.Take([&](uint32_t address, uint32_t length) {
      CHECK(n == 0 ? address == 0x1000 && length == 16 : address == 0x2000 && length == 32);
      ++n;
    });
    CHECK(n == 2);
    for (uint32_t i = 0; i < rs::ReadRing::kReads + 10; ++i) ring.Push(i, 1);
    n = 0;
    ring.Take([&](uint32_t address, uint32_t) { CHECK(address == uint32_t(n)); ++n; });
    CHECK(n == int(rs::ReadRing::kReads));
    ring.Push(7, 1);
    n = 0;
    ring.Take([&](uint32_t address, uint32_t) { CHECK(address == 7); ++n; });
    CHECK(n == 1);
  }

  const std::string cp = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string h = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  if (cp.empty() || h.empty()) {
    std::printf("no assembled tree here; the command processor not checked\n");
  } else {
    CHECK(h.find("uint32_t passes[readback_slots::kCount] = {};") != std::string::npos);
    CHECK(h.find("uint64_t frames[readback_slots::kCount] = {};") != std::string::npos);
    // The pass counted per frame before the slot is chosen.
    const size_t counted = cp.find("const uint32_t pass = ++rb.pass;");
    const size_t write = cp.find("uint32_t write_index = readback_slots::ChooseWrite(\n"
                                 "        rb.submissions, rb.valid_sizes, rb.passes, readback_slots::kCount,");
    const size_t read = cp.find("rb.passes, rb.frames,\n                                         readback_slots::kCount,");
    CHECK(cp.find("if (rb.pass_frame != frame_current_) {") < counted);
    CHECK(counted != std::string::npos && counted < write && write != std::string::npos && write < read &&
          read != std::string::npos);
    CHECK(cp.find("pass, frame_current_,\n                                         readback_completed, auto_readback)", read) !=
          std::string::npos);
    // Stamped with the copy, cleared with a new buffer.
    CHECK(cp.find("rb.passes[write_index] = pass;  // fix 91\n    rb.frames[write_index] = frame_current_;") !=
          std::string::npos);
    CHECK(cp.find("rb.passes[write_index] = 0;  // fix 91\n      rb.frames[write_index] = 0;") != std::string::npos);
    CHECK(cp.find("is resolved {} times in a frame: each resolve") != std::string::npos);
    // The earlier tries are gone.
    CHECK(cp.find("OwnCopyAfterPause") == std::string::npos && cp.find("RepeatedInFrame") == std::string::npos);
    // 1.0.5-work-fix91f: the read callback registered and unregistered, the reads taken before
    // the key's frame bookkeeping, the rule in the fast mode only, the watch armed after the copy.
    CHECK(cp.find("memory_->RegisterPhysicalMemoryReadCallback(ReadbackReadCallback, this);") != std::string::npos);
    CHECK(cp.find("memory_->UnregisterPhysicalMemoryReadCallback(readback_read_callback_);") != std::string::npos);
    const size_t take = cp.find("readback_reads_.Take(");
    const size_t gap = cp.find("readback_slots::Occasional(previous_use_frame, frame_current_, rb.gap_average);");
    CHECK(take != std::string::npos && take < gap && gap != std::string::npos);
    // 1.0.5-work-fix91j: "occasional own" takes every key that is not steady, read or not; without
    // it, a key the CPU is seen reading that is occasional. Counted and logged.
    // Fix 133 keeps the switch and the 91i side, and takes a key whose family (by its end
    // address) is steady off the 91k side unless the CPU reads it.
    CHECK(cp.find("const bool own_copy = g_readback_rules.occasional_own\n"
                  "                              ? !readback_slots::SteadyPresence(rb.presence)\n"
                  "                              : rb.cpu_reads && occasional;") != std::string::npos ||
          cp.find("        g_readback_rules.occasional_own\n"
                  "            ? readback_batching::OwnCopy(key_steady, family_steady, rb.cpu_reads,\n"
                  "                                         cvars::readback_steady_by_end)\n"
                  "            : rb.cpu_reads && occasional;") != std::string::npos);
    const size_t presence = cp.find("rb.presence = readback_slots::Presence(previous_use_frame, frame_current_, rb.presence);");
    CHECK(presence != std::string::npos && presence < cp.find("const bool own_copy ="));
    CHECK(cp.find("if (readback_mode == ReadbackResolveMode::kFast && !auto_readback && own_copy &&\n"
                  "        newest_copy >= 0) {\n      read_cache_miss = true;") != std::string::npos);
    const size_t streak = cp.find("rb.streak = readback_slots::Streak(previous_use_frame, frame_current_, rb.streak);");
    CHECK(gap != std::string::npos && gap < streak && streak != std::string::npos &&
          streak < cp.find("const bool own_copy ="));
    CHECK(cp.find("\"Readback own copies, last 30 s: {} ({}) (fix 91j/k)\"") != std::string::npos);
    CHECK(cp.find("\"Readback: the key {:08X} ({} bytes) gets its own copy: {} frame(s) in a row, about \"") !=
          std::string::npos);
    const size_t copy = cp.find("memory::vastcpy(memory_->TranslatePhysical(written_address),");
    const size_t arm = cp.find("memory_->EnablePhysicalMemoryAccessCallbacks(written_address, readback_length, false,");
    CHECK(copy != std::string::npos && copy < arm && arm != std::string::npos);
    CHECK(cp.find("(watch_forced ||\n             (!rb.cpu_reads && (!rb.armed_frame || frame_current_ - rb.armed_frame >= 600)))") !=
          std::string::npos);
    // 1.0.5-work-fix91g: the read watch observable: fires and drops counted in the callback,
    // the arms and flagged keys on the GPU thread, a 30 s line, the first reads raw.
    CHECK(cp.find("self->readback_read_fires_.fetch_add(1, std::memory_order_relaxed);") != std::string::npos);
    CHECK(cp.find("if (!self->readback_reads_.Push(physical_address, length)) {") != std::string::npos);
    CHECK(cp.find("++readback_watches_armed_;") != std::string::npos && cp.find("++readback_keys_flagged_;") !=
          std::string::npos);
    CHECK(cp.find("\"Readback read watches, last 30 s: armed {}, reads seen {} (dropped {}, {} of them a \"") !=
          std::string::npos);
    CHECK(cp.find("\"Readback read watch: the CPU read {:08X} ({} bytes) (fix 91g)\"") != std::string::npos);
    const std::string h = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
    CHECK(h.find("bool cpu_reads = false;") != std::string::npos && h.find("readback_slots::ReadRing readback_reads_;") !=
          std::string::npos);
    std::printf("tree: the fast readback gives each pass the previous frame's copy of itself, and an occasional "
                "key the CPU reads its own\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-repeat: all passed\n");
  return 0;
}
