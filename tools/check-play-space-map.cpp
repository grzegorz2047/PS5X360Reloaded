// Fix 194: the play space as a Kinect game sees it, in the motion panel
// (include/xbox360ps5/play_space_map.hpp).
//
//   check-play-space-map <english/lang folder>     run from an assembled tree's root
//
// Checks, on frames built as the phone sends them and normalised as the console does
// (body_scale.hpp): the place the game gets (the hip centre in sensor metres, SkeletonSpace's
// sign and clamp), the guidance thresholds (depth, sides, the view's edge, the whole body, a body
// lying sideways), the map's geometry (field of view, sweet spot, the dot and its clamp, both panel
// places), the front view (upright, centred on the hips, the old place without the map), the
// texts in every language file, and the tree's overlay and option.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/body_scale.hpp"
#include "xbox360ps5/phone_qr_panel.hpp"
#include "xbox360ps5/play_space_map.hpp"

namespace m = xbox360ps5::motion;
namespace ps = xbox360ps5::motion::play_space;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static bool Near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

static std::string ReadText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// A standing body as the phone sends it (pose.mjs order, metres, X picture-right, Y up, Z towards
// the camera, the hips at `hip`): upright, arms down, every joint seen.
static m::Frame Standing(float hip_x = 0, float hip_z = 0, float confidence = 1) {
  m::Frame f;
  f.tracked = true;
  f.session = 7;
  f.sequence = 1;
  const float at[20][2] = {
      {0, 0},       {0, 0.25f},    {0, 0.5f},     {0, 0.72f},     // hip centre, spine, shoulder centre, head
      {0.18f, 0.48f}, {0.22f, 0.2f}, {0.24f, -0.05f}, {0.25f, -0.12f},  // left shoulder .. hand
      {-0.18f, 0.48f}, {-0.22f, 0.2f}, {-0.24f, -0.05f}, {-0.25f, -0.12f},
      {0.1f, -0.02f}, {0.11f, -0.45f}, {0.12f, -0.85f}, {0.12f, -0.9f},  // left hip .. foot
      {-0.1f, -0.02f}, {-0.11f, -0.45f}, {-0.12f, -0.85f}, {-0.12f, -0.9f}};
  for (int n = 0; n < 20; ++n) f.joints[size_t(n)] = {hip_x + at[n][0], at[n][1], hip_z, confidence};
  return f;
}
// The same body turned in the picture by `degrees` (a phone on its side with its picture not turned).
static m::Frame Turned(float degrees) {
  m::Frame f = Standing();
  const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
  for (auto& j : f.joints) {
    const float x = j.x, y = j.y;
    j.x = x * c - y * s;
    j.y = x * s + y * c;
  }
  return f;
}

