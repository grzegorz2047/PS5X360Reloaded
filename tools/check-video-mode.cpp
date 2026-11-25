// SPDX-License-Identifier: MIT
// Fix 140 (tools/source-1.0.2/fixes/140-video-mode-900p.patch): "Emulated console resolution" is what the game is
// told, and 1600x900 is added as value 4.
//  (a) include/xbox360ps5/video_mode.hpp: the five choices (0 the console's own, 1 1920x1080, 2 848x480,
//      3 960x540, 4 1600x900), anything else the default; a saved value out of range is the default; the size
//      reported (the custom one when both sides are set, else the console's); 900p back to the default reports
//      the console's own size again; hi-def and widescreen as VdQueryVideoMode decides them; the log line;
//  (b) the launcher: the option keeps its first four choices in order and adds 900p as the fifth, Apply sets
//      both core sizes from ForChoice (no "& 3" left), Load reads the saved value through LoadedChoice;
//  (c) the core: VdQueryVideoMode takes the custom size only when one is set (the default path is the
//      console's own size, as before), before deciding widescreen, and logs its first answer once.
// Fix 140b (140b-video-mode-real.patch): Minecraft crashed at 480p told 848x480 (a VGA-only mode); the game is
// told only modes a real console reports.
//  (d) the header: ReportedMode's table (1080p 1920x1080; 480p 640x480 with the widescreen flag; 900p 1600x900;
//      540p and any other size not reported), each option value through it;
//  (e) the core: VdQueryVideoMode takes the table's size and its widescreen flag.
// Fix 140c (140c-video-mode-launch.patch), from the pre-release review:
//  (f) the header: the size latched at launch and the mode told from it (a later change waits for the next start);
//      the 2x-tuning warning only at 2x, with tuning, when another mode is reported;
//  (g) the tree: VdQueryVideoMode and the scaler take the launch mode, the scaler's widescreen flag includes the
//      reported mode's (480p 16:9), the game start latches the size right after applying the settings, and logs
//      the tuning warning.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/video_mode.hpp"

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

namespace vm = xbox360ps5::video_mode;

bool Same(vm::Size a, uint32_t w, uint32_t h) { return a.width == w && a.height == h; }

void CheckHeader() {
  Expect(vm::kChoices == 5, "five choices");
  Expect(Same(vm::ForChoice(0), 0, 0), "0: the console's own (no custom size)");
  Expect(Same(vm::ForChoice(1), 1920, 1080), "1: 1920x1080 as before");
  Expect(Same(vm::ForChoice(2), 848, 480), "2: 848x480 as before");
  Expect(Same(vm::ForChoice(3), 960, 540), "3: 960x540 as before");
  Expect(Same(vm::ForChoice(4), 1600, 900), "4: 1600x900 (new)");
  for (int bad : {-1, 5, 7, 99, -100}) Expect(Same(vm::ForChoice(bad), 0, 0), "invalid " + std::to_string(bad) + ": default");
  for (int i = 0; i < 5; ++i) Expect(vm::LoadedChoice(i) == i, "saved " + std::to_string(i) + " keeps its meaning");
  for (int bad : {-1, 5, 8, 1000}) Expect(vm::LoadedChoice(bad) == 0, "saved " + std::to_string(bad) + ": the default");

  const vm::Size console{1280, 720};
  Expect(Same(vm::Reported(0, 0, console), 1280, 720), "no custom size: the console's own");
  Expect(Same(vm::Reported(1600, 0, console), 1280, 720) && Same(vm::Reported(0, 900, console), 1280, 720),
         "one side only: the console's own");
  Expect(Same(vm::Reported(1600, 900, console), 1600, 900), "900p: reported");
  Expect(Same(vm::Reported(70000, 900, console), 1280, 720), "a width beyond 16 bits: the console's own");
  Expect(vm::IsCustom(1600, 900) && !vm::IsCustom(0, 0) && !vm::IsCustom(1600, 0), "custom only when both are set");
  // 900p, then back to the default: the default sets 0x0 again, so the console's size is reported again.
  vm::Size cvars = vm::ForChoice(4);
  Expect(Same(vm::Reported(cvars.width, cvars.height, console), 1600, 900), "900p chosen");
  cvars = vm::ForChoice(0);
  Expect(cvars.width == 0 && cvars.height == 0 && Same(vm::Reported(cvars.width, cvars.height, console), 1280, 720),
         "900p back to the default: both sizes 0, the console's own reported");

  Expect(vm::HiDef({1600, 900}) && vm::HiDef({1280, 720}) && vm::HiDef({1920, 1080}), "hi-def from 1280 wide");
  Expect(!vm::HiDef({848, 480}) && !vm::HiDef({960, 540}) && !vm::HiDef({640, 480}), "not hi-def below 1280");
  Expect(vm::AspectWide({1600, 900}) && vm::AspectWide({848, 480}) && vm::AspectWide({960, 540}), "16:9 sizes wide");
  Expect(!vm::AspectWide({640, 480}) && !vm::AspectWide({1024, 768}), "4:3 sizes not wide");

  Expect(vm::Line({1600, 900}, true, true, true) ==
             "Video mode (fix 140): 1600x900 reported to the game (hi-def, widescreen), from the Emulated console "
             "resolution setting",
         "the line for 900p");
  Expect(vm::Line({1280, 720}, true, true, false) ==
             "Video mode (fix 140): 1280x720 reported to the game (hi-def, widescreen), the console's own (Emulated "
             "console resolution: 720p)",
         "the line for the default");
  Expect(Has(vm::Line({848, 480}, false, true, true), "848x480 reported to the game (standard definition, widescreen)"),
         "the line for 480p");
}

