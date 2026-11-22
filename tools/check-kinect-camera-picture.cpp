// SPDX-License-Identifier: MIT
// Fix 119 (issue 15): the phone's camera picture as the virtual Kinect's colour stream.
//
//   check-kinect-camera-picture [language folder]     run from an assembled tree's root
//
//  (a) xbox360ps5/camera_picture.hpp, the picture: "XPC1", its size, sequence and planes; a wrong
//      magic, a size that does not match, odd, too small or too large sides are refused;
//  (b) the colours: full-range YCbCr to RGB as JPEG has it (red, white, black, grey); the mosaic's
//      sites (G R on even rows, B G on odd rows, as the USB Kinect's Bayer frames);
//  (c) the scaling to 640x480: a red picture gives red sites full and the others empty; a picture
//      white on its left half and black on its right half stays so, with the edge in the middle;
//      every size from 16x16 to 320x240 fills all 307200 bytes;
//  (d) the store: a picture while the setting is off is refused and drops the one kept; one that
//      does not parse is refused; the colour frame is the picture while it is under a second old
//      and wanted, untouched (the grey of before) when it is older, missing or no longer wanted,
//      and a picture no longer wanted is dropped;
//  (e) the tree: the colour stream (VirtualDepthCamera::CopyColorFrame) uses it with both
//      switches and falls back to 128; the HTTPS server takes /api/motion/picture (a larger
//      body only there), answers 204, 409 when off and 400, logs once, and its frame and status
//      answers carry "picture"; the setting (off by default, live) is applied with the motion
//      switch wherever that is; its texts are in every language file.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/camera_picture.hpp"

namespace m = xbox360ps5::motion;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::string ReadText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  std::string text = s.str();
  for (size_t at; (at = text.find('\r')) != std::string::npos;) text.erase(at, 1);
  return text;
}

// A picture `width` x `height` whose pixel (x, y) has the YCbCr colour(x, y).
template <typename Colour>
static std::string Make(uint32_t width, uint32_t height, uint32_t sequence, Colour colour) {
  std::string body = "XPC1";
  auto put16 = [&](uint32_t v) { body += char(v & 255); body += char(v >> 8 & 255); };
  auto put32 = [&](uint32_t v) { put16(v & 0xFFFF); put16(v >> 16); };
  put16(width); put16(height); put32(sequence); put32(0);
  std::string y, cb, cr;
  for (uint32_t row = 0; row < height; ++row)
    for (uint32_t x = 0; x < width; ++x) y += char(colour(x, row)[0]);
  for (uint32_t row = 0; row < height / 2; ++row)
    for (uint32_t x = 0; x < width / 2; ++x) {
      cb += char(colour(2 * x, 2 * row)[1]);
      cr += char(colour(2 * x, 2 * row)[2]);
    }
  return body + y + cb + cr;
}
struct Ycc {
  uint8_t v[3];
  uint8_t operator[](int n) const { return v[n]; }
};
static Ycc Solid(uint8_t y, uint8_t cb, uint8_t cr) { return {{y, cb, cr}}; }