static void CheckPlace() {
  // The hips where the bridge puts the body (Z 0: 2.5 m), centred: the game sees them there.
  ps::Place p = ps::PlaceOf(Standing());
  CHECK(p.body && Near(p.x, 0) && Near(p.z, m::kBodyDistance) && p.hips_seen && p.whole_body);
  CHECK(p.tilt_known && p.tilt_degrees < 1);
  // Exactly as WriteSkeletonFrame writes the hip centre (SkeletonSpace: x mirrored, z = 2.5 - Z).
  for (const float x : {-1.0f, -0.3f, 0.0f, 0.4f, 1.2f})
    for (const float z : {-1.6f, -0.7f, 0.0f, 0.6f, 1.5f, 2.0f}) {
      const m::Frame f = Standing(x, z);
      float gx, gy, gz;
      m::SkeletonSpace(f.joints[0], gx, gy, gz);
      p = ps::PlaceOf(f);
      CHECK(Near(p.x, gx) && Near(p.z, gz) && Near(p.x, -x));
    }
  // The clamp the game gets: never nearer than 0.8 m or further than 4 m.
  CHECK(Near(ps::PlaceOf(Standing(0, 2.0f)).z, 0.8f) && Near(ps::PlaceOf(Standing(0, -3.0f)).z, 4.0f));
  // The phone's +X (picture right) is the player's left: the game's -X.
  CHECK(ps::PlaceOf(Standing(0.5f)).x < 0 && ps::PlaceOf(Standing(-0.5f)).x > 0);
  // Inferred hips (fix 106 sends them at 0.49) still place the body, as the game gets them.
  m::Frame f = Standing();
  for (int n : {0, 12, 13, 14, 15, 16, 17, 18, 19}) f.joints[size_t(n)].confidence = 0.49f;
  p = ps::PlaceOf(f);
  CHECK(p.body && !p.hips_seen && !p.whole_body && Near(p.z, 2.5f));
  // No hip centre at all (confidence 0: the game gets a zeroed joint): the spine; neither: no place.
  f = Standing(0.4f, 0.5f);
  f.joints[0].confidence = 0;
  p = ps::PlaceOf(f);
  CHECK(p.body && !p.hips_seen && Near(p.x, -0.4f) && Near(p.z, 2.0f));
  f.joints[1].confidence = 0;
  CHECK(!ps::PlaceOf(f).body);
  f = Standing();
  f.tracked = false;
  CHECK(!ps::PlaceOf(f).body && ps::Guide(ps::PlaceOf(f)).hints == ps::kNone && !ps::Guide(ps::PlaceOf(f)).good);
  // The phone's depth offset (Nike+: the page at its 3.8 m limit, hips at Z -1.3) is re-centred by
  // the console before the game: after a few seconds the game sees the hips at 2.5 m.
  m::BodyScaleState state;
  m::Frame out;
  for (uint32_t n = 1; n <= 900; ++n) {
    m::Frame in = Standing(0, -1.3f);
    in.sequence = n;
    m::ObserveBody(state, in);
    out = m::NormalizeBody(state, in);
  }
  p = ps::PlaceOf(out);
  CHECK(Near(p.z, 2.5f, 0.05f) && ps::Guide(p).good);
  std::printf("place: the hip centre as the game gets it, %.2f m after the console re-centres a 3.8 m phone\n", double(p.z));
}

static void CheckGuidance() {
  auto hints = [](float hip_x, float hip_z) { return ps::Guide(ps::PlaceOf(Standing(hip_x, hip_z))).hints; };
  // In the sweet spot (and at the bridge's 2.5 m, where the console keeps the hips): good.
  for (const float z : {0.0f, 0.3f, 0.7f, -0.09f}) {  // 2.5, 2.2, 1.8, 2.59 m
    const ps::Guidance g = ps::Guide(ps::PlaceOf(Standing(0, z)));
    CHECK(g.hints == ps::kNone && g.good);
  }
  CHECK(ps::Guide(ps::PlaceOf(Standing(0, 0.3f))).in_sweet_spot);
  // Depth: 0.1 m of slack either side of 1.8-2.5 m.
  CHECK(hints(0, 0.79f) == ps::kNone);           // 1.71 m
  CHECK(hints(0, 0.81f) == ps::kStepBack);       // 1.69 m
  CHECK(hints(0, 1.5f) == ps::kStepBack);        // 1.0 m
  CHECK(hints(0, 3.0f) == ps::kStepBack);        // nearer than the sensor sees: 0.8 m
  CHECK(hints(0, -0.11f) == ps::kStepCloser);    // 2.61 m
  CHECK(hints(0, -1.3f) == ps::kStepCloser);     // 3.8 m, the game's depth (before the console re-centres)
  // Sides: the player's right is the game's +X; too far right, move left.
  CHECK(hints(-0.49f, 0) == ps::kNone && hints(0.49f, 0) == ps::kNone);
  CHECK(hints(-0.51f, 0) == ps::kMoveLeft);      // phone -X: the player's right
  CHECK(hints(0.51f, 0) == ps::kMoveRight);
  CHECK(!ps::Guide(ps::PlaceOf(Standing(-0.51f, 0))).in_sweet_spot);
  // Near the sensor the view is narrow: half a body from its edge already counts.
  const float half = ps::HalfWidthAt(1.0f);
  CHECK(Near(half, 0.543f, 0.002f));
  CHECK(hints(-0.2f, 1.5f) == ps::kStepBack);
  CHECK(hints(-(half - ps::kEdgeMargin) - 0.02f, 1.5f) == (ps::kStepBack | ps::kMoveLeft));
  // Both together, in the order the panel shows them.
  CHECK(hints(0.7f, -0.5f) == (ps::kStepCloser | ps::kMoveRight));
  // Not the whole body (the feet inferred): said first, with the place's hints.
  m::Frame f = Standing(-0.8f, 1.0f);
  f.joints[15].confidence = f.joints[19].confidence = 0.4f;
  CHECK(ps::Guide(ps::PlaceOf(f)).hints == (ps::kWholeBody | ps::kStepBack | ps::kMoveLeft));
  // Lying sideways in the picture (the user's screenshot: a phone turned, its picture not): only
  // that is said; 45 degrees (a lean) is not; upside down is.
  CHECK(ps::Guide(ps::PlaceOf(Turned(90))).hints == ps::kSideways);
  CHECK(ps::Guide(ps::PlaceOf(Turned(-90))).hints == ps::kSideways);
  CHECK(ps::Guide(ps::PlaceOf(Turned(180))).hints == ps::kSideways);
  CHECK(ps::Guide(ps::PlaceOf(Turned(45))).hints == ps::kNone);
  CHECK(Near(ps::PlaceOf(Turned(90)).tilt_degrees, 90, 0.5f) && Near(ps::PlaceOf(Turned(30)).tilt_degrees, 30, 0.5f));
  // The keys, in order.
  const auto keys = ps::HintKeys(0x3F);
  CHECK(keys.size() == 6 && std::strcmp(keys[0], "The camera sees you sideways: stand the phone upright") == 0 &&
        std::strcmp(keys[1], "Show your whole body to the camera") == 0 && std::strcmp(keys[2], "Step back") == 0 &&
        std::strcmp(keys[3], "Step closer") == 0 && std::strcmp(keys[4], "Move left") == 0 &&
        std::strcmp(keys[5], "Move right") == 0);
  CHECK(ps::HintKeys(ps::kNone).empty());
  std::printf("guidance: depth 1.7-2.6 m, sides 0.5 m or half a body from the view's edge, whole body, sideways\n");
}

