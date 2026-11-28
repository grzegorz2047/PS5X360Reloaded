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
//  fix 162h: Grand Theft Auto V, Battlefield 3, Tekken Tag Tournament 2 and Red Dead Redemption listed
//      (they hit fix 117b's floor at 2x in our logs), an explicit 2x still 2x; GTA IV not listed.
//  fix 197: Halo: Reach listed the same way (classes kept at 1x and noise at 2x on 1.0.7_candidate_1,
//      clean at 1x).
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
#ifdef XBOX360PS5_KINECT_SPORTS_2
  // Fix 162d: Kinect Sports: Season Two too.
  Expect(rs::AutomaticStartsAtOneX("4D5309D6") && rs::AutomaticStartsAtOneX("4d5309d6") &&
             as::Resolve(as::kAutomatic, "4D5309D6", false).choice == 0 && as::Resolve(1, "4D5309D6", false).choice == 1,
         "(a) fix 162d: Season Two listed: Automatic 1x, an explicit 2x still 2x");
  Expect(rs::AutomaticStartsAtOneX("4D4A07F0") && as::Resolve(as::kAutomatic, "4d4a07f0", false).choice == 0,
         "(a) fix 162d: Zumba Fitness Core listed: Automatic 1x");
#ifdef XBOX360PS5_AUTO_ONE_X_FLOOR_TITLES
  // Fix 162h: the games our logs show hitting fix 117b's floor at 2x.
  for (const char* id : {"545408A7", "45410950", "4E4D0859", "5454082B", "545408a7"}) {
    const as::Start start = as::Resolve(as::kAutomatic, id, false);
    Expect(rs::AutomaticStartsAtOneX(id) && start.choice == 0 && start.why == "Automatic: 1x (compiled-in list)" &&
               as::Resolve(1, id, false).choice == 1,
           std::string("(a) fix 162h: ") + id + " listed: Automatic 1x (" + start.why + "), an explicit 2x still 2x");
  }
  Expect(!rs::AutomaticStartsAtOneX("545407F2") && as::Resolve(as::kAutomatic, "545407F2", false).choice == 1,
         "(a) fix 162h: GTA IV (five 2x logs, the floor never hit) stays at 2x under Automatic");
#endif
#ifdef XBOX360PS5_AUTO_ONE_X_HALO_REACH
  // Fix 197: Halo: Reach (4D53085B) kept classes at 1x at 2x and filled with noise; clean at 1x.
  for (const char* id : {"4D53085B", "4d53085b"}) {
    const as::Start start = as::Resolve(as::kAutomatic, id, false);
    Expect(rs::AutomaticStartsAtOneX(id) && start.choice == 0 && start.why == "Automatic: 1x (compiled-in list)" &&
               as::Resolve(1, id, false).choice == 1 && as::Resolve(0, id, false).choice == 0,
           std::string("(a) fix 197: Halo: Reach ") + id + " listed: Automatic 1x (" + start.why +
               "), an explicit 2x still 2x, 1x stays 1x");
  }
  // Other Halo games are not listed (Halo 3 4D5307E6, Halo 4 4D530919, Halo CE Anniversary 4D5309B1).
  for (const char* id : {"4D5307E6", "4D530919", "4D5309B1"})
    Expect(!rs::AutomaticStartsAtOneX(id) && as::Resolve(as::kAutomatic, id, false).choice == 1,
           std::string("(a) fix 197: ") + id + " (another Halo) still 2x under Automatic");
#endif
  Expect(!rs::AutomaticStartsAtOneX("545407F2") && !rs::AutomaticStartsAtOneX("") && !rs::AutomaticStartsAtOneX("4D5308C"),
         "(a) GTA IV, no title, a short ID are not");
#else
  Expect(!rs::AutomaticStartsAtOneX("4D5309D6") && !rs::AutomaticStartsAtOneX("545407F2") && !rs::AutomaticStartsAtOneX("") &&
             !rs::AutomaticStartsAtOneX("4D5308C"),
         "(a) Season Two, GTA IV, no title, a short ID are not");
#endif
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
