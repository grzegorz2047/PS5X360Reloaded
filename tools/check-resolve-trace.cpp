// SPDX-License-Identifier: MIT
// Fix 167 (tools/source-1.0.2/fixes/167-resolve-trace.patch): with resolve-trace.txt in the data
// folder, each distinct render target resolve is logged once (at most 400 a session), to find what
// leaves GTA IV's thin seams at resolution scale 2x.
//  (a) include/xbox360ps5/resolve_trace.hpp: where a resolve starts in its surface (tiles from the
//      surface's base: 80x16 at 1x, 40 wide with 4xMSAA, 8 rows with 2x/4xMSAA, a 64bpp surface's
//      tiles counted twice, plus the offset in the base tile);
//  (b) a resolve noted once, another source, destination, pixel centre or fill a new one; 400 at most;
//  (c) the line (source area and where it starts, layouts, pixel centre, fill, scaled or 1x, the copy's
//      destination, clears);
//  (d) on only with the switch file (asked once a session);
//  (e) the tree: the Vulkan render target cache traces every resolve before fix 154's copy command 2
//      skip, from the data folder's switch, with the surface base and the pixel centre.
// Run from the assembled tree's root.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/resolve_trace.hpp"

namespace {
namespace rt = xbox360ps5::resolve_trace;
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

rt::Resolve Sample() {
  rt::Resolve r;
  r.original_base = 100;
  r.base_tiles = 100 + 16 * 2 + 3;  // row 2, column 3 of a 16-tile (1280 px) pitch
  r.pitch_tiles = 16;
  r.format = 6;
  r.offset_x = 8;
  r.offset_y = 8;
  r.width = 256;
  r.height = 256;
  r.fill = true;
  r.pixel_center = 0;
  r.scaled = true;
  r.copy = true;
  r.copy_command = 0;
  r.dest_base = 0x1A2B3000;
  r.dest_pitch = 256;
  r.dest_height = 256;
  r.dest_format = 6;
  return r;
}

void Position() {
  rt::Resolve r = Sample();
  Expect(rt::SurfaceX(r) == 3 * 80 + 8 && rt::SurfaceY(r) == 2 * 16 + 8, "(a) 1x: column 3, row 2, offset 8,8");
  r.msaa_index = 2;
  Expect(rt::SurfaceX(r) == 3 * 40 + 8 && rt::SurfaceY(r) == 2 * 8 + 8, "(a) 4xMSAA: 40 pixels a tile wide, 8 rows");
  r.msaa_index = 1;
  Expect(rt::SurfaceX(r) == 3 * 80 + 8 && rt::SurfaceY(r) == 2 * 8 + 8, "(a) 2xMSAA: 80 wide, 8 rows");
  r = Sample();
  r.is_64bpp = true;
  r.pitch_tiles = 32;
  r.base_tiles = 100 + 2 * (16 * 2 + 3);
  Expect(rt::SurfaceX(r) == 3 * 80 + 8 && rt::SurfaceY(r) == 2 * 16 + 8, "(a) 64bpp: tiles counted twice");
  r = Sample();
  r.base_tiles = r.original_base;
  r.offset_x = r.offset_y = 0;
  Expect(rt::SurfaceX(r) == 0 && rt::SurfaceY(r) == 0, "(a) at the surface's base: 0,0");
}

void Dedup() {
  rt::Trace trace;
  uint64_t number = 0;
  const rt::Resolve r = Sample();
  Expect(trace.Note(r, &number) && number == 1 && !trace.Note(r), "(b) a resolve once");
  rt::Resolve other = r;
  other.dest_base += 0x1000;
  Expect(trace.Note(other, &number) && number == 2, "(b) another destination: a new one");
  other = r;
  other.pixel_center = 1;
  Expect(trace.Note(other), "(b) another pixel centre: a new one");
  other = r;
  other.fill = false;
  Expect(trace.Note(other), "(b) the fill off: a new one");
  for (uint32_t i = 0; i < 1000; ++i) {
    other = r;
    other.dest_x = i * 8;
    trace.Note(other);
  }
  Expect(trace.lines() == rt::kMaxLines && !trace.Note([] {
           rt::Resolve last = Sample();
           last.dest_base = 0xFFFFF000;
           return last;
         }()),
         "(b) 400 lines at most");
  Expect(trace.seen() > rt::kMaxLines, "(b) the resolves seen are still counted");
}

void Text() {
  const rt::Resolve r = Sample();
  const std::string line = rt::Line(7, r);
  Expect(line == "Resolve (fix 167) #7: colour 256x256 at 248,40 of its surface (EDRAM base 135, surface base 100, "
                 "pitch 16 tiles, 1xMSAA, format 6); pixel centre D3D zero, half-pixel fill on, scaled; copy (command 0) "
                 "to 1A2B3000, 256x256 at 0,0, format 6",
         "(c) a copy: " + line);
  rt::Resolve clear = r;
  clear.copy = false;
  clear.depth = true;
  clear.depth_clear = true;
  clear.fill = false;
  clear.scaled = false;
  clear.pixel_center = 1;
  const std::string cleared = rt::Line(8, clear);
  Expect(cleared.find("#8: depth 256x256") != std::string::npos &&
             cleared.find("pixel centre OpenGL (.5), half-pixel fill off, at 1x; no copy, clears depth") != std::string::npos,
         "(c) a clear: " + cleared);
  rt::Resolve native = r;
  native.native = true;
  Expect(rt::Line(9, native).find(", scaled, copied at 1x; copy") != std::string::npos, "(c) copied at 1x");
}

void Switch() {
  const auto folder = std::filesystem::temp_directory_path() / "check-resolve-trace";
  std::error_code error;
  std::filesystem::remove_all(folder, error);
  std::filesystem::create_directories(folder, error);
  Expect(!rt::EnabledIn(folder), "(d) off without resolve-trace.txt");
  std::ofstream(folder / rt::kSwitchFile) << "on\n";
  Expect(rt::EnabledIn(folder), "(d) on with it");
  rt::enabled_state = -1;
  Expect(rt::Enabled(folder), "(d) Enabled asks the folder");
  std::filesystem::remove(folder / rt::kSwitchFile, error);
  Expect(rt::Enabled(folder), "(d) and keeps the answer for the session");
  std::filesystem::remove_all(folder, error);
}

void Wiring() {
  std::ifstream input(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc", std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string code;
  for (char c : buffer.str())
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') code += c;
  Expect(code.find("#include\"xbox360ps5/resolve_trace.hpp\"") != std::string::npos &&
             code.find("#include\"xbox360ps5/user_storage.hpp\"") != std::string::npos,
         "(e) the Vulkan render target cache includes it and the data folder");
  const size_t on = code.find("if(xbox360ps5::resolve_trace::Enabled(xbox360ps5::user_storage::Root())){");
  const size_t base = code.find("r.original_base=r.depth?resolve_info.depth_original_base:resolve_info.color_original_base;", on);
  const size_t centre = code.find("r.pixel_center=uint32_t(register_file().Get<reg::PA_SU_VTX_CNTL>().pix_center);", on);
  const size_t logged = code.find("if(trace::Session().Note(r,&number)){XELOGW(\"{}\",trace::Line(number,r));}", on);
  const size_t skip = code.find("xenos::CopyCommand::kConvertTo1111){", on);
  Expect(on != std::string::npos && base != std::string::npos && centre != std::string::npos &&
             logged != std::string::npos && skip != std::string::npos && logged < skip,
         "(e) every resolve traced (surface base, pixel centre) before fix 154's copy command 2 skip");
}
}  // namespace

int main() {
  Position();
  Dedup();
  Text();
  Switch();
  if (std::ifstream(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_render_target_cache.cc")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: the resolve trace: each distinct resolve once, where it starts, its fill and pixel centre\n");
  return 0;
}
