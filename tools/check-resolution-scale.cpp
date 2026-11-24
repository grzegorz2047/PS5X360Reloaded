// SPDX-License-Identifier: MIT
// Fix 117 (tools/source-1.0.2/fixes/117-resolution-scale.patch): "Resolution scale (test)", a game
// rendered at twice its resolution (the core's draw_resolution_scale), with lines that tell on the
// console whether it works and a floor of free direct memory for the scaled resolve heaps.
//  (a) include/xbox360ps5/resolution_scale.hpp: the choices (0 1x, 1 2x, nothing else gives more
//      than 1x), the 1 GiB floor (a heap that would leave less is refused, an unknown amount is
//      allowed), the counters, and the lines' texts (none every 10 s at 1x);
//  (b) the launcher: option draw_scale, a start option a game may have of its own, 1x by default,
//      exactly the two choices; Apply sets both core scales from it; the old resolution_scale
//      key of settings.txt (written as 1 by every earlier build) is still not read, so it cannot
//      turn into 2x; the launcher's built-in presets give no game a scale;
//  (c) the core: the device line after the clamp, the sparse buffers' line, the floor asked
//      before each heap's vkAllocateMemory, the refused commits not logged one by one, the 10 s line.
// Fix 117b (117b-resolution-scale-floor.patch): the same floor for new render targets of the scaled
// class (their pitch and MSAA render at 1x from then on, the threshold's native class) and new scaled
// textures, the free memory asked at most every 250 ms, and the Vulkan driver's memory (every
// vkAllocateMemory less vkFreeMemory, counted in radv_dispatch.cpp) in the 10 s line, at 1x too.
//  (d) the header: the driver memory tally (sizes by handle, a handle freed twice or never seen, the
//      peak), the cached free memory, the render target and texture admissions and their estimates;
//  (e) the tree: radv_dispatch.cpp hands out counting vkAllocateMemory/vkFreeMemory through
//      vkGetDeviceProcAddr; IsScaleNativeForPitch honours the kept-native classes; the render
//      target and texture floors come before their allocations; render_target_cache.h includes
//      platform.h (the class's layout must not differ between translation units).
// Fix 117c (117c-resolution-scale-burst.patch): Tekken Tag Tournament 2 at 2x made ~2 GB of render
// targets in 10 s on one cached reading of free memory, then drew black textures.
//  (f) the header: bytes admitted since a reading count against it (a burst cannot pass on one
//      reading), a fresh reading resets that, render targets stop at 1.5 GiB, a refused texture
//      counts as made at 1x, the made-by-kind counters and the line's text;
//  (g) the tree: render targets and heaps ask for a fresh reading; FindOrCreateTexture makes a
//      scaled texture under the floor at 1x (scaled_resolve 0) before it looks for or creates it;
//      vulkan_texture_cache.cc no longer refuses textures, it counts the scaled ones; render
//      targets and the texture cache's total are counted for the line.
// Fix 117d (117d-resolution-scale-correct.patch): Battlefield 3 at 2x without image readback showed both
// fallbacks wrong (a texture made at 1x read old guest memory: a flat grey world; a resolve whose heap was
// refused took the core's unscaled path and wrote scaled-layout data into guest memory: tile garbage).
//  (h) the header: heaps always committed and counted under the floor, the texture cache's eviction
//      limits under pressure (half the cache, at least 128 MiB, never above its own; 2 s unused), the
//      once-a-session warning at 2x under 1 GiB, the line's text;
//  (i) the tree: no texture is made at 1x any more (FindOrCreateTexture has no fallback, no
//      AdmitTexture anywhere), heaps are not refused, a scaled resolve without a buffer is skipped
//      before the core's fallback and the copy, the texture cache's eviction takes the pressure limits
//      and counts what it evicts, and the notice and its text in Tr.
// Fix 117e (117e-resolution-scale-threshold.patch): a game's presets.txt section may say
// scale_threshold=<pixels>; at 2x surfaces up to that pitch render at 1x (the core's
// draw_resolution_scale_threshold). Tekken Tag Tournament 2 drew its fighters dark at 2x.
//  (j) the header: the threshold only at 2x and only for 1 to 8192 pixels, the start line;
//  (k) the tree: launcher.cpp reads scale_threshold from the game's presets.txt section, the game
//      start sets the core's threshold from it and the game's own scale (0 otherwise) and logs it;
//      assets/presets.txt (argument 1, from the repository) gives Tekken Tag Tournament 2 1040 and
//      keeps its readback=1.
// Fix 117f (117f-resolution-scale-trim.patch): Tekken Tag Tournament 2 at 2x made 4.2 GiB of render targets,
// never freed, down to 94 MiB free, and its 117e preset was not in the staged package.
//  (l) the header: no render target under 512 MiB, the trim's conditions (2x, under 1.5 GiB, every 10 s at
//      most, only after new render targets), its counters and line, the compiled-in thresholds;
//  (m) the tree: the trim at the end of a frame before the core's cache clear, after an idle wait, with the
//      core's RenderTargetCache::ClearCache; the 1x-class stop before the allocation; the launcher's
//      threshold from presets.txt first, then compiled in, with its source on the start line.
// Fix 117g (117g-resolution-scale-idle-trim.patch): Battlefield 3 showed noise after each 117f trim (the
// core's ClearCache destroyed per-frame aliases). Only targets owning no EDRAM data, not bound last and
// unused for 300 frames go now.
//  (n) the header: MayTrim's rules and the trim line;
//  (o) the tree: render targets stamped with the frame at each lookup and creation, the frame counted
//      in BeginFrame, TrimUnusedRenderTargets keeping owners, the last bound ones and recent ones, the
//      Vulkan side dropping framebuffers first, the command processor calling it instead of ClearCache.
// Fix 117h (117h-resolution-scale-key-log.patch): each render target made at 2x is logged once, at most 64
// lines a session, to choose Battlefield 3's threshold from data.
//  (p) the header: the line's text, the 64-line cap;
//  (q) the tree: the line after the render target is counted, only at 2x, through the cap.
// Fix 117i (117i-resolution-scale-bf3.patch): Battlefield 3's threshold, 640, compiled in (its scene is
// 1280 pixels of pitch; its half-resolution particle and smoke passes 640 and below).
//  (r) the header: Battlefield 3 compiled in at 640, Tekken Tag Tournament 2 kept at 1040.
// Fix 117j (117j-resolution-scale-rdr.patch): Red Dead Redemption's threshold, 800, compiled in (its
// scene and post-processing are 1280 pixels of pitch; its shadow cascade 800 and smaller passes below).
//  (s) the header: Red Dead Redemption compiled in at 800, below its 1280-pixel scene; the start line
//      names it.
// Fix 117k (117k-resolution-scale-classes.patch): Red Dead Redemption's 800 threshold broke its frame (passes
// sharing EDRAM with the 1280 frame); a game names exact 1x classes (pitch and MSAA) instead, compiled in or
// from the console's resolution-scale/<TITLE ID>.txt, which wins.
//  (t) the header: EDRAM tiles of a pitch (80 samples, 40 pixels at 4xMSAA), the MSAA index, the parser
//      (separators, comments, bad entries), Red Dead Redemption's compiled list and no threshold, the class
//      bits only at 2x, the start line;
//  (u) the tree: IsScaleNativeForPitch asks the class list, the launcher reads the console's file, the
//      game start sets the classes (the file first) and logs them.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "xbox360ps5/resolution_scale.hpp"

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}