void CheckRealModes() {
  // Each option value: what the core's sizes are (as before) and what the game is told.
  struct Row {
    int choice;
    uint32_t told_width, told_height;
    bool reported, widescreen_forced;
  };
  const Row rows[] = {
      {0, 0, 0, false, false},        // the console's own (720p)
      {1, 1920, 1080, true, false},   // 1080p: a real HDMI mode
      {2, 640, 480, true, true},      // 480p: 640x480 with the widescreen flag, as over HDMI
      {3, 0, 0, false, false},        // 540p: not a real mode, the console's own
      {4, 1600, 900, true, false},    // 900p: a clean 16:9
  };
  for (const Row& row : rows) {
    const vm::Size core = vm::ForChoice(row.choice);
    const vm::Mode mode = vm::ReportedMode(core.width, core.height);
    const std::string name = "choice " + std::to_string(row.choice);
    Expect(mode.reported == row.reported, name + ": reported or not");
    if (row.reported)
      Expect(Same(mode.size, row.told_width, row.told_height), name + ": the size the game is told");
    Expect(mode.widescreen_forced == row.widescreen_forced, name + ": the widescreen flag");
  }
  Expect(Same(vm::ForChoice(2), 848, 480) && Same(vm::ForChoice(3), 960, 540),
         "the core's own sizes for 480p and 540p stay as before (the vertical counter's step)");
  Expect(!vm::ReportedMode(1024, 768).reported && !vm::ReportedMode(1, 1).reported && !vm::ReportedMode(0, 0).reported,
         "other sizes: the console's own");
  const vm::Mode p480 = vm::ReportedMode(848, 480);
  Expect(!vm::HiDef(p480.size) && !vm::AspectWide(p480.size) && p480.widescreen_forced,
         "480p: standard definition, 4:3-sized, widescreen by the flag");
}

void CheckLaunchLatch() {
  vm::LatchAtLaunch(0, 0);
  Expect(!vm::LaunchMode().reported, "nothing latched: the console's own");
  vm::Size core = vm::ForChoice(2);
  vm::LatchAtLaunch(core.width, core.height);
  Expect(vm::LaunchMode().reported && Same(vm::LaunchMode().size, 640, 480) && vm::LaunchMode().widescreen_forced,
         "480p latched at launch: 640x480 with the widescreen flag");
  // A change while the game runs only writes the core's sizes; the latched mode stays until the next start.
  core = vm::ForChoice(4);
  Expect(Same(vm::LaunchMode().size, 640, 480), "a change in the game: the mode stays 480p");
  vm::LatchAtLaunch(core.width, core.height);
  Expect(Same(vm::LaunchMode().size, 1600, 900) && !vm::LaunchMode().widescreen_forced, "the next start: 900p");
  vm::LatchAtLaunch(0, 0);
  Expect(vm::TuningMayNotFit(4, 2, true) && vm::TuningMayNotFit(1, 2, true) && vm::TuningMayNotFit(2, 2, true),
         "2x tuning with 900p, 1080p or 480p told: warned");
  Expect(!vm::TuningMayNotFit(0, 2, true) && !vm::TuningMayNotFit(3, 2, true), "720p and 540p (told 720p): not warned");
  Expect(!vm::TuningMayNotFit(4, 1, true) && !vm::TuningMayNotFit(4, 2, false), "1x or no tuning: not warned");
}

