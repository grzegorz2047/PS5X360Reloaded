// SPDX-License-Identifier: MIT
// Fix 162a (tools/source-1.0.2/fixes/162a-kinect-sports-camera-classes.patch): Kinect Sports at 2x
// keeps the render target classes its Kinect camera pictures are copied from at 1x, so the core does
// those copies (copy command 2, only from targets at 1x) instead of skipping them; and it is off fix
// 150g's 1x list again (fix 163's Automatic is the fallback).
//  (a) include/xbox360ps5/resolution_scale.hpp: CompiledNativeClasses gives Kinect Sports (4D5308C9,
//      either case) exactly 80x1 160x1 320x1 - pitch 1, 2 and 4 tiles at 1xMSAA, the owners fix 161
//      logged on 1.0.6-work-fix161b - Red Dead Redemption keeps its list, other games none; applied
//      at 2x only (SetNativeClasses), not the 4xMSAA classes of the same pitch; the 117k line;
//  (b) TwoXUnfit no longer lists it, so ChoiceFor keeps its 2x;
//  (c) the tree: the launch still takes the compiled-in list when the console has no file of its own.
// Run from the assembled tree's root.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/resolution_scale.hpp"

namespace {
namespace rs = xbox360ps5::resolution_scale;
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

void Classes() {
#ifndef XBOX360PS5_KINECT_SPORTS_CAMERA_CLASSES
  Expect(false, "(a) fix 162a's resolution_scale.hpp (XBOX360PS5_KINECT_SPORTS_CAMERA_CLASSES)");
#endif
  const auto ks = rs::CompiledNativeClasses("4D5308C9");
  Expect(ks.size() == 3 && ks[0].pitch_px == 80 && ks[0].samples == 1 && ks[1].pitch_px == 160 &&
             ks[1].samples == 1 && ks[2].pitch_px == 320 && ks[2].samples == 1,
         "(a) Kinect Sports: exactly 80x1 160x1 320x1");
  Expect(rs::CompiledNativeClasses("4d5308c9").size() == 3, "(a) its title ID in either case");
  Expect(rs::PitchTiles(80, 1) == 1 && rs::PitchTiles(160, 1) == 2 && rs::PitchTiles(320, 1) == 4,
         "(a) the classes are pitch 1, 2 and 4 tiles, as fix 161's owners (RT @ 0t, <1t>/<2t>/<4t>, 1xMSAA)");
#ifdef XBOX360PS5_KINECT_SPORTS_2
  // Fix 162d: Kinect Sports: Season Two, the same engine, the same three classes.
  const auto ks2 = rs::CompiledNativeClasses("4d5309d6");
  Expect(ks2.size() == 3 && ks2[0].pitch_px == 80 && ks2[1].pitch_px == 160 && ks2[2].pitch_px == 320 &&
             ks2[0].samples == 1 && ks2[1].samples == 1 && ks2[2].samples == 1,
         "(a) fix 162d: Season Two (4D5309D6, either case): 80x1 160x1 320x1 too");
  Expect(rs::CompiledNativeClasses("5454082B").size() == 8 && rs::CompiledNativeClasses("545407F2").empty(),
         "(a) Red Dead Redemption keeps its list; GTA IV has none");
#else
  Expect(rs::CompiledNativeClasses("5454082B").size() == 8 && rs::CompiledNativeClasses("4D5309D6").empty() &&
             rs::CompiledNativeClasses("545407F2").empty(),
         "(a) Red Dead Redemption keeps its list; Season Two and GTA IV have none");
#endif

  Expect(rs::SetNativeClasses(ks, 1) == 0 && !rs::IsNativeClass(1, 0) && !rs::IsNativeClass(4, 0),
         "(a) at 1x nothing to keep");
  Expect(rs::SetNativeClasses(ks, 2) == 3 && rs::IsNativeClass(1, 0) && rs::IsNativeClass(2, 0) &&
             rs::IsNativeClass(4, 0),
         "(a) at 2x the three classes are kept at 1x");
  Expect(!rs::IsNativeClass(1, 2) && !rs::IsNativeClass(2, 2) && !rs::IsNativeClass(4, 2) &&
             !rs::IsNativeClass(8, 0) && !rs::IsNativeClass(16, 0),
         "(a) not the 4xMSAA classes of the same pitch, nor the wider targets (the 2x picture stays 2x)");
  rs::SetNativeClasses({}, 1);
  const std::string line = rs::NativeClassesLine(ks, "4D5308C9", "compiled-in");
  Expect(line.find("Resolution scale (fix 117k): classes at 1x for 4D5308C9 (compiled-in): 80x1 160x1 320x1") == 0,
         "(a) the start line: " + line);

  Expect(!rs::TwoXUnfit("4D5308C9") && rs::ChoiceFor(1, "4D5308C9") == 1,
         "(b) Kinect Sports is off fix 150g's 1x list: 2x chosen stays 2x");
}

void Wiring() {
  std::ifstream input("src/canary_game_main.cpp", std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string main;
  for (char c : buffer.str())
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') main += c;
  Expect(main.find("classes_text.empty()?xbox360ps5::resolution_scale::CompiledNativeClasses(settings_title)") !=
                 std::string::npos &&
             main.find("xbox360ps5::resolution_scale::SetNativeClasses(classes,xbox360ps5::resolution_scale::"
                       "ScaleForChoice(settings.draw_scale))") != std::string::npos,
         "(c) the launch takes the compiled-in list (no console file) and applies it at the game's scale");
}
}  // namespace

int main() {
  Classes();
  if (std::ifstream("src/canary_game_main.cpp")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: Kinect Sports' camera classes at 1x at 2x; 2x allowed for it again\n");
  return 0;
}
