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
    // 1.0.5-work-fix119f: held until a newer one comes, for up to 10 s (with 1 s the gaps between
    // pictures gave the game grey frames, and the replay flickered).
    CHECK(m::kPictureFreshMs == 10000);
    CHECK(store.Copy(2000, true, out) && out[0] == 90);                    // a second old: still the picture
    CHECK(store.Copy(10999, true, out) && out[0] == 90);
    out.assign(4, 7);
    CHECK(!store.Copy(11000, true, out) && out.size() == 4);               // 10 s old: grey again
    CHECK(store.Publish(grey, 13000, true) == m::PictureResult::accepted);
    CHECK(store.Copy(13100, true, out) && out[0] == 90);                   // a new one: the picture again
    out.assign(4, 7);
    CHECK(!store.Copy(13100, false, out) && out.size() == 4);              // no longer wanted
    CHECK(!store.Copy(13200, true, out));                                  // and dropped
    CHECK(store.Publish(grey, 4000, true) == m::PictureResult::accepted);
    CHECK(store.Publish(grey, 4100, false) == m::PictureResult::off);      // off drops it too
    CHECK(!store.Copy(4200, true, out));
    CHECK(std::string(m::PictureJson(true)) == ",\"picture\":1" && std::string(m::PictureJson(false)) == ",\"picture\":0");
    CHECK(!m::picture_wanted.load());  // off by default
  }

  // (d1) 1.0.5-work-fix119g: with two on their way the newest shown; a new page counts from 1.
  {
    m::PictureStore store;
    std::vector<uint8_t> out;
    auto shade = [](uint8_t y, uint32_t sequence) {
      return Make(16, 16, sequence, [y](uint32_t, uint32_t) { return Solid(y, 128, 128); });
    };
    CHECK(store.Publish(shade(50, 5), 1000, true) == m::PictureResult::accepted);
    CHECK(store.Publish(shade(60, 4), 1010, true) == m::PictureResult::accepted);  // older, came later
    CHECK(store.Copy(1020, true, out) && out[0] == 50);                            // not shown
    CHECK(store.Publish(shade(70, 6), 1030, true) == m::PictureResult::accepted);
    CHECK(store.Copy(1040, true, out) && out[0] == 70);
    CHECK(store.Publish(shade(80, 6), 1050, true) == m::PictureResult::accepted);  // the same number
    CHECK(store.Copy(1060, true, out) && out[0] == 70);
    CHECK(store.Publish(shade(90, 1), 2030, true) == m::PictureResult::accepted);  // a second later: a new page
    CHECK(store.Copy(2040, true, out) && out[0] == 90);
    CHECK(store.pictures() == 5 && m::kPictureRestartMs == 1000);
  }

  // (d3) 1.0.5-work-fix119h: the console's own time per picture POST on the line.
  {
    m::PictureStats stats;
    stats.Due(1000, true);
    stats.Served(3000, 4000);
    stats.Served(4000, 4400);
    std::string line = stats.Due(11000, true);
    CHECK(line.find("; per picture on the console: receive 3.5 ms, convert and store 4.2 ms") != std::string::npos);
    CHECK(stats.Due(21000, true).find("per picture on the console") == std::string::npos);  // none served: none told
  }

  // (d2) 1.0.5-work-fix119b: the 10 s line and the page's report.
  {
    CHECK(m::ReportNumber("{\"fps\":25,\"picture_on\":1,\"picture_sent\":512}", "picture_on") == 1);
    CHECK(m::ReportNumber("{\"picture_sent\":512,\"x\":1}", "picture_sent") == 512);
    CHECK(m::ReportNumber("{\"fps\":25}", "picture_on") == -1 && m::ReportNumber("", "picture_on") == -1);
    CHECK(m::ReportNumber("{\"picture_on\":x}", "picture_on") == -1);
    m::PictureStats stats;
    CHECK(stats.Due(1000, true).empty());                 // the first call starts the window
    CHECK(stats.Due(10999, true).empty());                // not 10 s yet
    std::string line = stats.Due(11000, true);            // nothing came: still a line, setting on
    CHECK(line == "Virtual Kinect: camera picture in 10 s (fix 119): 0 taken, refused 0 (setting off 0, not a picture 0); "
                  "colour frames 0 from the picture, 0 grey; setting on; phone page: no report (an older page, or not paired)");
    for (int n = 0; n < 47; ++n) stats.Received(160, 120);
    stats.Refused(true);
    stats.Refused(false);
    stats.Refused(false);
    for (int n = 0; n < 290; ++n) stats.Frame(true);
    for (int n = 0; n < 10; ++n) stats.Frame(false);
    stats.Page("{\"fps\":30,\"picture_on\":1,\"picture_sent\":512,\"picture_skipped\":31,\"picture_failed\":2,\"picture_wanted\":1}");
    stats.Page("{\"fps\":30}");                           // a report without them changes nothing
    line = stats.Due(21000, true);
    CHECK(line == "Virtual Kinect: camera picture in 10 s (fix 119): 47 taken (last 160x120), refused 3 (setting off 1, "
                  "not a picture 2); colour frames 290 from the picture, 10 grey; setting on; phone page: switch on, sees the "
                  "PS5 ask, 512 sent, 31 skipped for the network, 2 failed");
    // An older 119c page: no "picture_failed" nor "picture_wanted" (0 failed, the ask not told).
    stats.Page("{\"picture_on\":1,\"picture_sent\":5,\"picture_skipped\":0}");
    CHECK(stats.Due(22000, true).empty());
    CHECK(stats.Due(32000, true).find("phone page: switch on, 5 sent, 0 skipped for the network, 0 failed") !=
          std::string::npos);
    stats.Page("{\"picture_on\":1,\"picture_sent\":5,\"picture_skipped\":0,\"picture_wanted\":0}");
    line = stats.Due(42000, true);
    CHECK(line.find("switch on, sees no ask from the PS5, 5 sent") != std::string::npos);
    CHECK(line.find("per picture on the phone") == std::string::npos);  // no times reported
    // 1.0.5-work-fix119g: the page's average time per picture (capture, pack, upload).
    stats.Page("{\"picture_on\":1,\"picture_sent\":9,\"picture_skipped\":0,\"picture_failed\":0,\"picture_wanted\":1,"
               "\"picture_capture_ms\":12,\"picture_pack_ms\":3,\"picture_upload_ms\":36}");
    line = stats.Due(47000, true);
    CHECK(line.empty());
    line = stats.Due(52000, true);
    CHECK(line.find("0 failed; per picture on the phone: capture 12 ms, pack 3 ms, upload 36 ms") != std::string::npos);
    stats.Page("{\"picture_on\":1,\"picture_sent\":9,\"picture_skipped\":0}");  // no times this time: none told
    CHECK(stats.Due(62000, true).find("per picture") == std::string::npos);
    stats.Page("{\"picture_on\":0,\"picture_sent\":512,\"picture_skipped\":31}");
    for (int n = 0; n < 300; ++n) stats.Frame(false);
    line = stats.Due(72000, true);                       // the counters started again
    CHECK(line.find(": 0 taken (last 160x120), refused 0") != std::string::npos);
    CHECK(line.find("colour frames 0 from the picture, 300 grey; setting on; phone page: switch off, 512 sent") != std::string::npos);
    for (int n = 0; n < 300; ++n) stats.Frame(false);
    CHECK(stats.Due(82000, false).empty());               // setting off and nothing came: no line
    stats.Refused(true);
    CHECK(stats.Due(92000, false).find("setting off; ") != std::string::npos);  // a refusal is told
  }

  // (e) The tree.
  const std::string camera = ReadText("include/xbox360ps5/virtual_depth_camera.hpp");
  CHECK(camera.find("#include \"xbox360ps5/camera_picture.hpp\"") != std::string::npos);
  CHECK(camera.find("if (picture_store.Copy(SteadyMs(), picture_wanted.load() && enabled.load(), out)) {\n"
                    "      picture_stats.Frame(true);") != std::string::npos);
  CHECK(camera.find("    picture_stats.Frame(false);\n"
                    "    out.assign(kColorFrameBytes,kNoPictureLevel);return sequence;") != std::string::npos);
  const std::string pscam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  CHECK(pscam.find("++xbox360ps5::motion::guest_color_frames;\n"
                   "      // Fix 119b: what became of the phone's pictures, every 10 s (camera_picture.hpp).\n"
                   "      if (const std::string line = xbox360ps5::motion::picture_stats.Due(") != std::string::npos);
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
  CHECK(tls.find("motion::picture_stats.Received(taken.width, taken.height);") != std::string::npos);
  // Fix 119h: the POST's receive and answer timed; answers not held back (TCP_NODELAY where known).
  CHECK(tls.find("const auto head_at = std::chrono::steady_clock::now();") < tls.find("const auto body_at = std::chrono::steady_clock::now();"));
  CHECK(tls.find("motion::picture_stats.Served(us(body_at - head_at), us(std::chrono::steady_clock::now() - body_at));") !=
        std::string::npos);
  CHECK(tls.find("const auto body_at") < tls.find("const std::string reply = Route("));
  CHECK(tls.find("#if __has_include(<netinet/tcp.h>)") != std::string::npos);
  CHECK(tls.find("#ifdef TCP_NODELAY\n") != std::string::npos &&
        tls.find("setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));") != std::string::npos);
  CHECK(tls.find("motion::picture_stats.Refused(result == motion::PictureResult::off);") != std::string::npos);
  CHECK(tls.find("motion::picture_stats.Page(body);  // fix 119b") < tls.find("phone_latency::Recentred(body)"));
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
