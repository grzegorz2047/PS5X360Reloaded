// SPDX-License-Identifier: MIT
// Fix 168 (tools/source-1.0.2/fixes/168-half-pixel-host.patch): GPU.resolution_scale_half_pixel_host
// (off by default) applies the D3D9 half-pixel offset as half a host pixel at resolution scale, so a
// primitive edge on a whole guest coordinate leaves no one-host-pixel seam (GTA IV at 2x).
//  (a) include/xbox360ps5/half_pixel.hpp: the offset (0.5 guest without the switch or at 1x; half a
//      host pixel with it: 0.25 guest at 2x, 1/6 at 3x);
//  (b) the coverage, by the top-left rule: at 1x an edge at guest 61 covers from host pixel 61; at 2x
//      the core's offset covers from 123 (host 122 of guest 61 left: GTA IV's minimap frame at x 61,
//      y 536 and its column at x 40), with the switch from 122 and up to guest 100 exactly; an edge
//      the game compensated (60.5) is exact without the switch and one host pixel early on the left,
//      one short on the right with it (the trade-off);
//  (c) the resolve fill: at a scale above 1 with its options on and D3D-zero centres only, never with
//      the switch;
//  (d) the tree: draw_util.cc defines the option off by default in section GPU, subtracts the
//      reduction after the sample position compensation for D3D-zero centres, and decides the fill
//      through ResolveFill.
// Run from the assembled tree's root.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/half_pixel.hpp"

namespace {
namespace hp = xbox360ps5::half_pixel;
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Span(const hp::Span& s) { return "[" + std::to_string(s.first) + ", " + std::to_string(s.end) + ")"; }

void Offset() {
  Expect(hp::GuestOffset(false, 2) == 0.5f && hp::GuestOffset(true, 1) == 0.5f && hp::GuestOffset(false, 1) == 0.5f,
         "(a) without the switch or at 1x: half a guest pixel");
  Expect(hp::GuestOffset(true, 2) == 0.25f && hp::GuestOffsetReduction(true, 2) == 0.25f,
         "(a) with the switch at 2x: half a host pixel (0.25 guest)");
  Expect(hp::GuestOffset(true, 3) > 0.1666f && hp::GuestOffset(true, 3) < 0.1667f, "(a) at 3x: 1/6 guest");
}

void Coverage() {
  // GTA IV's minimap frame: an edge on guest x 61 (and y 536), a box to guest 100.
  const hp::Span one = hp::Covered(61, 100, hp::GuestOffset(false, 1), 1);
  Expect(one.first == 61 && one.end == 100, "(b) 1x: guest 61..99 exactly " + Span(one));
  const hp::Span old = hp::Covered(61, 100, hp::GuestOffset(false, 2), 2);
  Expect(old.first == 123, "(b) 2x, the core's offset: from host 123, host 122 of guest 61 left (the seam) " + Span(old));
  const hp::Span fixed = hp::Covered(61, 100, hp::GuestOffset(true, 2), 2);
  Expect(fixed.first == 122 && fixed.end == 200, "(b) 2x with the switch: host 122..199, guest 61..99 exactly " + Span(fixed));
  const hp::Span column = hp::Covered(40, 1280, hp::GuestOffset(false, 2), 2);
  Expect(column.first == 81 && hp::Covered(40, 1280, hp::GuestOffset(true, 2), 2).first == 80,
         "(b) the scene column at guest 40: host 80 left without the switch, covered with it");
  const hp::Span row = hp::Covered(536, 700, hp::GuestOffset(true, 2), 2);
  Expect(row.first == 1072 && hp::Covered(536, 700, hp::GuestOffset(false, 2), 2).first == 1073,
         "(b) the frame's top at guest y 536: host row 1072 covered with the switch");
  const hp::Span compensated_old = hp::Covered(60.5, 99.5, hp::GuestOffset(false, 2), 2);
  const hp::Span compensated_new = hp::Covered(60.5, 99.5, hp::GuestOffset(true, 2), 2);
  Expect(compensated_old.first == 122 && compensated_old.end == 200,
         "(b) an edge the game compensated (60.5): exact without the switch " + Span(compensated_old));
  Expect(compensated_new.first == 121 && compensated_new.end == 199,
         "(b) with the switch: one host pixel early on the left, one short on the right (the trade-off) " +
             Span(compensated_new));
}

void Fill() {
  Expect(hp::ResolveFill(true, true, true, true, false), "(c) 2x, options on, D3D zero: the fill");
  Expect(!hp::ResolveFill(true, true, true, true, true), "(c) never with the switch");
  Expect(!hp::ResolveFill(false, true, true, true, false) && !hp::ResolveFill(true, false, true, true, false) &&
             !hp::ResolveFill(true, true, false, true, false) && !hp::ResolveFill(true, true, true, false, false),
         "(c) not at 1x, without its option, without the half-pixel offset, or with OpenGL centres");
}

void Wiring() {
  std::ifstream input(".deps/xenia-canary/src/xenia/gpu/draw_util.cc", std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string code;
  for (char c : buffer.str())
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') code += c;
  const size_t option = code.find("DEFINE_bool(resolution_scale_half_pixel_host,false,");
  Expect(code.find("#include\"xbox360ps5/half_pixel.hpp\"") != std::string::npos && option != std::string::npos &&
             code.find("\"GPU\");", option) != std::string::npos,
         "(d) the option, off by default, in section GPU");
  const size_t compensation = code.find("offset_add_xy[j]-=offset_fraction_xy[j]*");
  const size_t reduction = code.find("offset_add_xy[0]-=xbox360ps5::half_pixel::GuestOffsetReduction("
                                     "cvars::resolution_scale_half_pixel_host,args->draw_resolution_scale_x);");
  const size_t reduction_y = code.find("offset_add_xy[1]-=xbox360ps5::half_pixel::GuestOffsetReduction("
                                       "cvars::resolution_scale_half_pixel_host,args->draw_resolution_scale_y);");
  Expect(compensation != std::string::npos && reduction != std::string::npos && reduction_y != std::string::npos &&
             compensation < reduction,
         "(d) the reduction after the sample position compensation, on both axes");
  Expect(code.find("boolfill_half_pixel_offset=xbox360ps5::half_pixel::ResolveFill(") != std::string::npos &&
             code.find("cvars::resolution_scale_half_pixel_host);") != std::string::npos,
         "(d) the resolve fill through ResolveFill, with the switch");
}
}  // namespace

int main() {
  Offset();
  Coverage();
  Fill();
  if (std::ifstream(".deps/xenia-canary/src/xenia/gpu/draw_util.cc")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: the half-pixel offset in host pixels at resolution scale, behind its switch\n");
  return 0;
}