namespace rs = xbox360ps5::resolution_scale;

int queries = 0;
int64_t answer = 0;
int64_t Query() {
  ++queries;
  return answer;
}

void CheckHeader() {
  Expect(rs::kChoices == 2, "two choices");
  Expect(rs::ScaleForChoice(0) == 1, "choice 0 is 1x");
  Expect(rs::ScaleForChoice(1) == 2, "choice 1 is 2x");
  for (int choice : {-1, 2, 3, 7, 100}) Expect(rs::ScaleForChoice(choice) == 1, "choice " + std::to_string(choice) + " is 1x");

  constexpr uint64_t kHeap = uint64_t(16) << 20;
  constexpr int64_t kGiB = int64_t(1) << 30;
  Expect(rs::kDirectFloor == uint64_t(kGiB), "the floor is 1 GiB");
  Expect(rs::MayCommit(-1, kHeap), "unknown free memory: allowed");
  Expect(rs::MayCommit(4 * kGiB, kHeap), "4 GiB free: allowed");
  Expect(rs::MayCommit(kGiB + int64_t(kHeap), kHeap), "exactly the floor left: allowed");
  Expect(!rs::MayCommit(kGiB + int64_t(kHeap) - 1, kHeap), "one byte under the floor: refused");
  Expect(!rs::MayCommit(kGiB, kHeap), "1 GiB free: a heap would leave less, refused");
  Expect(!rs::MayCommit(0, kHeap), "nothing free: refused");

  rs::Counters c;
  rs::DriverMemory::Totals driver;
  driver.bytes = uint64_t(1500) << 20;
  driver.peak = uint64_t(1600) << 20;
  driver.allocations = 420;
  c.texture_cache_bytes = uint64_t(300) << 20;
  const std::string one = rs::Line(c, driver, 3000ll << 20);
  Expect(one == "Resolution scale (fix 117): 1x; texture cache 300 MiB; driver memory 1500 MiB (peak 1600 MiB, 420 "
                "allocations); direct free 3000 MiB",
         "10 s line at 1x (fixes 117b, 117c): " + one);
  c.scale = 2;
  c.sparse = true;
  Expect(rs::Admit(c, 3000ll << 20, kHeap), "3000 MiB free: admitted");
  rs::Allocated(c, true, kHeap);
  Expect(rs::Admit(c, 2000ll << 20, kHeap), "2000 MiB free: admitted");
  rs::Allocated(c, false, kHeap);
  Expect(!rs::Admit(c, 1000ll << 20, kHeap), "1000 MiB free: refused");
  Expect(!rs::Admit(c, 900ll << 20, kHeap), "900 MiB free: refused");
  Expect(rs::Admit(c, -1, kHeap), "unknown: admitted");
  rs::Allocated(c, true, kHeap);
  Expect(c.heaps == 2 && c.heap_bytes == 2 * kHeap, "two heaps of 16 MiB counted");
  Expect(c.failed == 1 && c.refused == 2, "one failed, two refused");
  Expect(c.lowest_free == (900ll << 20), "lowest free seen at a commit: 900 MiB");
  Expect(!rs::AdmitRenderTarget(c, 1030ll << 20, rs::RenderTargetBytes(2560, 1440, 1, false)),
         "a 2560x1440 render target (14 MiB) with 1030 MiB free: refused");
  Expect(!rs::AdmitRenderTarget(c, 1500ll << 20, rs::RenderTargetBytes(2560, 1440, 1, false)),
         "fix 117c: the same with 1500 MiB free: refused (render targets stop at 1.5 GiB)");
  Expect(rs::AdmitRenderTarget(c, 1600ll << 20, rs::RenderTargetBytes(2560, 1440, 1, false)),
         "with 1600 MiB free: admitted");
  Expect(rs::AdmitRenderTarget(c, 3000ll << 20, rs::RenderTargetBytes(2560, 1440, 4, true)),
         "a 4xMSAA depth target with 3000 MiB free: admitted");
  Expect(c.native_classes == 2, "two render target classes kept at 1x");
  // Fix 117d: heaps always committed, counted under the floor.
  rs::NoteHeapCommit(c, 3000ll << 20, kHeap);
  rs::NoteHeapCommit(c, 1030ll << 20, kHeap);
  rs::NoteHeapCommit(c, 500ll << 20, kHeap);
  rs::NoteHeapCommit(c, -1, kHeap);
  Expect(c.heaps_under_floor == 2, "two heaps committed under the floor (1030 and 500 MiB free)");
  c.textures_evicted = 7;
  c.resolves_skipped = 3;
  rs::NoteRenderTarget(c, true, uint64_t(118) << 20);
  rs::NoteRenderTarget(c, true, uint64_t(30) << 20);
  rs::NoteRenderTarget(c, false, uint64_t(4) << 20);
  rs::NoteScaledTexture(c, uint64_t(28) << 20);
  const std::string line = rs::Line(c, driver, 1200ll << 20);
  Expect(line == "Resolution scale (fix 117): 2x, scaled resolve sparse, 2 heaps (32 MiB) committed, 2 refused "
                 "(under 1024 MiB free), 1 failed; render targets made scaled 2 (148 MiB), 1x 1 (4 MiB), classes "
                 "kept at 1x 2 (under 1536 MiB free); scaled textures made 1 (28 MiB); texture cache 300 MiB, 7 "
                 "evicted under memory pressure; heaps under the floor 2, resolves skipped 3; render targets refused "
                 "under 512 MiB free 0, trims 0 (0 destroyed); driver memory 1500 MiB (peak 1600 MiB, 420 "
                 "allocations); direct free 1200 MiB (lowest at a commit 500 MiB)",
         "10 s line: " + line);
  c.sparse = false;
  Expect(Has(rs::Line(c, driver, -1), "scaled resolve none,") && Has(rs::Line(c, driver, -1), "direct free -1 MiB"),
         "10 s line without sparse buffers or a known free amount");

  // Fix 117b: estimates.
  Expect(rs::RenderTargetBytes(2560, 1440, 1, false) == uint64_t(2560) * 1440 * 4, "a 32bpp render target");
  Expect(rs::RenderTargetBytes(2560, 1440, 4, true) == uint64_t(2560) * 1440 * 4 * 8, "a 4xMSAA 64bpp/depth target");
  Expect(rs::RenderTargetBytes(10, 10, 0, false) == 400, "no sample count counts as one");
  Expect(rs::TextureBytes(100, 100, 1, false) == 80000, "a texture without mips, 8 bytes a texel");
  Expect(rs::TextureBytes(100, 100, 6, true) == 480000 + 160000, "a cube with mips: a third more");

  // Fix 117b: the driver's memory.
  rs::DriverMemory m;
  m.Allocated(0x10, 100);
  m.Allocated(0x20, 300);
  Expect(m.Get().bytes == 400 && m.Get().peak == 400 && m.Get().allocations == 2, "two allocations");
  m.Freed(0x10);
  Expect(m.Get().bytes == 300 && m.Get().peak == 400 && m.Get().allocations == 1, "one freed, the peak kept");
  m.Freed(0x10);
  m.Freed(0x99);
  Expect(m.Get().bytes == 300 && m.Get().allocations == 1, "a handle freed twice or never seen changes nothing");
  m.Allocated(0x20, 50);
  Expect(m.Get().bytes == 50 && m.Get().allocations == 1, "a handle reused by the driver counts its new size once");
  m.Allocated(0x30, 1000);
  Expect(m.Get().peak == 1050, "the peak follows the total");

  // Fix 117b: the free memory asked at most every 250 ms.
  queries = 0;
  rs::direct_free_at_ms = -1000;
  answer = 5000;
  Expect(rs::DirectFreeRecent(10000, Query) == 5000 && queries == 1, "first ask queries");
  answer = 4000;
  Expect(rs::DirectFreeRecent(10100, Query) == 5000 && queries == 1, "100 ms later: the value kept");
  Expect(rs::DirectFreeRecent(10249, Query) == 5000 && queries == 1, "249 ms later: the value kept");
  Expect(rs::DirectFreeRecent(10250, Query) == 4000 && queries == 2, "250 ms later: asked again");
  answer = 3000;
  Expect(rs::DirectFreeRecent(9000, Query) == 3000 && queries == 3, "a clock behind the last ask: asked again");

  // Fix 117c: a burst cannot pass on one reading.
  queries = 0;
  rs::direct_free_at_ms = -1000;
  answer = int64_t(1700) << 20;
  rs::Counters burst;
  burst.scale = 2;
  const uint64_t depth = rs::RenderTargetBytes(2560, 1440, 4, true);  // ~112 MiB
  Expect(rs::DirectFreeRecent(20000, Query) == answer && queries == 1, "a reading of 1700 MiB");
  int admitted = 0;
  for (int i = 0; i < 20; ++i) {
    const int64_t reading = rs::DirectFreeRecent(20010 + i, Query);
    if (rs::MayCommit(reading, depth)) {
      rs::NoteAdmitted(depth);
      ++admitted;
    }
  }
  Expect(queries == 1, "the burst asks no new reading within 250 ms");
  Expect(admitted == 6, "on one 1700 MiB reading, only six 112 MiB allocations pass the 1 GiB floor (" +
                            std::to_string(admitted) + ")");
  Expect(rs::DirectFreeRecent(20100, Query) == answer - int64_t(6 * depth), "the reading less what was admitted");
  answer = int64_t(900) << 20;
  Expect(rs::DirectFreeFresh(20200, Query) == answer && queries == 2 && rs::admitted_since_reading == 0,
         "a fresh reading starts again from what the system says");
  Expect(rs::DirectFreeRecent(20300, Query) == answer && queries == 2, "and is kept for the next asks");
  rs::admitted_since_reading = int64_t(2000) << 20;
  Expect(rs::DirectFreeRecent(20310, Query) == 0, "never below zero");
  rs::direct_free_seen = -1;
  Expect(rs::DirectFreeRecent(20320, Query) == -1, "unknown stays unknown");
  // Render targets ask a fresh reading each: a burst of them stops at the floor.
  int rts = 0;
  int64_t system = int64_t(2200) << 20;
  for (int i = 0; i < 20; ++i) {
    answer = system;
    if (rs::AdmitRenderTarget(burst, rs::DirectFreeFresh(30000 + i, Query), depth)) {
      ++rts;
      system -= int64_t(depth);
    }
  }
  Expect(rts == 5, "render targets from 2200 MiB, 112 MiB each, stop at the 1.5 GiB floor (" + std::to_string(rts) + ")");

  // Fix 117d: the texture cache's eviction under pressure.
  const auto ev1 = rs::TextureEviction(456, 30000, 455, 3000ll << 20);
  Expect(!ev1.pressure && ev1.soft_mb == 456 && ev1.lifetime_ms == 30000, "3000 MiB free: the cache's own limits");
  const auto ev2 = rs::TextureEviction(456, 30000, 455, 1400ll << 20);
  Expect(ev2.pressure && ev2.soft_mb == 227 && ev2.lifetime_ms == 2000,
         "1400 MiB free: half of 455 MiB, textures unused for 2 s");
  const auto ev3 = rs::TextureEviction(456, 30000, 200, 900ll << 20);
  Expect(ev3.pressure && ev3.soft_mb == 128, "a small cache keeps at least 128 MiB");
  const auto ev4 = rs::TextureEviction(100, 1000, 900, 900ll << 20);
  Expect(ev4.soft_mb == 100 && ev4.lifetime_ms == 1000, "never above the cache's own limits");
  const auto ev5 = rs::TextureEviction(456, 30000, 455, -1);
  Expect(!ev5.pressure && ev5.soft_mb == 456, "unknown free memory: no pressure");
  // Fix 117e: the threshold only at 2x, only for a sane width.
  Expect(rs::ThresholdFor(2, 1040) == 1040, "2x with 1040: 1040");
  Expect(rs::ThresholdFor(1, 1040) == 0, "1x: no threshold");
  Expect(rs::ThresholdFor(2, 0) == 0 && rs::ThresholdFor(2, -5) == 0, "no preset or a negative one: none");
  Expect(rs::ThresholdFor(2, 8192) == 8192 && rs::ThresholdFor(2, 8193) == 0, "at most 8192 pixels");
  Expect(rs::ThresholdLine(1040, "4E4D0859", "compiled-in") ==
             "Resolution scale (fix 117e): surfaces up to 1040 pixels of pitch render at 1x (preset of 4E4D0859, "
             "compiled-in)",
         "the start line, with its source (fix 117f)");
  Expect(Has(rs::ThresholdLine(800, "", "presets.txt"), "(preset of no identified title, presets.txt)"),
         "the start line without a title");
  Expect(Has(rs::ThresholdLine(800, "X", nullptr), ", unknown)"), "the start line without a source");
  // Fix 117f: compiled-in thresholds.
  Expect(rs::CompiledThreshold("4E4D0859") == 1040 && rs::CompiledThreshold("4e4d0859") == 1040,
         "Tekken Tag Tournament 2 compiled in, any case");
  Expect(rs::CompiledThreshold("45410950") == 640 && rs::CompiledThreshold("45410950") < 1280 &&
             rs::ThresholdFor(2, rs::CompiledThreshold("45410950")) == 640,
         "fix 117i: Battlefield 3 compiled in at 640, below its 1280-pixel scene");
  Expect(rs::CompiledThreshold("5454082B") == 0,
         "fix 117k: Red Dead Redemption has no threshold any more (800 broke its frame, fix 117j)");
  // Fix 117k: render target classes at 1x.
  Expect(rs::PitchTiles(800, 1) == 10 && rs::PitchTiles(1280, 2) == 16 && rs::PitchTiles(40, 4) == 1 &&
             rs::PitchTiles(160, 4) == 4 && rs::PitchTiles(640, 4) == 16,
         "EDRAM tiles: 80 pixels each, 40 at 4xMSAA");
  Expect(rs::PitchTiles(100, 1) == 0 && rs::PitchTiles(0, 1) == 0 && rs::PitchTiles(60, 4) == 0, "not whole tiles: 0");
  Expect(rs::MsaaIndex(1) == 0 && rs::MsaaIndex(2) == 1 && rs::MsaaIndex(4) == 2, "MSAA index as xenos::MsaaSamples");
  {
    const auto parsed = rs::ParseNativeClasses("# shadow\n800x1, 40x4 80X4\n100x1 640x3 abc 160x4 # 320x1\n");
    Expect(parsed.size() == 4 && parsed[0].pitch_px == 800 && parsed[0].samples == 1 && parsed[1].pitch_px == 40 &&
               parsed[1].samples == 4 && parsed[2].pitch_px == 80 && parsed[3].pitch_px == 160,
           "the parser: separators, comments, whole tiles and 1/2/4 samples only (" + std::to_string(parsed.size()) +
               ")");
    Expect(rs::ParseNativeClasses("").empty() && rs::ParseNativeClasses("800").empty() &&
               rs::ParseNativeClasses("x4 800x").empty(),
           "nothing parsed from nothing");
    const auto rdr = rs::CompiledNativeClasses("5454082b");
    Expect(rdr.size() == 8 && rdr[0].pitch_px == 800 && rdr[0].samples == 1,
           "Red Dead Redemption: its shadow cascade and the passes of 40 to 200 pixels");
    for (const auto& c : rdr)
      Expect(c.pitch_px == 800 || c.pitch_px <= 200, "no pass between 200 and 800 (they share EDRAM with the frame)");
    Expect(rs::CompiledNativeClasses("45410950").empty(), "no list for other games");
    Expect(rs::SetNativeClasses(rdr, 1) == 0 && !rs::IsNativeClass(10, 0), "1x: no classes");
    Expect(rs::SetNativeClasses(rdr, 2) == 8, "2x: eight classes");
    Expect(rs::IsNativeClass(10, 0) && rs::IsNativeClass(1, 2) && rs::IsNativeClass(4, 2) && rs::IsNativeClass(2, 0),
           "800x1, 40x4, 160x4 and 160x1 at 1x");
    Expect(!rs::IsNativeClass(16, 1) && !rs::IsNativeClass(16, 0) && !rs::IsNativeClass(16, 2) &&
               !rs::IsNativeClass(8, 0),
           "the 1280 scene (2x and 1x MSAA), 640x4 and 640x1 stay scaled");
    Expect(!rs::IsNativeClass(300, 0) && !rs::IsNativeClass(10, 5), "out of range: no");
    Expect(rs::NativeClassesLine(rdr, "5454082B", "compiled-in") ==
               "Resolution scale (fix 117k): classes at 1x for 5454082B (compiled-in): 800x1 40x4 80x4 80x1 120x4 160x4 "
               "160x1 200x4",
           "the start line");
    rs::SetNativeClasses({}, 2);
    Expect(!rs::IsNativeClass(10, 0), "an empty list clears the classes");
  }
  Expect(rs::CompiledThreshold("545407F2") == 0 && rs::CompiledThreshold("") == 0 &&
             rs::CompiledThreshold("4E4D08590") == 0,
         "no other title, nor a malformed id");
  // Fix 117f: no render target under 512 MiB.
  {
    rs::Counters h;
    const uint64_t rt = uint64_t(14) << 20;
    Expect(rs::AdmitAnyRenderTarget(h, 600ll << 20, rt), "600 MiB free: a 14 MiB target made");
    Expect(!rs::AdmitAnyRenderTarget(h, 520ll << 20, rt), "520 MiB free: refused (would leave under 512)");
    Expect(rs::AdmitAnyRenderTarget(h, -1, rt), "unknown: made");
    Expect(h.rt_refused == 1, "one refused");
  }
  // Fix 117f: the trim's conditions.
  {
    rs::Counters t2;
    Expect(!rs::ShouldTrim(t2, 1000ll << 20, 100000), "1x: no trim");
    t2.scale = 2;
    Expect(!rs::ShouldTrim(t2, 1000ll << 20, 100000), "2x, nothing made yet: no trim");
    rs::NoteRenderTarget(t2, true, uint64_t(50) << 20);
    Expect(!rs::ShouldTrim(t2, 2000ll << 20, 100000), "2000 MiB free: no trim");
    Expect(!rs::ShouldTrim(t2, -1, 100000), "unknown free memory: no trim");
    Expect(rs::ShouldTrim(t2, 1400ll << 20, 100000), "1400 MiB free after a new target: trim");
    rs::NoteTrim(t2, 100000, 12);
    Expect(t2.trims == 1 && t2.trimmed == 12, "the trim counted");
    rs::NoteRenderTarget(t2, false, uint64_t(4) << 20);
    Expect(!rs::ShouldTrim(t2, 1400ll << 20, 105000), "5 s later: not yet");
    Expect(rs::ShouldTrim(t2, 1400ll << 20, 110000), "10 s later with a new target: trim");
    rs::NoteTrim(t2, 110000, 0);
    Expect(!rs::ShouldTrim(t2, 1400ll << 20, 130000), "nothing made since the last trim: no trim");
    Expect(rs::TrimLine(12, 30, uint64_t(4000) << 20, uint64_t(2500) << 20, 1900ll << 20) ==
               "Resolution scale (fix 117f): render targets trimmed: 12 destroyed, 30 kept (owning EDRAM data or "
               "used in the last 300 frames); driver memory 4000 -> 2500 MiB; direct free 1900 MiB",
           "the trim's line");
    // Fix 117h: the render target lines.
    Expect(rs::RenderTargetKeyLine(2560, 2048, 1280, 0, 1, "k_8_8_8_8", false, true) ==
               "Resolution scale (fix 117h): render target 2560x2048, pitch 1280 px, EDRAM base 0, 1xMSAA, "
               "k_8_8_8_8 colour, scaled",
           "a scaled colour target's line");
    Expect(Has(rs::RenderTargetKeyLine(640, 512, 640, 1200, 4, "kD24FS8", true, false),
               "pitch 640 px, EDRAM base 1200, 4xMSAA, kD24FS8 depth, 1x"),
           "a 1x depth target's line");
    Expect(Has(rs::RenderTargetKeyLine(8, 8, 8, 0, 0, nullptr, false, true), "1xMSAA, ? colour"),
           "no sample count or format");
    rs::key_lines = 0;
    int taken = 0;
    for (int i = 0; i < 100; ++i) taken += rs::TakeKeyLine() ? 1 : 0;
    Expect(rs::kKeyLines == 64 && taken == 64, "at most 64 lines a session (" + std::to_string(taken) + ")");
    // Fix 117g: what a trim may destroy.
    Expect(rs::kTrimIdleFrames == 300, "300 idle frames");
    Expect(rs::MayTrim(false, false, 1000, 600), "unowned, not bound, 400 frames idle: may go");
    Expect(rs::MayTrim(false, false, 1000, 700), "exactly 300 frames idle: may go");
    Expect(!rs::MayTrim(false, false, 1000, 701), "299 frames idle: stays (a per-frame alias)");
    Expect(!rs::MayTrim(true, false, 5000, 0), "owning EDRAM data: stays");
    Expect(!rs::MayTrim(false, true, 5000, 0), "bound last: stays");
    Expect(!rs::MayTrim(false, false, 10, 20), "used after the frame asked about: stays");
  }
  // Fix 117d: the warning, once a session, at 2x under 1 GiB.
  rs::Counters w;
  Expect(!rs::ShouldWarn(w, 500ll << 20), "1x: no warning");
  w.scale = 2;
  Expect(!rs::ShouldWarn(w, 1100ll << 20), "2x with 1100 MiB free: no warning");
  Expect(!rs::ShouldWarn(w, -1), "unknown: no warning");
  Expect(rs::ShouldWarn(w, 1000ll << 20), "2x with 1000 MiB free: warned");
  Expect(!rs::ShouldWarn(w, 500ll << 20), "only once a session");

  rs::Device d;
  d.asked_x = d.asked_y = 2;
  d.used_x = d.used_y = 1;
  d.max_storage_buffer_range = uint64_t(1) << 32;
  d.max_image_2d = 16384;
  d.max_framebuffer_width = d.max_framebuffer_height = 16384;
  const std::string no_sparse = rs::DeviceLine(d, 6122ll << 20);
  Expect(no_sparse == "Resolution scale (fix 117): asked 2x2, in use 1x1; device sparseBinding no, sparseResidencyBuffer no, "
                      "fragmentShaderPixelInterlock no, maxStorageBufferRange 4096 MiB, maxImageDimension2D 16384, "
                      "maxFramebuffer 16384x16384; direct free 6122 MiB (the device cannot: 1x)",
         "device line without sparse: " + no_sparse);
  d.sparse_binding = d.sparse_residency_buffer = d.interlock = true;
  d.used_x = d.used_y = 2;
  const std::string sparse = rs::DeviceLine(d, -1);
  Expect(Has(sparse, "in use 2x2; device sparseBinding yes, sparseResidencyBuffer yes, fragmentShaderPixelInterlock yes") &&
             !Has(sparse, "cannot"),
         "device line with sparse: " + sparse);
  d.asked_x = d.asked_y = d.used_x = d.used_y = 1;
  Expect(!Has(rs::DeviceLine(d, -1), "cannot"), "1x asked: no 'cannot'");

  const std::string made = rs::SparseLine(rs::Sparse::kMade, 1, 128, 0, 6000ll << 20);
  Expect(made == "Resolution scale (fix 117): scaled resolve: 1 sparse buffer(s) of 2 GiB made, up to 128 heaps of 16 MiB; "
                 "direct free 6000 MiB, heaps stop below 1024 MiB free",
         "sparse line: " + made);
  Expect(Has(rs::SparseLine(rs::Sparse::kBufferFailed, 0, 0, -2, -1), "sparse buffer 0 of 2 GiB not made (VkResult -2)"),
         "sparse buffer failure line");
  Expect(Has(rs::SparseLine(rs::Sparse::kNoSupport, 0, 0, 0, -1), "no sparse binding on the device"), "no sparse line");
  Expect(Has(rs::SparseLine(rs::Sparse::kNoMemoryType, 0, 0, 0, -1), "no device-local memory type"), "memory type line");
  Expect(Has(rs::SparseLine(rs::Sparse::kOffByCvar, 0, 0, 0, -1), "tiled_shared_memory false"), "cvar line");
}

