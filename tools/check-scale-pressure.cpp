// SPDX-License-Identifier: MIT
// Fix 162c (tools/source-1.0.2/fixes/162c-auto-scale-one-x-list.patch): Automatic starts Kinect Sports
// at 1x (a compiled-in list; an explicit 2x still gives 2x), and the 10 s line of fix 117 counts the
// memory pressure that may break a 2x picture (Kinect Sports' replay artifacts on 1.0.6-work-fix162a).
//  (a) resolution_scale.hpp's AutomaticStartsAtOneX (4D5308C9 in either case; not Season Two, GTA IV,
//      no title, a short ID; in fix 162c, which applies before fix 163) and auto_scale.hpp's Resolve
//      (fix 163, which asks it): Automatic gives it 1x "Automatic: 1x (compiled-in list)", an explicit
//      2x gives 2x, 1x stays 1x, other games still 2x, the launcher 2x;
//  (b) include/xbox360ps5/resolution_scale.hpp: NoteTextureFree counts a scaled texture made under the
//      1.5 GiB render target floor (not above it, not with free memory unknown), NoteTransfer a
//      transfer between a target at 1x and one at 2x (not between two of the same scale); the 10 s
//      line ends with both only once either happened (otherwise it is fix 117's line as it was);
//  (c) the tree: the Vulkan texture cache counts its scaled textures made under the floor, the Vulkan
//      render target cache each colour or depth transfer's scales.
// Run from the assembled tree's root.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/auto_scale.hpp"
#include "xbox360ps5/resolution_scale.hpp"

namespace {
namespace as = xbox360ps5::auto_scale;
namespace rs = xbox360ps5::resolution_scale;
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Squeezed(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string out;
  for (char c : buffer.str())
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
  return out;
}

void List() {
  Expect(rs::AutomaticStartsAtOneX("4D5308C9") && rs::AutomaticStartsAtOneX("4d5308c9"), "(a) Kinect Sports (either case) is listed");
  Expect(!rs::AutomaticStartsAtOneX("4D5309D6") && !rs::AutomaticStartsAtOneX("545407F2") && !rs::AutomaticStartsAtOneX("") &&
             !rs::AutomaticStartsAtOneX("4D5308C"),
         "(a) Season Two, GTA IV, no title, a short ID are not");
  const as::Start automatic = as::Resolve(as::kAutomatic, "4D5308C9", false);
  Expect(automatic.choice == 0 && automatic.why == "Automatic: 1x (compiled-in list)",
         "(a) Automatic: Kinect Sports starts at 1x without trying 2x (" + automatic.why + ")");
  Expect(as::Resolve(1, "4D5308C9", false).choice == 1 && as::Resolve(0, "4D5308C9", false).choice == 0,
         "(a) an explicit 2x still gives 2x, 1x stays 1x");
  Expect(as::Resolve(as::kAutomatic, "545407F2", false).choice == 1 && as::Resolve(as::kAutomatic, "", false).choice == 1,
         "(a) other games and the launcher: Automatic still 2x");
}

void Counters() {
  rs::Counters c;
  c.scale = 2;
  c.sparse = true;
  rs::DriverMemory::Totals driver;
  driver.bytes = uint64_t(1500) << 20;
  const std::string quiet = rs::Line(c, driver, 1200ll << 20);
  Expect(quiet.find("transfers between") == std::string::npos && quiet.find("made under the") == std::string::npos &&
             quiet.size() > 4 && quiet.compare(quiet.size() - 4, 4, "MiB)") == 0,
         "(b) nothing happened: fix 117's line as it was");
  rs::NoteTextureFree(c, 1200ll << 20);
  rs::NoteTextureFree(c, 2000ll << 20);
  rs::NoteTextureFree(c, -1);
  Expect(c.textures_under_floor == 1, "(b) a scaled texture under the 1.5 GiB floor counted; above it or unknown not");
  rs::NoteTransfer(c, true, false);
  rs::NoteTransfer(c, false, true);
  rs::NoteTransfer(c, false, false);
  rs::NoteTransfer(c, true, true);
  Expect(c.mixed_transfers == 2, "(b) transfers between 1x and 2x counted both ways; same scale not");
  const std::string line = rs::Line(c, driver, 1200ll << 20);
  const std::string tail = "; scaled textures made under the 1536 MiB floor 1, transfers between 1x and 2x targets 2";
  Expect(line.size() > tail.size() && line.compare(line.size() - tail.size(), tail.size(), tail) == 0 &&
             line.compare(0, quiet.size(), quiet) == 0,
         "(b) the line ends with both: " + line.substr(quiet.size()));
}

void Wiring() {
  const std::string tex = Squeezed(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_texture_cache.cc");
  const size_t scaled = tex.find("if(key.scaled_resolve){xbox360ps5::resolution_scale::NoteScaledTexture(");
  const size_t under = tex.find("xbox360ps5::resolution_scale::NoteTextureFree(xbox360ps5::resolution_scale::counters,"
                                "xbox360ps5::resolution_scale::DirectFreeRecent());",
                                scaled);
  Expect(scaled != std::string::npos && under != std::string::npos && under - scaled < 600,
         "(c) the texture cache counts its scaled textures made under the floor");
  const std::string rt = Squeezed(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc");
  const size_t key = rt.find("new_transfer_shader_key.source_scale_native=source_rt_key.scale_native;");
  const size_t note = rt.find("if(!j){xbox360ps5::resolution_scale::NoteTransfer(xbox360ps5::resolution_scale::counters,"
                              "bool(source_rt_key.scale_native),bool(dest_rt_key.scale_native));}",
                              key);
  Expect(key != std::string::npos && note != std::string::npos && note - key < 200,
         "(c) the render target cache counts each colour or depth transfer's scales");
}
}  // namespace

int main() {
#ifndef XBOX360PS5_AUTO_SCALE_ONE_X_LIST
  Expect(false, "fix 162c's auto_scale.hpp (XBOX360PS5_AUTO_SCALE_ONE_X_LIST)");
#endif
  List();
  Counters();
  if (std::ifstream(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_texture_cache.cc")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: Automatic starts Kinect Sports at 1x; the 117 line counts the memory pressure\n");
  return 0;
}
