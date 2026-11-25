// SPDX-License-Identifier: MIT
// Fix 162f (tools/source-1.0.2/fixes/162f-kinect-title-rule.patch): a Kinect title (its executable says
// it requires Kinect, the XEX flag fix 57 reads) starts at 1x under Automatic and, at 2x, keeps the
// Kinect library's camera classes (80x1 160x1 320x1) at 1x unless it has a list of its own.
//  (a) nui_image_scan.hpp's XexHeaderRequiresKinect on made-up headers: required yes; supported only
//      (optional Kinect), the other flags word, a count that leaves it out, not XEX2: no;
//  (b) the order: an explicit 1x or 2x wins over the Kinect rule, the Kinect rule over fix 162c's
//      compiled-in list, the list over plain Automatic (2x), the launcher (no game) 2x; a title noted
//      as not requiring Kinect any more falls back to the list or 2x;
//  (c) the classes: a Kinect title without a list of its own gets 80x1 160x1 320x1, one with its own
//      list keeps it, a title that is not a Kinect title none;
//  (d) the tree: the start and the launch note the chosen game's flag (covers.hpp GameRequiresKinect)
//      before its options are worked out, the launch logs it, and fix 163's Resolve asks
//      AutomaticOneXReason.
// Run from the assembled tree's root.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/auto_scale.hpp"
#include "xbox360ps5/nui_image_scan.hpp"
#include "xbox360ps5/resolution_scale.hpp"

namespace {
namespace as = xbox360ps5::auto_scale;
namespace rs = xbox360ps5::resolution_scale;
namespace m = xbox360ps5::motion;
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Squeezed(const char* path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string out;
  for (char c : buffer.str())
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
  return out;
}

void Flag() {
  std::vector<uint8_t> h(0x60, 0);
  auto put = [&h](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) h[at + i] = uint8_t(v >> (24 - 8 * i));
  };
  put(0, 0x58455832);  // XEX2
  put(0x14, 2);
  put(0x18, 0x00040006);
  put(0x1C, 0x40);
  put(0x20, 0x00030100);  // system flags 32
  put(0x24, 0x40);        // skeletal tracking required
  Expect(m::XexHeaderRequiresKinect(h.data(), h.size()), "(a) skeletal tracking required: a Kinect title");
  put(0x24, 0x80);
  Expect(!m::XexHeaderRequiresKinect(h.data(), h.size()), "(a) supported only (optional Kinect): not");
  put(0x24, 0x40);
  put(0x20, 0x00030000);
  Expect(!m::XexHeaderRequiresKinect(h.data(), h.size()), "(a) the other system flags word: not");
  put(0x20, 0x00030100);
  put(0x14, 1);
  Expect(!m::XexHeaderRequiresKinect(h.data(), h.size()), "(a) a header count that leaves it out: not");
  put(0x14, 2);
  put(0, 0x58455831);
  Expect(!m::XexHeaderRequiresKinect(h.data(), h.size()), "(a) not an XEX2 header: not");
}

void Order() {
  const std::string kinect_adventures = "4D5307E6";  // not on any list
  const std::string kinect_sports = "4D5308C9";      // on fix 162c's list
  const std::string gta4 = "545407F2";
  rs::NoteKinectTitle(kinect_adventures, true);
  rs::NoteKinectTitle(kinect_sports, true);
  rs::NoteKinectTitle(gta4, false);
  Expect(rs::KinectTitle("4d5307e6") && rs::KinectTitle(kinect_sports) && !rs::KinectTitle(gta4),
         "(b) the noted flags (title IDs in either case)");
  Expect(as::Resolve(1, kinect_adventures, false).choice == 1 && as::Resolve(0, gta4, false).choice == 0,
         "(b) an explicit 2x or 1x wins over the Kinect rule");
  const as::Start kinect = as::Resolve(as::kAutomatic, kinect_adventures, false);
  Expect(kinect.choice == 0 && kinect.why == "Automatic: 1x (Kinect title)",
         "(b) Automatic: a Kinect title starts at 1x (" + kinect.why + ")");
  const as::Start listed = as::Resolve(as::kAutomatic, kinect_sports, false);
  Expect(listed.choice == 0 && listed.why == "Automatic: 1x (Kinect title)",
         "(b) the Kinect rule before the compiled-in list (" + listed.why + ")");
  rs::NoteKinectTitle(kinect_sports, false);
  const as::Start list_only = as::Resolve(as::kAutomatic, kinect_sports, false);
  Expect(list_only.choice == 0 && list_only.why == "Automatic: 1x (compiled-in list)",
         "(b) then the compiled-in list (" + list_only.why + ")");
  Expect(as::Resolve(as::kAutomatic, gta4, false).choice == 1 && as::Resolve(as::kAutomatic, "", false).choice == 1,
         "(b) then plain Automatic: 2x; the launcher 2x");
  rs::NoteKinectTitle(kinect_adventures, false);
  Expect(as::Resolve(as::kAutomatic, kinect_adventures, false).choice == 1,
         "(b) a title noted as not requiring Kinect any more: 2x again");
}