static void CheckMap() {
  for (const float shift : {0.0f, m::kQrPanelLeftShift}) {
    for (const bool bars : {false, true}) {
      const ps::MapView map = ps::MapFor(shift, bars);
      // Inside the panel's right half, above the foot line (and the reach bars).
      CHECK(map.left >= 1724 + shift && map.right <= 1900 + shift && map.top >= 185);
      CHECK(map.bottom <= (bars ? 565.0f : 640.0f) - ps::kMapFootRoom + 0.01f);
      CHECK(Near(map.pixels_per_metre, (ps::kMapRight - ps::kMapLeft) / 3.0f, 0.01f));  // x +-1.5 m fills it
      // The sensor at the top, the player's right on the right, further away lower down.
      const ps::Point sensor = ps::MapPoint(map, 0, 0), right = ps::MapPoint(map, 1, 2), far = ps::MapPoint(map, 0, 4);
      CHECK(Near(sensor.x, (map.left + map.right) / 2) && sensor.y > map.top && sensor.y < map.top + 10);
      CHECK(right.x > sensor.x && far.y > right.y && far.y < map.bottom);
      // The view: symmetric, 0.8-4 m, wider further back; the sweet spot inside it, 1.8-2.5 m.
      const ps::Quad view = ps::ViewQuad(map), sweet = ps::SweetQuad(map);
      CHECK(Near(view.corner[0].x + view.corner[3].x, 2 * sensor.x, 0.01f) && Near(view.corner[1].x + view.corner[2].x, 2 * sensor.x, 0.01f));
      CHECK(view.corner[3].x - view.corner[0].x < view.corner[2].x - view.corner[1].x);
      CHECK(Near(view.corner[0].y, ps::MapPoint(map, 0, 0.8f).y) && Near(view.corner[1].y, far.y));
      CHECK(Near(sweet.corner[0].y, ps::MapPoint(map, 0, 1.8f).y) && Near(sweet.corner[2].y, ps::MapPoint(map, 0, 2.5f).y));
      CHECK(sweet.corner[0].x > map.left && sweet.corner[2].x < map.right);
      CHECK(ps::kSweetHalfWidth + ps::kEdgeMargin < ps::HalfWidthAt(ps::kSweetNear));  // the camera sees all of it
      // The dot: at the place; kept on the map (and said so) when the place is off it.
      ps::Place p = ps::PlaceOf(Standing(-0.3f, 0.3f));  // 0.3 m to the right, 2.2 m
      bool outside = true;
      ps::Point dot = ps::DotPoint(map, p, &outside);
      CHECK(!outside && Near(dot.x, ps::MapPoint(map, 0.3f, 2.2f).x) && Near(dot.y, ps::MapPoint(map, 0.3f, 2.2f).y));
      CHECK(dot.x > sensor.x);
      p.x = 2.5f;
      dot = ps::DotPoint(map, p, &outside);
      CHECK(outside && dot.x <= map.right - 6 && dot.x > sensor.x);
      p.x = 0;
      p.z = 4.0f;
      dot = ps::DotPoint(map, p, &outside);
      CHECK(!outside && dot.y < map.bottom);
    }
  }
  std::printf("map: the view, the sweet spot and the dot inside the panel's right half, both places\n");
}