std::string presets_path = "assets/presets.txt";

void CheckLauncher() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::string header = Read("include/xbox360ps5/launcher.hpp");
  Expect(Has(header, "int draw_scale = 0;"), "Settings::draw_scale, 1x by default");
  Expect(Has(header, "int resolution_scale = 1;"), "the old field stays (written to settings.txt as before)");
  const std::regex option(
      R"re(\{"draw_scale", &S::draw_scale, (\d+), W::(\w+), (\d+), (true|false), "([^"]*)",\s*"[^"]*",\s*\{([^}]*)\}\})re");
  std::smatch m;
  const bool found = std::regex_search(launcher, m, option);
  Expect(found, "the draw_scale option");
  if (found) {
    Expect(m[1] == "0", "in the Video sub-menu");
    Expect(m[2] == "start", "a start option (the core reads the scale as it starts)");
    Expect(m[3] == "0", "1x recommended");
    Expect(m[4] == "true", "a game may have its own");
    Expect(m[5] == "Resolution scale (test)", "label (" + m[5].str() + ")");
    Expect(m[6] == R"x("1x (720p, native)", "2x (1440p, demanding)")x", "exactly 1x and 2x (" + m[6].str() + ")");
  }
  Expect(Has(launcher, "cvars::draw_resolution_scale_x = cvars::draw_resolution_scale_y =\n"
                       "      xbox360ps5::resolution_scale::ScaleForChoice(draw_scale);"),
         "Apply sets both scales from draw_scale");
  Expect(!Has(launcher, "= resolution_scale;"), "the scales no longer come from the old field");
  Expect(Has(launcher, "// resolution_scale: not read."), "the old key is still skipped in Load");
  const size_t table = launcher.find("built_in[] = {");
  const size_t table_end = launcher.find("\n  };", table);
  Expect(table != std::string::npos && launcher.substr(table, table_end - table).find("draw_scale") == std::string::npos,
         "no built-in preset sets a scale");
  const std::string presets = Read(presets_path);
  Expect(!presets.empty(), "assets/presets.txt read (" + presets_path + ")");
  Expect(!Has(presets, "draw_scale"), "no presets.txt entry sets a scale");
  {
    // Fix 117e: Tekken Tag Tournament 2's section.
    const size_t tekken = presets.find("\n[4E4D0859]\n");
    const size_t next = presets.find("\n[", tekken + 1);
    const std::string section = tekken == std::string::npos ? std::string()
                                                            : presets.substr(tekken, next == std::string::npos
                                                                                         ? std::string::npos
                                                                                         : next - tekken);
    Expect(Has(section, "\nreadback=1\n") && Has(section, "\nscale_threshold=1040\n"),
           "fix 117e: Tekken Tag Tournament 2 keeps readback=1 and gets scale_threshold=1040");
    size_t count = 0;
    for (size_t at = presets.find("scale_threshold="); at != std::string::npos; at = presets.find("scale_threshold=", at + 1))
      ++count;
    Expect(count == 1, "fix 117e: only Tekken Tag Tournament 2 has a threshold (" + std::to_string(count) + ")");
  }
}

