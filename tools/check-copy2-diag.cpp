// SPDX-License-Identifier: MIT
// Fix 161 (tools/source-1.0.2/fixes/161-copy2-diagnostic.patch): what the resolves skipped at 2x
// with copy command 2 read (fix 154 counts them), so the owners' class can be kept at 1x (fix 117k).
//  (a) include/xbox360ps5/copy2_diag.hpp: ClassText writes a class fix 117k's parser reads back as
//      the same pitch and MSAA (every pitch of 1 to 255 tiles, 1x, 2x and 4x MSAA); the classes
//      noted once each, 16 at most; the line's text (source, destination, each owner with its
//      class, scaled or at 1x, none, at most 6 owners and how many more);
//  (b) the tree: vulkan_render_target_cache.cc includes it and, in fix 154's skip, lists the
//      owners of the source before the copy is dropped, and logs on fix 154's lines or a new class.
// Run from the assembled tree's root.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/copy2_diag.hpp"
#include "xbox360ps5/resolution_scale.hpp"

namespace {
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void Classes() {
  namespace diag = xbox360ps5::copy2_diag;
  namespace rs = xbox360ps5::resolution_scale;
  Expect(diag::ClassText(2, 0) == "160x1" && diag::ClassText(4, 2) == "160x4" && diag::ClassText(16, 1) == "1280x2",
         "(a) classes as fix 117k writes them: 160x1, 160x4, 1280x2");
  bool round_trip = true;
  for (uint32_t tiles = 1; tiles < 256; ++tiles) {
    for (uint32_t msaa = 0; msaa < 3; ++msaa) {
      const auto parsed = rs::ParseNativeClasses(diag::ClassText(tiles, msaa));
      round_trip = round_trip && parsed.size() == 1 &&
                   rs::PitchTiles(parsed[0].pitch_px, parsed[0].samples) == tiles &&
                   rs::MsaaIndex(parsed[0].samples) == msaa;
    }
  }
  Expect(round_trip, "(a) every class read back by fix 117k's parser as the same pitch and MSAA");
  diag::SeenClasses seen;
  Expect(seen.Note(2, 0) && !seen.Note(2, 0) && seen.Note(2, 2), "(a) a class noted once; another MSAA is another class");
  for (uint32_t tiles = 10; tiles < 40; ++tiles) seen.Note(tiles, 0);
  Expect(seen.size() == diag::SeenClasses::kMax && !seen.Note(200, 0), "(a) 16 classes at most");
}

void LineText() {
  namespace diag = xbox360ps5::copy2_diag;
  diag::Source s;
  s.width = 160;
  s.height = 120;
  s.base_tiles = 1938;
  s.pitch_tiles = 2;
  s.format = 6;
  s.dest_pitch = 160;
  s.dest_height = 128;
  const std::string none = diag::Line(5, s, {});
  Expect(Has(none, "Resolve copy command 2 (fix 161), skip 5: source 160x120 at 0,0, EDRAM base 1938, pitch 2 tiles, "
                   "1xMSAA, colour format 6; destination 160x128; owners: none"),
         "(a) the source and destination, no owner: " + none);
  std::vector<diag::Owner> owners = {{"RT @ 1938t, <2t>, 1xMSAA, k_8_8_8_8", 2, 0, false},
                                     {"RT @ 1938t, <2t>, 4xMSAA, kD24S8, native", 4, 2, true}};
  const std::string two = diag::Line(1, s, owners);
  Expect(Has(two, "owners: RT @ 1938t, <2t>, 1xMSAA, k_8_8_8_8 (117k class 160x1, scaled); "
                  "RT @ 1938t, <2t>, 4xMSAA, kD24S8, native (117k class 160x4, at 1x)"),
         "(a) each owner with its class, scaled or at 1x: " + two);
  owners.resize(9, owners[0]);
  const std::string many = diag::Line(1, s, owners);
  Expect(Has(many, "; 3 more") && many.size() < 1200, "(a) at most 6 owners, then how many more");
  s.depth = true;
  s.msaa_index = 2;
  Expect(Has(diag::Line(1, s, {}), "4xMSAA, depth format 6"), "(a) a depth source with 4xMSAA");
}

void Wiring() {
  std::ifstream input(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc", std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const std::string code = buffer.str();
  Expect(Has(code, "#include \"xbox360ps5/copy2_diag.hpp\"  // fix 161"), "(b) the Vulkan render target cache includes it");
  const size_t skip = code.find("\"Resolve copy command 2 is not supported with resolution scaling ({} \"");
  const size_t owners = code.find("GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used,", skip);
  const size_t when = code.find("if (count < 4 || !(count & 0xFFF) || new_class) {", skip);
  const size_t line = code.find("XELOGW(\"{}\", diag::Line(count + 1, source, owners));", skip);
  const size_t dropped = code.find("resolve_info.copy_dest_extent_length = 0;", skip);
  Expect(skip != std::string::npos && owners != std::string::npos && when != std::string::npos &&
             line != std::string::npos && dropped != std::string::npos && owners < when && when < line &&
             line < dropped,
         "(b) in fix 154's skip: the owners listed, logged on its lines or a new class, before the copy is dropped");
  Expect(Has(code, "new_class = diag::Seen().Note(owner_key.pitch_tiles_at_32bpp,"),
         "(b) each owner's class noted");
}
}  // namespace

int main() {
  Classes();
  LineText();
  if (std::ifstream(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: the skipped copy command 2 resolves name their source and its owners' classes\n");
  return 0;
}
