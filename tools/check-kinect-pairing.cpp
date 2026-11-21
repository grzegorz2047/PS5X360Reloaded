// SPDX-License-Identifier: MIT
// Fix 56 (tools/source-1.0.2/fixes/56-kinect-pairing.patch): pairing the phone as the Kinect sensor.
// The phone camera page (the official package's assets/kinect/index.html, served by the console)
// told a phone scanned from the library "Turn on Camera motion in the Kinect game's settings on
// the PS5 first." with the setting on (the game had not started), and the pairing key was only in
// small text at the bottom of the library.
//  (a) include/xbox360ps5/motion_input.hpp: the status JSON keeps its fields and adds
//      game_running and waiting_for: "game" before a game runs, "setting" in a running game with
//      the camera off, "" once it is on;
//  (b) the assembled tree: the HTTPS server serves the title's own camera page and its pairing
//      module (port/phone_camera_page.hpp; tools/check-kinect-page.mjs checks the page and the
//      state machine with node) instead of the package's assets/kinect/index.html; both servers
//      compare the key in lower case; the title says a game runs once it has launched; the
//      library shows the key large beside both Kinect QR codes.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/motion_input.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

namespace motion = xbox360ps5::motion;

void CheckStatus() {
  motion::SetEnabled(false);
  motion::game_running = false;
  std::string status = motion::Status();
  std::printf("   %s\n", status.c_str());
  Expect(status.rfind("{\"enabled\":false,\"game_running\":false,\"waiting_for\":\"game\",\"tracked\":false,", 0) == 0,
         "before a game: waiting for the game");
  // Fix 99b (1.0.5-work-fix99b2) adds fields after guest_skeleton_bodies.
  Expect(Has(status, "\"guest_camera_open\":false") &&
             (Has(status, "\"guest_skeleton_bodies\":0}") || Has(status, "\"guest_skeleton_bodies\":0,")),
         "the earlier fields are still there");
  motion::game_running = true;
  Expect(Has(motion::Status(), "\"game_running\":true,\"waiting_for\":\"setting\""), "a game with the camera off: the setting");
  motion::SetEnabled(true);
  Expect(Has(motion::Status(), "{\"enabled\":true,\"game_running\":true,\"waiting_for\":\"\""), "camera on: nothing to wait for");
  motion::SetEnabled(false);
  motion::game_running = false;
}

void CheckTree() {
  const std::string tls = Read("port/phone_camera_tls.cpp");
  const size_t own = tls.find("std::string page = phone_camera::kCameraPage;");
  const size_t meta = tls.find("content=\\\"ps5\\\">\");", own);
  const size_t module = tls.find("if (method == \"GET\" && target == \"/pairing.mjs\")", meta);
  const size_t serves = tls.find("phone_camera::kPairingModule);", module);
  Expect(Has(tls, "#include \"phone_camera_page.hpp\"") && own != std::string::npos && meta != std::string::npos &&
             module != std::string::npos && serves != std::string::npos,
         "the HTTPS server serves the title's page (marked as the console's) and pairing.mjs");
  Expect(!Has(tls, "ReadFile(std::string(kAssets) + \"index.html\")"), "not the package's index.html any more");
  Expect(Has(tls, "target == \"/pose.mjs\" || target == \"/kinect_tracker.mjs\" || target == \"/demo_body.mjs\""),
         "the tracker modules still come from the package");
  Expect(Has(tls, "std::string given = Header(head, \"X-Key\");") &&
             Has(tls, "for (char& c : given) c = char(std::tolower(static_cast<unsigned char>(c)));") &&
             Has(tls, "if (given != key) return Response(\"403 Forbidden\""),
         "the HTTPS server compares the key in lower case");
  const std::string settings = Read("platform/ps5/web_settings.cpp");
  const size_t parameter = settings.find("if (given.empty()) given = Parameter(query, \"k\");");
  const size_t lower = settings.find("for (char& c : given) c = char(std::tolower(static_cast<unsigned char>(c)));", parameter);
  const size_t accept = settings.find("web_key::Accept(given, key, official_key", parameter);
  Expect(parameter != std::string::npos && lower < accept && accept != std::string::npos,
         "the settings server lowers the key before comparing it");
  const std::string main_text = Read("src/canary_game_main.cpp");
  const size_t launched = main_text.find("XELOGI(\"GAME LAUNCH {:08X}\", status);");
  const size_t running = main_text.find("xbox360ps5::motion::game_running = status == 0;", launched);
  Expect(launched != std::string::npos && running != std::string::npos && running - launched < 200,
         "the title says a game runs once it has launched");
  const std::string launcher = Read("src/launcher.cpp");
  size_t keys = 0;
  for (size_t at = launcher.find("DrawQrCard(c, camera_qr,"); at != std::string::npos;
       at = launcher.find("DrawQrCard(c, camera_qr,", at + 1)) {
    const std::string after = launcher.substr(at, 300);
    if (Has(after, "c.Text(std::string(Tr(\"Code: \")) + web_key_,")) ++keys;
  }
  Expect(keys == 2, "the key beside both Kinect QR codes (" + std::to_string(keys) + ")");
}

}  // namespace

int main() {
  CheckStatus();
  CheckTree();
  if (failures) {
    std::printf("check-kinect-pairing: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-pairing: ok\n");
  return 0;
}