static void CheckFront() {
  // Without the map: exactly fix 63's drawing for a body at the centre (PanelMirrorX, 130 per
  // metre, the phone's Y 0 at y 405).
  const ps::FrontView old = ps::FrontFor(false, 0);
  for (const float x : {-0.4f, 0.0f, 0.25f})
    for (const float y : {-0.9f, 0.0f, 0.72f}) {
      const ps::Point p = ps::FrontPoint(old, m::Joint{x, y, 0, 1}, 0);
      CHECK(Near(p.x, m::PanelMirrorX(1725, x, 130), 0.01f) && Near(p.y, 405 - y * 130, 0.01f));
    }
  CHECK(ps::FrontFor(false, m::kQrPanelLeftShift).centre_x == 1725 + m::kQrPanelLeftShift);
  // With the map: the left half; a standing body fits, upright (head above hips above feet), the
  // player's right on the right, centred on its hips wherever it stands.
  for (const float shift : {0.0f, m::kQrPanelLeftShift}) {
    const ps::FrontView front = ps::FrontFor(true, shift);
    CHECK(front.left == 1550 + shift && front.right <= ps::kMapLeft + shift);
    const m::Frame f = Standing(-0.6f, -0.4f);
    const ps::Place place = ps::PlaceOf(f);
    const ps::Point head = ps::FrontPoint(front, f.joints[3], place.x), hip = ps::FrontPoint(front, f.joints[0], place.x),
                    foot = ps::FrontPoint(front, f.joints[15], place.x), right_hand = ps::FrontPoint(front, f.joints[11], place.x);
    CHECK(head.y < hip.y && hip.y < foot.y && Near(hip.x, front.centre_x));
    CHECK(right_hand.x > hip.x);
    CHECK(head.y > 185 && foot.y < 565 && Near(foot.y, front.floor_y + 0.0f, 3.0f));
    for (const auto& joint : f.joints) {
      const ps::Point p = ps::FrontPoint(front, joint, place.x);
      CHECK(p.x > front.left && p.x < front.right);
    }
    // Arms straight out (DrawTargetArms: at least 0.55 m) stay inside the half.
    CHECK(front.centre_x - (0.2f + 0.65f) * front.pixels_per_metre > front.left &&
          front.centre_x + (0.2f + 0.65f) * front.pixels_per_metre < front.right);
  }
  // A body turned in the picture is drawn turned: the front view shows what the game gets.
  const m::Frame lying = Turned(90);
  const ps::FrontView front = ps::FrontFor(true, 0);
  const ps::Point head = ps::FrontPoint(front, lying.joints[3], 0), hip = ps::FrontPoint(front, lying.joints[0], 0);
  CHECK(std::fabs(head.x - hip.x) > std::fabs(head.y - hip.y));
  std::printf("front view: upright as the game gets it, centred on the hips; fix 63's place without the map\n");
}

static void CheckLanguages(const char* folder) {
  auto escaped = [](std::string text) {
    std::string out;
    for (const char c : text) out += c == ' ' ? std::string("\\s") : std::string(1, c);
    return out;
  };
  std::vector<std::string> keys;
  for (const char* key : ps::HintKeys(0x3F)) keys.push_back(key);
  for (const char* key : {ps::kGoodSpot, "Play-space map",
                          "With a phone as the Kinect, the motion panel also shows the room from above as the game sees it: the camera's view, the spot most games want, you as a dot, and where to step.",
                          "Shows from above where the game sees you, and where to step."})
    keys.push_back(key);
  int files = 0;
  for (const char* code : {"de", "en", "es-419", "es", "fr", "it", "ja", "ko", "nl", "pl", "pt-BR", "pt", "ru", "tr",
                           "zh-Hans", "zh-Hant"}) {
    const std::string lang = ReadText(std::string(folder) + "/" + code + ".txt");
    bool all = true;
    for (const auto& key : keys) {
      const bool found = lang.find("\n" + escaped(key) + " = ") != std::string::npos;
      if (!found) std::printf("missing in %s.txt: %s\n", code, key.c_str());
      all = all && found;
    }
    files += all;
  }
  CHECK(files == 16);
  // Polish first (the user's): the translations the panel shows.
  const std::string pl = ReadText(std::string(folder) + "/pl.txt");
  for (const char* line : {"Step\\sback = Odsuń\\ssię", "Step\\scloser = Podejdź\\sbliżej", "Move\\sleft = W\\slewo",
                           "Move\\sright = W\\sprawo", "Good\\sspot = Dobre\\smiejsce",
                           "Show\\syour\\swhole\\sbody\\sto\\sthe\\scamera = Pokaż\\skamerze\\scałe\\sciało"})
    CHECK(pl.find(line) != std::string::npos);
  std::printf("texts: %zu keys in all 16 language files\n", keys.size());
}