void Classes() {
  const std::string kinect_adventures = "4D5307E6";
  rs::NoteKinectTitle(kinect_adventures, true);
  const auto classes = rs::CompiledNativeClasses(kinect_adventures);
  Expect(classes.size() == 3 && classes[0].pitch_px == 80 && classes[1].pitch_px == 160 &&
             classes[2].pitch_px == 320 && classes[0].samples == 1 && classes[2].samples == 1,
         "(c) a Kinect title without a list of its own: 80x1 160x1 320x1");
  rs::NoteKinectTitle("5454082B", true);  // made up: Red Dead Redemption noted as a Kinect title
  Expect(rs::CompiledNativeClasses("5454082B").size() == 8, "(c) a title with its own list keeps it");
  rs::NoteKinectTitle("5454082B", false);
  Expect(rs::CompiledNativeClasses("45410950").empty(), "(c) not a Kinect title, no list: none");
  rs::NoteKinectTitle(kinect_adventures, false);
  Expect(rs::CompiledNativeClasses(kinect_adventures).empty(), "(c) the flag gone: none");
}

void Wiring() {
  const std::string main = Squeezed("src/canary_game_main.cpp");
  const auto has = [&main](const std::string& text, size_t from = 0) {
    std::string squeezed;
    for (char c : text)
      if (c != ' ') squeezed += c;
    return main.find(squeezed, from);
  };
  Expect(has("#include \"xbox360ps5/covers.hpp\"") != std::string::npos, "(d) main includes covers.hpp");
  const size_t start_note = has("xbox360ps5::resolution_scale::NoteKinectTitle(settings_title, "
                                "xbox360ps5::GameRequiresKinect(start_game));");
  const size_t start_overrides = has("xbox360ps5::GameOverrides overrides = xbox360ps5::LoadGameOverrides(settings_title);");
  Expect(start_note != std::string::npos && start_overrides != std::string::npos && start_note < start_overrides,
         "(d) a start straight into a game notes its flag before its options");
  const size_t chosen = has("settings_title = log_id;");
  const size_t launch_note = has("const bool requires_kinect = xbox360ps5::GameRequiresKinect(game);", chosen);
  const size_t launch_options = has("overrides = xbox360ps5::LoadGameOverrides(settings_title);", chosen);
  const size_t line = has("\"Resolution scale (fix 162f): {} requires Kinect:", chosen);
  Expect(chosen != std::string::npos && launch_note != std::string::npos && launch_options != std::string::npos &&
             line != std::string::npos && launch_note < launch_options && line < launch_options,
         "(d) the launch notes the chosen game's flag and logs it before its options");
  const std::string auto_scale = Squeezed("include/xbox360ps5/auto_scale.hpp");
  Expect(auto_scale.find("if(constchar*why=xbox360ps5::resolution_scale::AutomaticOneXReason(title_id))return{0,"
                         "std::string(\"Automatic:1x(\")+why+\")\"};") != std::string::npos,
         "(d) fix 163's Resolve asks AutomaticOneXReason");
}
}  // namespace

int main() {
#ifndef XBOX360PS5_KINECT_TITLE_RULE
  Expect(false, "fix 162f's resolution_scale.hpp (XBOX360PS5_KINECT_TITLE_RULE)");
#endif
  Flag();
  Order();
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
  std::printf("PASS: Kinect titles: Automatic at 1x, the camera classes at 2x, the order of the rules\n");
  return 0;
}