void CheckTree() {
  const std::string launcher = Read("src/launcher.cpp");
  Expect(Has(launcher, "{\"720p\", \"1080p\", \"480p (848x480)\", \"540p (960x540)\", \"900p (1600x900)\"}}"),
         "the option keeps its four choices in order and adds 900p fifth");
  Expect(Has(launcher, "at 900p or 1080p some draw more (sharper, heavier)."), "the option's text names 900p");
  Expect(Has(launcher, "xbox360ps5::video_mode::ForChoice(internal_resolution);") &&
             Has(launcher, "cvars::custom_internal_display_resolution_x = video_mode.width;") &&
             Has(launcher, "cvars::custom_internal_display_resolution_y = video_mode.height;") &&
             !Has(launcher, "internal_resolution & 3"),
         "Apply sets both sizes from ForChoice, no \"& 3\"");
  Expect(Has(launcher, "else if (key == \"internal_resolution\") internal_resolution = "
                       "xbox360ps5::video_mode::LoadedChoice(value);"),
         "Load reads the saved value through LoadedChoice");
  const std::string header = Read("include/xbox360ps5/launcher.hpp");
  Expect(Has(header, "int internal_resolution = 0;"), "the default is still 0 (720p)");
  // Fix 117's option is untouched: its own key (fix 163 makes Automatic, 2, its recommended value).
  Expect(Has(launcher, "{\"draw_scale\", &S::draw_scale, 0, W::start, 0, true, \"Resolution scale (test)\",") ||
             Has(launcher, "{\"draw_scale\", &S::draw_scale, 0, W::start, 2, true, \"Resolution scale (test)\","),
         "fix 117's draw_scale option unchanged (or with fix 163's Automatic recommended)");

  const std::string video = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc");
  const size_t fn = video.find("void VdQueryVideoMode(X_VIDEO_MODE* video_mode,");
  const size_t console = video.find("XCONFIG_USER_CATEGORY, XCONFIG_USER_AV_COMPOSITE_SCREENSZ));", fn);
  const size_t custom = video.find("if (custom_mode) {", fn);
  const size_t wide = video.find("const auto is_widescreen = IsWidescreen(kernel_state(), resolution);", fn);
  const size_t logged = video.find("if (!video_mode_logged.exchange(true, std::memory_order_relaxed)) {", fn);
  Expect(fn != std::string::npos && console != std::string::npos && custom != std::string::npos &&
             wide != std::string::npos && console < custom && custom < wide,
         "VdQueryVideoMode takes the custom size after the console's, before deciding widescreen");
  Expect(Has(video, "const xbox360ps5::video_mode::Mode reported_mode =\n"
                    "      xbox360ps5::video_mode::LaunchMode();") &&
             Has(video, "const bool custom_mode = reported_mode.reported;") &&
             Has(video, "resolution = Resolution(uint16_t(reported_mode.size.width),"),
         "fix 140b: only the table's modes are reported (the default path is the console's own size)");
  Expect(Has(video, "const auto is_widescreen = IsWidescreen(kernel_state(), resolution) ||\n"
                    "                             reported_mode.widescreen_forced;"),
         "fix 140b: the table's widescreen flag (480p 16:9)");
  // Fix 140c.
  Expect(Has(video, "       X_VIDEO_FLAGS::Widescreen) ||\n"
                    "      xbox360ps5::video_mode::LaunchMode().widescreen_forced;"),
         "fix 140c: the scaler's widescreen flag includes the reported mode's (480p shown 16:9)");
  const std::string game_main = Read("src/canary_game_main.cpp");
  Expect(Has(game_main, "        settings.Apply();\n"
                        "        // Fix 140c: the video mode the game is told until it starts again.\n"
                        "        xbox360ps5::video_mode::LatchAtLaunch(cvars::custom_internal_display_resolution_x,\n"
                        "                                              cvars::custom_internal_display_resolution_y);"),
         "fix 140c: the game start latches the size right after applying the settings");
  Expect(Has(game_main, "LatchAtLaunch(") && game_main.find("LatchAtLaunch(") == game_main.rfind("LatchAtLaunch("),
         "fix 140c: latched only at the game start (in-game changes do not)");
  Expect(Has(game_main, "xbox360ps5::video_mode::TuningMayNotFit(") &&
             Has(game_main, "Resolution scale (fix 140c): this game's 2x tuning was measured at 720p"),
         "fix 140c: the tuning warning");
  Expect(logged != std::string::npos && logged > wide && Has(video, "xbox360ps5::video_mode::Line("),
         "the first answer is logged once");
  Expect(Has(video, "DECLARE_uint32(custom_internal_display_resolution_x);") &&
             Has(video, "DECLARE_uint32(custom_internal_display_resolution_y);"),
         "the core's sizes declared in the kernel");
}

}  // namespace

int main() {
  CheckHeader();
  CheckRealModes();
  CheckLaunchLatch();
  CheckTree();
  if (failures) {
    std::printf("check-video-mode: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-video-mode: ok\n");
  return 0;
}