static void CheckTree() {
  const std::string game = ReadText("src/canary_game_main.cpp");
  const std::string launcher = ReadText("src/launcher.cpp");
  const std::string settings = ReadText("include/xbox360ps5/launcher.hpp");
  const std::string groups = ReadText("include/xbox360ps5/option_groups.hpp");
  if (game.empty() || launcher.empty()) {
    std::printf("no assembled tree here; the tree not checked\n");
    return;
  }
  CHECK(game.find("#include \"xbox360ps5/play_space_map.hpp\"") != std::string::npos);
  // The frame the game gets (with the emergency pose), only while the panel is drawn.
  const size_t branch = game.find("} else if (!panel_off) {");
  const size_t guest = game.find("const auto guest = xbox360ps5::motion::emergency_pose.Overlay(pose);");
  const size_t map = game.find("if (motion_map_) DrawPlaySpace(draw, scale, place, hands.valid);  // fix 194");
  const size_t foot = game.find("DrawFootLine(draw, scale, place, hands.valid);");
  const size_t bars = game.find("if (hands.valid) DrawReachBars(draw, scale, hands);");
  CHECK(branch != std::string::npos && guest != std::string::npos && map != std::string::npos && foot != std::string::npos);
  CHECK(branch < guest && guest < map && map < foot && foot < bars);
  CHECK(game.find("const ps::FrontView front = ps::FrontFor(motion_map_ != 0, shift_);") != std::string::npos);
  CHECK(game.find("ps::FrontPoint(front, guest.joints[n], place.x)") != std::string::npos);
  CHECK(game.find("DrawTargetArms(draw, scale, guest, hands, front, place.x)") < game.find("const int bones[][2]"));
  CHECK(game.find("xbox360ps5::motion::play_space::FrontPoint(front, shoulder, hip_x)") != std::string::npos);
  // The distance is the game's, not the phone page's.
  CHECK(game.find("const float metres = place.z;") != std::string::npos);
  CHECK(game.find("kBridgeBodyDistance - phone.joints[0].z") == std::string::npos);
  // Every hint through Tr with its literal key (check-english-source.py reads them), and Good spot.
  for (const char* key : ps::HintKeys(0x3F)) CHECK(game.find(std::string("Tr(\"") + key + "\")") != std::string::npos);
  CHECK(game.find("Tr(\"Good spot\")") != std::string::npos);
  // The option: on by default, per game, live; the overlay reads it.
  CHECK(game.find("settings.touchpad_menu,settings.motion_panel,settings.motion_map);") != std::string::npos);
  CHECK(game.find("const int& motion_map_;") != std::string::npos);
  CHECK(launcher.find("{\"motion_map\", &S::motion_map, 3, W::live, 1, true, \"Play-space map\",") != std::string::npos);
  CHECK(settings.find("int motion_map = 1;") != std::string::npos);
  CHECK(groups.find("{\"motion_map\", Group::other, nullptr, nullptr},") != std::string::npos);
  CHECK(groups.find("{\"motion_map\", \"Shows from above where the game sees you, and where to step.\"},") != std::string::npos);
  std::printf("tree: the panel draws the game's frame, the map behind its option and the game's distance\n");
}

int main(int argc, char** argv) {
  CheckPlace();
  CheckGuidance();
  CheckMap();
  CheckFront();
  if (argc > 1) CheckLanguages(argv[1]);
  else std::printf("no language folder given; texts not checked\n");
  CheckTree();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-play-space-map: all passed\n");
  return 0;
}