int main(int argc, char** argv) {
  // (a) Parsing.
  {
    const std::string good = Make(160, 120, 77, [](uint32_t, uint32_t) { return Solid(10, 20, 30); });
    CHECK(good.size() == 16 + 160 * 120 * 3 / 2);
    m::Picture p;
    CHECK(m::ParsePicture(good, p));
    CHECK(p.width == 160 && p.height == 120 && p.sequence == 77);
    CHECK(p.y[0] == 10 && p.cb[0] == 20 && p.cr[0] == 30 && p.cr + 80 * 60 == reinterpret_cast<const uint8_t*>(good.data()) + good.size());
    std::string bad = good;
    bad[3] = '2';
    CHECK(!m::ParsePicture(bad, p));
    CHECK(!m::ParsePicture(good + "x", p) && !m::ParsePicture(good.substr(0, good.size() - 1), p));
    CHECK(!m::ParsePicture(good.substr(0, 10), p) && !m::ParsePicture("", p));
    auto solid = [](uint32_t, uint32_t) { return Solid(0, 128, 128); };
    CHECK(!m::ParsePicture(Make(8, 8, 1, solid), p));        // too small
    CHECK(!m::ParsePicture(Make(322, 240, 1, solid), p));    // too wide
    CHECK(!m::ParsePicture(Make(320, 242, 1, solid), p));    // too tall
    std::string odd = Make(18, 16, 1, solid);
    odd[4] = 17;                                              // odd width, size of 18
    CHECK(!m::ParsePicture(odd, p));
    CHECK(m::ParsePicture(Make(320, 240, 1, solid), p) && m::ParsePicture(Make(16, 16, 1, solid), p));
    CHECK(Make(320, 240, 1, solid).size() == m::kPictureMaxBytes);
  }

  // (b) Colours and sites.
  {
    auto near = [](int a, int b) { return a - b <= 2 && b - a <= 2; };
    CHECK(near(m::PictureChannel(76, 85, 255, 0), 254) && m::PictureChannel(76, 85, 255, 1) <= 2 &&
          m::PictureChannel(76, 85, 255, 2) <= 2);  // red
    for (int c = 0; c < 3; ++c) {
      CHECK(m::PictureChannel(255, 128, 128, c) == 255);
      CHECK(m::PictureChannel(0, 128, 128, c) == 0);
      CHECK(m::PictureChannel(128, 128, 128, c) == 128);
    }
    CHECK(m::MosaicChannel(0, 0) == 1 && m::MosaicChannel(1, 0) == 0);  // G R
    CHECK(m::MosaicChannel(0, 1) == 2 && m::MosaicChannel(1, 1) == 1);  // B G
    CHECK(m::MosaicChannel(638, 478) == 1 && m::MosaicChannel(639, 479) == 1);
  }

  // (c) Scaling.
  {
    m::Picture p;
    std::vector<uint8_t> out;
    const std::string red = Make(160, 120, 1, [](uint32_t, uint32_t) { return Solid(76, 85, 255); });
    CHECK(m::ParsePicture(red, p));
    m::PictureMosaic(p, out);
    CHECK(out.size() == m::kMosaicBytes);
    bool red_ok = true;
    for (uint32_t y = 0; y < 480; ++y)
      for (uint32_t x = 0; x < 640; ++x) {
        const uint8_t v = out[y * 640 + x];
        red_ok &= m::MosaicChannel(x, y) == 0 ? v >= 250 : v <= 3;
      }
    CHECK(red_ok);
    const std::string halves = Make(160, 120, 2, [](uint32_t x, uint32_t) { return Solid(x < 80 ? 255 : 0, 128, 128); });
    CHECK(m::ParsePicture(halves, p));
    m::PictureMosaic(p, out);
    bool halves_ok = true;
    for (uint32_t y = 0; y < 480; ++y) {
      for (uint32_t x = 0; x < 316; ++x) halves_ok &= out[y * 640 + x] == 255;
      for (uint32_t x = 324; x < 640; ++x) halves_ok &= out[y * 640 + x] == 0;
      halves_ok &= out[y * 640 + 318] > 128 && out[y * 640 + 321] < 128;
    }
    CHECK(halves_ok);
    for (const auto [w, h] : {std::pair{16u, 16u}, std::pair{320u, 240u}, std::pair{160u, 90u}, std::pair{64u, 48u}}) {
      const std::string flat = Make(w, h, 3, [](uint32_t, uint32_t) { return Solid(200, 128, 128); });
      CHECK(m::ParsePicture(flat, p));  // p points into flat
      out.clear();
      m::PictureMosaic(p, out);
      bool all = out.size() == m::kMosaicBytes;
      for (const uint8_t v : out) all &= v == 200;
      CHECK(all);
    }
  }

  // (d) The store.
  {
    m::PictureStore store;
    std::vector<uint8_t> out(4, 7);
    const std::string grey = Make(160, 120, 1, [](uint32_t, uint32_t) { return Solid(90, 128, 128); });
    CHECK(!store.Copy(1000, true, out) && out.size() == 4);                // nothing yet: untouched
    CHECK(store.Publish(grey, 1000, false) == m::PictureResult::off);      // the setting off
    CHECK(store.Publish("XPC1", 1000, true) == m::PictureResult::malformed);
    CHECK(store.Publish(grey, 1000, true) == m::PictureResult::accepted && store.pictures() == 1);
    CHECK(store.Copy(1500, true, out) && out.size() == m::kMosaicBytes && out[0] == 90 && out[307199] == 90);
    out.assign(4, 7);
    CHECK(!store.Copy(2000, true, out) && out.size() == 4);                // a second old: grey again
    CHECK(store.Publish(grey, 3000, true) == m::PictureResult::accepted);
    CHECK(!store.Copy(3100, false, out) && out.size() == 4);               // no longer wanted
    CHECK(!store.Copy(3200, true, out));                                   // and dropped
    CHECK(store.Publish(grey, 4000, true) == m::PictureResult::accepted);
    CHECK(store.Publish(grey, 4100, false) == m::PictureResult::off);      // off drops it too
    CHECK(!store.Copy(4200, true, out));
    CHECK(std::string(m::PictureJson(true)) == ",\"picture\":1" && std::string(m::PictureJson(false)) == ",\"picture\":0");
    CHECK(!m::picture_wanted.load());  // off by default
  }

  // (e) The tree.
  const std::string camera = ReadText("include/xbox360ps5/virtual_depth_camera.hpp");
  CHECK(camera.find("#include \"xbox360ps5/camera_picture.hpp\"") != std::string::npos);
  CHECK(camera.find("if (picture_store.Copy(SteadyMs(), picture_wanted.load() && enabled.load(), out)) return sequence;\n"
                    "    out.assign(kColorFrameBytes,kNoPictureLevel);return sequence;") != std::string::npos);
  CHECK(camera.find("if (sensor_.active()) return sensor_.CopyColor(after,out);") != std::string::npos);  // a real Kinect first
  CHECK(camera.find("out.assign(kColorFrameBytes,128)") == std::string::npos);

  const std::string tls = ReadText("port/phone_camera_tls.cpp");
  CHECK(tls.find("const bool picture = head.compare(0, 25, \"POST /api/motion/picture \") == 0;") != std::string::npos);
  CHECK(tls.find("picture ? motion::kPictureMaxBytes : 8192);") != std::string::npos);
  CHECK(tls.find("if (method == \"POST\" && target == \"/api/motion/picture\") {") != std::string::npos);
  CHECK(tls.find("motion::picture_store.Publish(body, motion::SteadyMs(),\n"
                 "                                                      motion::picture_wanted.load() && motion::enabled.load());") !=
        std::string::npos);
  CHECK(tls.find("if (result == motion::PictureResult::accepted) return Response(\"204 No Content\", \"text/plain\", \"\");") !=
        std::string::npos);
  CHECK(tls.find("result == motion::PictureResult::off ? \"409 Conflict\" : \"400 Bad Request\"") != std::string::npos);
  CHECK(tls.find("\"Virtual Kinect: the phone sends its camera picture for the game's colour camera (fix 119)\"") !=
        std::string::npos);
  CHECK(tls.find("motion::PictureJson(motion::picture_wanted.load()) + \"}\");") != std::string::npos);
  // The picture route is after the key check.
  CHECK(tls.find("if (given != key)") < tls.find("target == \"/api/motion/picture\""));
  const std::string input = ReadText("include/xbox360ps5/motion_input.hpp");
  CHECK(input.find("PictureJson(picture_wanted.load()) + \"}\";  // fix 119") != std::string::npos);

  const std::string launcher_h = ReadText("include/xbox360ps5/launcher.hpp");
  const std::string launcher = ReadText("src/launcher.cpp");
  const std::string game = ReadText("src/canary_game_main.cpp");
  CHECK(launcher_h.find("int motion_picture = 0;") != std::string::npos);
  CHECK(launcher.find("{\"motion_picture\", &S::motion_picture, 3, W::live, 0, true, \"Camera picture for the game\",") !=
        std::string::npos);
  size_t applied = 0, enabled_at = 0;
  for (size_t at = 0; (at = game.find("xbox360ps5::motion::SetEnabled(", at)) != std::string::npos; ++at) ++enabled_at;
  for (size_t at = 0; (at = game.find(".motion_picture != 0);  // fix 119", at)) != std::string::npos; ++at) ++applied;
  CHECK(enabled_at == 4 && applied == 4);
  CHECK(game.find("xbox360ps5::motion::picture_wanted.store(global.motion_picture != 0);  // fix 119") != std::string::npos);
  CHECK(game.find("#include \"xbox360ps5/camera_picture.hpp\"") != std::string::npos);

  if (argc > 1) {
    const char* kLanguages[] = {"de", "en", "es", "es-419", "fr", "it", "ja", "ko", "nl", "pl",
                                "pt", "pt-BR", "ru", "tr", "zh-Hans", "zh-Hant"};
    for (const char* language : kLanguages) {
      const std::string text = ReadText(std::string(argv[1]) + "/" + language + ".txt");
      CHECK(!text.empty());
      CHECK(text.find("\nCamera\\spicture\\sfor\\sthe\\sgame = ") != std::string::npos);
      CHECK(text.find("\nWith\\sa\\sphone\\sas\\sthe\\sKinect,\\sthe\\sphone\\scan\\salso\\ssend") != std::string::npos);
      if (failures) std::printf("  (language %s)\n", language);
    }
  }

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-camera-picture: ok\n");
  return 0;
}