void CheckCore() {
  const std::string processor = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string texture_cache = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_texture_cache.cc");
  const std::string render_targets = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc");
  const std::string game_main = Read("src/canary_game_main.cpp");
  const size_t clamp = processor.find("ClampDrawResolutionScaleToMaxSupported(");
  const size_t device_line = processor.find("xbox360ps5::resolution_scale::DeviceLine(");
  const size_t rt_cache = processor.find("render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(");
  Expect(clamp != std::string::npos && device_line > clamp && device_line < rt_cache,
         "the device line comes after the clamp, before the render target cache");
  Expect(Has(processor, "xbox360ps5::resolution_scale::counters.scale.store(\n"
                        "        std::max(draw_resolution_scale_x, draw_resolution_scale_y),"),
         "the scale in use is recorded for the 10 s line");
  for (const char* outcome : {"Sparse::kMade", "Sparse::kNoSupport", "Sparse::kBufferFailed", "Sparse::kNoMemoryType",
                              "Sparse::kOffByCvar"})
    Expect(Has(texture_cache, std::string("xbox360ps5::resolution_scale::") + outcome), std::string("sparse line ") + outcome);
  // The sparse heaps: the floor asked just before vkAllocateMemory (fix 117d: counted, not refused), its
  // outcome counted after.
  const size_t allocate = texture_cache.find("dfn.vkAllocateMemory(device, &allocate_info, nullptr, &heap)");
  const size_t admit = allocate == std::string::npos
                           ? std::string::npos
                           : texture_cache.rfind("xbox360ps5::resolution_scale::NoteHeapCommit(", allocate);
  const size_t heap_check = texture_cache.rfind("if (scaled_resolve_heaps_[heap_index] == VK_NULL_HANDLE) {", allocate);
  const size_t allocated = texture_cache.find("xbox360ps5::resolution_scale::Allocated(", allocate);
  Expect(allocate != std::string::npos && admit != std::string::npos && heap_check != std::string::npos &&
             admit > heap_check && allocated != std::string::npos && allocated - allocate < 200,
         "the floor is noted before each heap's allocation, the outcome counted after");
  // The fallback buffers (no sparse buffers while scaled): the same floor before vmaCreateBuffer.
  const size_t vma = texture_cache.find("vmaCreateBuffer(\n            vma_allocator_, &buffer_create_info");
  const size_t fallback_admit =
      vma == std::string::npos ? std::string::npos : texture_cache.rfind("xbox360ps5::resolution_scale::Admit(", vma);
  Expect(vma != std::string::npos && fallback_admit != std::string::npos && vma - fallback_admit < 800 &&
             texture_cache.find("xbox360ps5::resolution_scale::Allocated(", vma) != std::string::npos,
         "the fallback buffers keep the floor");
  Expect(Has(render_targets, "Fix 117") && Has(render_targets, "failed_commits < 8"),
         "a refused commit is logged a few times, not at every resolve");
  Expect(Has(game_main, "xbox360ps5::resolution_scale::Line(") &&
             Has(game_main, "xbox360ps5::resolution_scale::driver_memory.Get(),"),
         "the 10 s line, with the driver's memory (fix 117b)");

  // Fix 117b.
  const std::string dispatch = Read("platform/ps5/radv_dispatch.cpp");
  Expect(Has(dispatch, "if (!std::strcmp(name, \"vkGetDeviceProcAddr\")) {") &&
             Has(dispatch, "return reinterpret_cast<PFN_vkVoidFunction>(DeviceProc);") &&
             Has(dispatch, "return reinterpret_cast<PFN_vkVoidFunction>(AllocateMemory);") &&
             Has(dispatch, "return reinterpret_cast<PFN_vkVoidFunction>(FreeMemory);"),
         "the driver's vkAllocateMemory/vkFreeMemory are handed out counting, through vkGetDeviceProcAddr");
  Expect(Has(dispatch, "resolution_scale::driver_memory.Allocated(MemoryHandle(*memory), info->allocationSize);") &&
             Has(dispatch, "resolution_scale::driver_memory.Freed(MemoryHandle(memory));"),
         "allocations counted with their size, frees by handle");
  const std::string rt_header = Read(".deps/xenia-canary/src/xenia/gpu/render_target_cache.h");
  const std::string rt_common = Read(".deps/xenia-canary/src/xenia/gpu/render_target_cache.cc");
  const size_t platform = rt_header.find("#include \"xenia/base/platform.h\"");
  const size_t first_ps5 = rt_header.find("#if XE_PLATFORM_PS5");
  Expect(platform != std::string::npos && first_ps5 != std::string::npos && platform < first_ps5,
         "render_target_cache.h includes platform.h before its PS5 member (one layout in every unit)");
  const size_t native_fn = rt_common.find("bool RenderTargetCache::IsScaleNativeForPitch(");
  const size_t kept = rt_common.find("kept_native_[pitch_tiles_at_32bpp * 4 + uint32_t(msaa_samples)]", native_fn);
  const size_t threshold = rt_common.find("uint32_t threshold = cvars::draw_resolution_scale_threshold;", native_fn);
  Expect(native_fn != std::string::npos && kept != std::string::npos && threshold != std::string::npos && kept < threshold,
         "IsScaleNativeForPitch answers kept-native classes before the threshold");
  const size_t rt_admit = render_targets.find("xbox360ps5::resolution_scale::AdmitRenderTarget(");
  const size_t rt_alloc = render_targets.find("if (!ui::vulkan::util::CreateDedicatedAllocationImage(", rt_admit);
  const size_t rt_keep = render_targets.find("KeepScaleNativeForPitch(key.pitch_tiles_at_32bpp, key.msaa_samples);", rt_admit);
  Expect(rt_admit != std::string::npos && rt_keep != std::string::npos && rt_alloc != std::string::npos &&
             rt_keep < rt_alloc && Has(render_targets, "if (!key.scale_native && IsDrawResolutionScaled()) {"),
         "a scaled render target asks the floor first and keeps its class at 1x when refused");
  // Fix 117c, with 117d.
  const std::string common_textures = Read(".deps/xenia-canary/src/xenia/gpu/texture_cache.cc");
  Expect(!Has(common_textures, "AdmitTexture(") && !Has(common_textures, "key.scaled_resolve = 0;") &&
             !Has(texture_cache, "AdmitTexture("),
         "fix 117d: no texture is made at 1x (no fallback in FindOrCreateTexture)");
  Expect(Has(common_textures, "counters.texture_cache_bytes.store(\n      textures_total_host_memory_usage_"),
         "the texture cache's total goes to the line");
  Expect(Has(texture_cache, "xbox360ps5::resolution_scale::NoteScaledTexture("),
         "vulkan_texture_cache.cc counts scaled textures");
  Expect(Has(texture_cache, "xbox360ps5::resolution_scale::NoteHeapCommit(") &&
             Has(texture_cache, "xbox360ps5::resolution_scale::DirectFreeFresh(),\n          kScaledResolveHeapSize);") &&
             !Has(texture_cache, "Admit(\n              xbox360ps5::resolution_scale::counters,\n              "
                                 "xbox360ps5::resolution_scale::DirectFreeFresh(),\n              kScaledResolveHeapSize)"),
         "fix 117d: heaps are always committed, counted with a fresh reading");
  Expect(Has(texture_cache, "xbox360ps5::resolution_scale::DirectFreeFresh(), buffer_size)"),
         "fallback buffers ask a fresh reading");
  // Fix 117d: the texture cache's eviction.
  const size_t completed = common_textures.find("void TextureCache::CompletedSubmissionUpdated(");
  const size_t eviction = common_textures.find("xbox360ps5::resolution_scale::TextureEviction(", completed);
  const size_t loop = common_textures.find("while (texture_used_first_ != nullptr) {", completed);
  const size_t evicted = common_textures.find("counters.textures_evicted.fetch_add(", completed);
  const size_t erase = common_textures.find("textures_.erase(found_texture_it);", completed);
  Expect(completed != std::string::npos && eviction != std::string::npos && loop != std::string::npos &&
             eviction < loop && evicted > loop && evicted < erase,
         "the eviction loop takes the pressure limits and counts what it evicts");
  // Fix 117d: a scaled resolve without a buffer is skipped before the core's fallback and the copy.
  const size_t skip = render_targets.find("const bool skip_scaled_copy = copy_dest_scaled && !scaled_buffer_ready;");
  const size_t guard = render_targets.find("if (!skip_scaled_copy) {", skip);
  const size_t core_fallback = render_targets.find("if (!scaled_buffer_ready && !use_persistent_dest) {", skip);
  const size_t copy_dispatch = render_targets.find("command_buffer.CmdVkDispatch(copy_group_count_x, copy_group_count_y,", skip);
  const size_t guard_end = render_targets.find("}  // !skip_scaled_copy (fix 117d)", skip);
  Expect(skip != std::string::npos && guard != std::string::npos && core_fallback != std::string::npos &&
             copy_dispatch != std::string::npos && guard_end != std::string::npos && guard < core_fallback &&
             core_fallback < copy_dispatch && copy_dispatch < guard_end,
         "fix 117d: the skip guards the core's unscaled fallback and the copy dispatch");
  // Fix 117d: the notice.
  Expect(Has(game_main, "xbox360ps5::resolution_scale::ShouldWarn(xbox360ps5::resolution_scale::counters, scale_free)") &&
             Has(game_main, "Tr(\"Resolution scale (test) 2x: this game needs more memory than is left. The picture may "
                            "break; set it to 1x in this game's settings.\")"),
         "fix 117d: the once-a-session notice through Tr");
  // Fix 117e.
  const std::string launcher = Read("src/launcher.cpp");
  const size_t threshold_fn = launcher.find("int ScaleThresholdFor(const std::string& title_id, const char** from) {");
  const size_t from_file = launcher.find("values.find(\"scale_threshold\")", threshold_fn);
  const size_t from_build = launcher.find("xbox360ps5::resolution_scale::CompiledThreshold(title_id)", threshold_fn);
  Expect(threshold_fn != std::string::npos &&
             launcher.find("ReadValues(\"/app0/assets/presets.txt\", title_id)", threshold_fn) != std::string::npos &&
             from_file != std::string::npos && from_build != std::string::npos && from_file < from_build &&
             launcher.find("*from = \"presets.txt\";", threshold_fn) != std::string::npos &&
             launcher.find("*from = \"compiled-in\";", threshold_fn) != std::string::npos,
         "fixes 117e, 117f: presets.txt's scale_threshold first, then the compiled-in one, with the source");
  Expect(Has(game_main, "DECLARE_uint32(draw_resolution_scale_threshold);") &&
             Has(game_main, "int ScaleThresholdFor(const std::string& title_id, const char** from);") &&
             Has(game_main, "xbox360ps5::resolution_scale::ThresholdFor(\n"
                            "              xbox360ps5::resolution_scale::ScaleForChoice(settings.draw_scale),\n"
                            "              xbox360ps5::ScaleThresholdFor(settings_title, &scale_threshold_from));") &&
             Has(game_main, "cvars::draw_resolution_scale_threshold = scale_threshold;") &&
             Has(game_main, "xbox360ps5::resolution_scale::ThresholdLine(scale_threshold, settings_title,\n"),
         "fix 117e: the game start sets the core's threshold at 2x (0 otherwise) and logs it with its source");
  // Fix 117f: the trim and the stop.
  const size_t trim = processor.find("xbox360ps5::resolution_scale::ShouldTrim(");
  const size_t trim_idle = processor.find("AwaitAllQueueOperationsCompletion()) {", trim);
  const size_t trim_clear = processor.find("render_target_cache_->TrimUnusedRenderTargetsVulkan(\n"
                                           "          xbox360ps5::resolution_scale::kTrimIdleFrames);", trim);
  const size_t core_clear = processor.find("if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {");
  Expect(trim != std::string::npos && trim_idle != std::string::npos && trim_clear != std::string::npos &&
             core_clear != std::string::npos && trim < trim_idle && trim_idle < trim_clear && trim_clear < core_clear &&
             Has(processor, "if (!cache_clear_requested_ && render_target_cache_ &&") &&
             Has(processor, "xbox360ps5::resolution_scale::NoteTrim("),
         "fixes 117f, 117g: the trim waits for the GPU and destroys only idle targets, before the core's own clear");
  // Fix 117g.
  const size_t get_fn = rt_common.find("RenderTargetCache::RenderTarget* RenderTargetCache::GetOrCreateRenderTarget(");
  const size_t stamp_found = rt_common.find("render_target->last_used_frame = trim_frame_;", get_fn);
  const size_t stamp_made =
      stamp_found == std::string::npos ? std::string::npos
                                       : rt_common.find("render_target->last_used_frame = trim_frame_;", stamp_found + 1);
  Expect(get_fn != std::string::npos && stamp_found != std::string::npos && stamp_made != std::string::npos,
         "fix 117g: a render target is stamped with the frame when looked up and when made");
  Expect(Has(rt_common, "  ++trim_frame_;  // fix 117g") && Has(rt_header, "uint64_t last_used_frame = 0;"),
         "fix 117g: the frame counted in BeginFrame, the stamp in RenderTarget");
  const size_t trim_fn = rt_common.find("size_t RenderTargetCache::TrimUnusedRenderTargets(uint64_t idle_frames) {");
  Expect(trim_fn != std::string::npos &&
             rt_common.find("ownership_range.host_depth_render_target_float24}) {", trim_fn) != std::string::npos &&
             rt_common.find("bound.emplace(last_update_used_render_targets_[i]);", trim_fn) != std::string::npos &&
             rt_common.find("bound.emplace(last_update_accumulated_render_targets_[i]);", trim_fn) != std::string::npos &&
             rt_common.find("xbox360ps5::resolution_scale::MayTrim(", trim_fn) != std::string::npos,
         "fix 117g: the trim keeps EDRAM owners, the last bound and the recently used targets");
  const size_t vk_trim = render_targets.find("size_t VulkanRenderTargetCache::TrimUnusedRenderTargetsVulkan(");
  const size_t vk_fb = render_targets.find("framebuffers_.clear();", vk_trim);
  const size_t vk_call = render_targets.find("return TrimUnusedRenderTargets(idle_frames);", vk_trim);
  Expect(vk_trim != std::string::npos && vk_fb != std::string::npos && vk_call != std::string::npos && vk_fb < vk_call,
         "fix 117g: the framebuffers go before the targets");
  // Fix 117h.
  const size_t note_rt = render_targets.find("xbox360ps5::resolution_scale::NoteRenderTarget(\n"
                                             "        xbox360ps5::resolution_scale::counters, !key.scale_native, rt_bytes);");
  const size_t take = render_targets.find("if (xbox360ps5::resolution_scale::TakeKeyLine()) {", note_rt);
  const size_t key_line = render_targets.find("xbox360ps5::resolution_scale::RenderTargetKeyLine(", take);
  Expect(note_rt != std::string::npos && take != std::string::npos && key_line != std::string::npos &&
             take - note_rt < 400,
         "fix 117h: each render target made at 2x is logged through the 64-line cap");
  // Fix 117k.
  const size_t native_check = rt_common.find("xbox360ps5::resolution_scale::IsNativeClass(pitch_tiles_at_32bpp,");
  const size_t threshold_read = rt_common.find("uint32_t threshold = cvars::draw_resolution_scale_threshold;");
  Expect(native_check != std::string::npos && threshold_read != std::string::npos && native_check < threshold_read,
         "fix 117k: IsScaleNativeForPitch asks the game's class list before the threshold");
  const size_t file_fn = launcher.find("std::string NativeClassesTextFor(const std::string& title_id) {");
  // Fix 165: in the user's data folder (UserData()); before 165 the internal storage (kStorage).
  Expect(file_fn != std::string::npos &&
             (launcher.find("UserData() / \"resolution-scale\" / (title_id + \".txt\")", file_fn) != std::string::npos ||
              launcher.find("kStorage / \"resolution-scale\" / (title_id + \".txt\")", file_fn) != std::string::npos),
         "fix 117k: the launcher reads <data>/resolution-scale/<TITLE ID>.txt");
  const size_t classes_set = game_main.find("xbox360ps5::resolution_scale::SetNativeClasses(");
  Expect(Has(game_main, "std::string NativeClassesTextFor(const std::string& title_id);") &&
             Has(game_main, "classes_text.empty() ? xbox360ps5::resolution_scale::CompiledNativeClasses(settings_title)\n"
                            "                                   : xbox360ps5::resolution_scale::ParseNativeClasses(classes_text);") &&
             classes_set != std::string::npos &&
             game_main.find("xbox360ps5::resolution_scale::ScaleForChoice(settings.draw_scale)", classes_set) !=
                 std::string::npos &&
             Has(game_main, "xbox360ps5::resolution_scale::NativeClassesLine(classes, settings_title, classes_from)"),
         "fix 117k: the game start sets the class list (the console's file first) at 2x and logs it");
  Expect(Has(rt_header, "size_t RenderTargetCount() const { return render_targets_.size(); }"),
         "fix 117f: the render target count");
  const size_t stop = render_targets.find("if (key.scale_native && IsDrawResolutionScaled() &&\n"
                                          "      !xbox360ps5::resolution_scale::AdmitAnyRenderTarget(");
  Expect(stop != std::string::npos && stop > rt_admit && stop < rt_alloc,
         "fix 117f: a 1x-class render target asks the 512 MiB stop before its allocation");
  const size_t rt_fresh = render_targets.find("xbox360ps5::resolution_scale::DirectFreeFresh(), bytes)");
  Expect(rt_fresh != std::string::npos && rt_fresh > rt_admit && rt_fresh < rt_alloc,
         "render targets ask a fresh reading");
  const size_t rt_note = render_targets.find("xbox360ps5::resolution_scale::NoteRenderTarget(", rt_alloc);
  Expect(rt_note != std::string::npos, "render targets made are counted after their allocation");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) presets_path = argv[1];
  CheckHeader();
  CheckLauncher();
  CheckCore();
  if (failures) {
    std::printf("check-resolution-scale: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-resolution-scale: ok\n");
  return 0;
}
