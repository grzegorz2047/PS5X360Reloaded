// SPDX-License-Identifier: MIT
// Fix 112 (tools/source-1.0.2/fixes/112-swap-pacing.patch): fix 10's VdSwap pacing (a 16.60 ms
// grid while VSync is on) only for a game whose own swaps come faster than the vblank. Pro
// Evolution Soccer 6 (4B4E07DA) waits for the vblank itself; its early swaps slept until the grid
// point and missed the next vblank: 53.5 to 54 frames a second with VSync on, 59.9 with it off
// (1.0.5_candidate_65, the same menus). Checked:
//  (a) include/xbox360ps5/swap_pacing.hpp in a model of a game that waits for each 60 Hz vblank
//      and then works 6 to 16 ms (with a few long frames), where a sleep wakes 1.5 ms late (a
//      host sleep wakes after the scheduler's latency, not at its deadline): fix 10's pacing
//      (always on) sleeps in many swaps and makes frames miss their vblank; fix 112 stops pacing
//      after the first window and the frames miss nothing more than their own long work;
//  (b) a game unlocked to about 80 frames a second (12.5 ms of its own per frame) is paced as
//      fix 10 paced it: about 60 swaps a second, the same as fix 10's pacing;
//  (c) a game that goes from unlocked to vblank-paced and back is paced, then not, then again;
//  (d) the assembled tree: PaceSwap still does nothing with VSync off, asks the pacer under a
//      lock, sleeps outside it, and logs the pacing every 30 s.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/swap_pacing.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
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
namespace sp = xbox360ps5::swap_pacing;

// Fix 10's pacing as it was (always on while VSync is on).
struct Fix10 {
  int64_t deadline = 0;
  uint64_t slept = 0;
  int64_t OnSwap(int64_t now) {
    const bool on_grid = deadline != 0 && now < deadline + sp::kPeriodNs;
    const int64_t sleep = on_grid && deadline > now ? deadline - now : 0;
    deadline = on_grid ? deadline + sp::kPeriodNs : now + sp::kPeriodNs;
    if (sleep) ++slept;
    return sleep;
  }
};

constexpr int64_t kVblankNs = 16666667;
constexpr int64_t kWakeLateNs = 1500000;  // a host sleep wakes after its deadline

// A game that starts each frame at a vblank, works, swaps (the pacing may sleep), and waits for
// the next vblank after its swap returns: a frame shows at the first vblank after the swap.
template <typename Pacer>
int VblankPacedMisses(Pacer& pacer, int frames, int& long_frames) {
  std::mt19937 random(112);
  int64_t vblank = kVblankNs;
  int misses = 0;
  long_frames = 0;
  for (int frame = 0; frame < frames; ++frame) {
    int64_t work = 6000000 + int64_t(random() % 10000000);  // 6 to 16 ms
    if (random() % 50 == 0) {
      work = 20000000;  // a long frame of its own
      ++long_frames;
    }
    const int64_t swap = vblank + work;
    const int64_t sleep = pacer.OnSwap(swap);
    const int64_t returned = swap + (sleep ? sleep + kWakeLateNs : 0);
    int64_t next = vblank + kVblankNs;
    while (next < returned) {  // the vblank came while it was still working or sleeping
      next += kVblankNs;
      ++misses;
    }
    vblank = next;
  }
  return misses;
}

void CheckVblankPaced() {
  int long_old = 0, long_new = 0;
  Fix10 old;
  const int misses_old = VblankPacedMisses(old, 3600, long_old);
  sp::Pacer pacer;
  const int misses_new = VblankPacedMisses(pacer, 3600, long_new);
  std::printf("   vblank-paced game, 3600 frames: fix 10 slept in %llu swaps, %d missed vblanks; fix 112 slept "
              "in %llu, %d missed (%d long frames of its own)\n",
              (unsigned long long)old.slept, misses_old, (unsigned long long)pacer.slept, misses_new, long_new);
  Expect(!pacer.pacing && pacer.changes == 1, "fix 112 sees the game pace itself and stops pacing");
  Expect(pacer.slept <= sp::kWindow, "no swap sleeps after the first window");
  Expect(misses_new <= long_new + 2, "the frames miss nothing more than their own long work");
  Expect(misses_old > misses_new, "fix 10's pacing made this game miss vblanks");
}

void CheckUnlocked() {
  Fix10 old;
  sp::Pacer pacer;
  int64_t t_old = 0, t_new = 0;
  for (int frame = 0; frame < 3600; ++frame) {  // 12.5 ms of its own per frame
    t_old += 12500000;
    t_old += old.OnSwap(t_old);
    t_new += 12500000;
    t_new += pacer.OnSwap(t_new);
  }
  const double fps_old = 3600.0 / (double(t_old) / 1e9), fps_new = 3600.0 / (double(t_new) / 1e9);
  std::printf("   unlocked game (80 fps of its own): fix 10 %.1f fps, fix 112 %.1f fps\n", fps_old, fps_new);
  Expect(pacer.pacing && pacer.changes == 0, "an unlocked game stays paced");
  Expect(fps_new > 59.5 && fps_new < 60.6 && fps_new - fps_old < 0.2 && fps_old - fps_new < 0.2,
         "paced to about 60 swaps a second, as fix 10");
}

void CheckSwitching() {
  sp::Pacer pacer;
  int64_t t = 0;
  auto run = [&](int frames, int64_t own) {
    for (int n = 0; n < frames; ++n) {
      t += own;
      t += pacer.OnSwap(t);
    }
  };
  run(320, 12500000);
  const bool paced_menu = pacer.pacing;
  run(320, kVblankNs);
  const bool paced_game = pacer.pacing;
  run(320, 12500000);
  Expect(paced_menu && !paced_game && pacer.pacing && pacer.changes == 2, "paced, not paced, paced again");
}

void CheckTree() {
  const std::string video = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc");
  const size_t fn = video.find("static void PaceSwap() {");
  const size_t vsync = video.find("if (!xbox360ps5::EffectiveVsync(cvars::vsync)) {\n    return;\n  }", fn);
  const size_t ask = video.find("std::lock_guard<std::mutex> lock(pacer_mutex);\n    sleep_ns = pacer.OnSwap(now);", vsync);
  const size_t line = video.find("\"Performance swap pacing (fix 112): {:.1f} swaps/s, {:.1f} slept/s \"", ask);
  const size_t sleep = video.find("  if (sleep_ns > 0) {\n    xe::threading::NanoSleep(sleep_ns);\n  }\n}", line);
  Expect(fn != std::string::npos && vsync != std::string::npos && ask != std::string::npos && line != std::string::npos &&
             sleep != std::string::npos,
         "PaceSwap: nothing with VSync off, the pacer under a lock, the line every 30 s, the sleep outside the lock");
  Expect(video.find("#include \"xbox360ps5/swap_pacing.hpp\"  // fix 112") != std::string::npos &&
             video.find("static std::atomic<int64_t> deadline_ns{0};") == std::string::npos,
         "fix 10's own grid is replaced by the pacer's");
  Expect(video.find("#if XBOX360PS5_SWAP_CAP\n  PaceSwap();\n#endif") != std::string::npos, "VdSwap still paces");
}

}  // namespace

int main() {
  CheckVblankPaced();
  CheckUnlocked();
  CheckSwitching();
  CheckTree();
  if (failures) {
    std::printf("check-swap-pacing: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-swap-pacing: ok\n");
  return 0;
}
