// SPDX-License-Identifier: MIT
// Fix 147b (tools/source-1.0.2/fixes/147b-smartglass-app.patch): XAM app 0xF1, the SmartGlass
// service. Dance Central 3 sends it 0x00800002 about once a frame; with no such app every request
// failed at once as "app undefined" (15,389 log lines in four minutes).
//  (a) smartglass_app.hpp: app 0xF1; every LRC-class message (0x0080xxxx) answers "no companion
//      device" (ERROR_DEVICE_NOT_CONNECTED, 0x48F), anything else ERROR_INVALID_FUNCTION;
//  (b) the core (app_manager.cc): the app is defined with that ID, registered with the others,
//      answers through Answer() with the extended error set when asked, and logs each message ID
//      once with "SmartGlass (fix 147b)".
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/smartglass_app.hpp"

namespace {
namespace fs = std::filesystem;
namespace sg = xbox360ps5::smartglass;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void Answers() {
  Expect(sg::kAppId == 0xF1, "(a) the SmartGlass app is 0xF1");
  Expect(sg::kNoCompanionDevice == 0x48F, "(a) no companion device is ERROR_DEVICE_NOT_CONNECTED");
  for (uint32_t message : {0x00800002u, 0x00800003u, 0x00800004u, 0x00800005u, 0x0080FFFFu})
    Expect(sg::Answer(message) == sg::kNoCompanionDevice, "(a) an LRC message: no companion device");
  Expect(sg::Answer(0x00070001u) == 1u && sg::Answer(0u) == 1u && sg::Answer(0x00810002u) == 1u,
         "(a) another message: ERROR_INVALID_FUNCTION");
}

void Tree() {
  const std::string core = Read(".deps/xenia-canary/src/xenia/kernel/xam/app_manager.cc");
  Expect(core.find("xbox360ps5/smartglass_app.hpp") != std::string::npos,
         "(b) app_manager.cc includes smartglass_app.hpp");
  const size_t app = core.find("class SmartGlassApp");
  Expect(app != std::string::npos, "(b) the SmartGlass app is defined");
  Expect(core.find("xbox360ps5::smartglass::kAppId", app) != std::string::npos,
         "(b) with the SmartGlass app ID");
  Expect(core.find("xbox360ps5::smartglass::Answer(", app) != std::string::npos,
         "(b) it answers through Answer()");
  Expect(core.find("X_HRESULT_FROM_WIN32(", app) != std::string::npos,
         "(b) the extended error is set when asked");
  Expect(core.find("SmartGlass (fix 147b)", app) != std::string::npos,
         "(b) each message ID is logged once with fix 147b");
  const size_t registry = core.find("void AppManager::RegisterApps(");
  Expect(registry != std::string::npos &&
             core.find("std::make_unique<SmartGlassApp>(kernel_state)", registry) != std::string::npos,
         "(b) the app is registered with the others");
}

}  // namespace

int main() {
  Answers();
  if (fs::exists(".deps/xenia-canary")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("smartglass app: all checks passed\n");
  return 0;
}
