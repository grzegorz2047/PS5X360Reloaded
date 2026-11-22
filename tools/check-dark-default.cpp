// SPDX-License-Identifier: MIT
// Fix 98 (tools/source-1.0.2/fixes/98-dark-default.patch): the dark theme is the default. 1.0.2's
// marker ui-dark.on made the theme dark, so a new install (or any player who never switched)
// started in the light theme, and a missing marker could not tell "light chosen" from "never
// chosen". The player's light choice is now ui-light.on.
//  (a) reconstructed/dark_theme.hpp on a temporary title folder: no marker -> dark; ui-dark.on
//      (1.0.2's dark choice) -> dark; ui-light.on -> light, also beside ui-dark.on; switching from
//      dark writes ui-light.on and removes ui-dark.on (a build before the fix then shows light
//      too); switching back removes ui-light.on and writes ui-dark.on; a folder that cannot be
//      written gives false and keeps the theme;
//  (b) the assembled tree: Present() and Toggle() use the title's folder through DarkAt and
//      ToggleAt, the launcher's Settings row and the home's Theme slot still call Toggle(), and
//      the web settings page and the phone camera page are dark pages with no theme of their own.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../reconstructed/dark_theme.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
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

std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

namespace fs = std::filesystem;
namespace dt = xbox360ps5::dark_theme;

void Touch(const fs::path& path) { std::ofstream(path, std::ios::trunc) << "1\n"; }

void CheckMarkers() {
  std::random_device random;
  const fs::path folder = fs::temp_directory_path() / ("check-dark-default-" + std::to_string(random()));
  fs::create_directories(folder);
  const fs::path dark = folder / "ui-dark.on", light = folder / "ui-light.on";

  Expect(dt::DarkAt(folder), "a new install (no marker) is dark");
  Touch(dark);
  Expect(dt::DarkAt(folder), "1.0.2's dark choice stays dark");
  Touch(light);
  Expect(!dt::DarkAt(folder), "the light choice wins over ui-dark.on");
  fs::remove(dark);
  Expect(!dt::DarkAt(folder), "the light choice alone is light");
  fs::remove(light);

  // Dark (the default) -> light -> dark.
  Expect(dt::ToggleAt(folder, true), "switching to light succeeds");
  Expect(fs::exists(light) && !fs::exists(dark) && !dt::DarkAt(folder), "light: ui-light.on, no ui-dark.on");
  Expect(Read(light) == "1\n", "ui-light.on holds 1");
  Expect(dt::ToggleAt(folder, false), "switching back to dark succeeds");
  Expect(!fs::exists(light) && fs::exists(dark) && dt::DarkAt(folder), "dark: ui-dark.on, no ui-light.on");
  Touch(dark);
  Expect(dt::ToggleAt(folder, true) && !fs::exists(dark), "from 1.0.2's dark, light removes ui-dark.on");
  Expect(dt::ToggleAt(folder, false), "and back");

  // A folder that is not there: nothing can be written, the theme stays dark.
  const fs::path missing = folder / "missing";
  Expect(!dt::ToggleAt(missing, true) && dt::DarkAt(missing), "an unwritable folder: false, still dark");
  std::error_code error;
  fs::remove_all(folder, error);
}

void CheckTree() {
  const std::string header = Read("reconstructed/dark_theme.hpp");
  Expect(Has(header, "inline bool Present() { return DarkAt(XBOX360PS5_DATA_ROOT); }"),
         "the theme at start: dark unless the title's folder has the light choice");
  Expect(Has(header, "inline bool Toggle() { return ToggleAt(XBOX360PS5_DATA_ROOT, AtStart()); }"),
         "the switch works in the title's folder from this run's theme");
  Expect(Has(header, "kLightMarker = XBOX360PS5_DATA_ROOT \"/ui-light.on\""), "the light marker's path");
  Expect(Has(header, "return AtStart() ? \"Light theme on. The emulator will restart.\""),
         "the home's message still follows the theme before the switch");
  const std::string launcher = Read("src/launcher.cpp");
  Expect(Has(launcher, "if (dark_theme::Toggle()) { settings_.Save(); restart_ = true; }"),
         "the Settings sheet's Theme row switches");
  Expect(Has(launcher, "dashboard = dashboard && !dark_theme::AtStart();"), "the colours follow the theme");
  Expect(Has(Read("port/kinect_press.inc"), "const bool done = dark_theme::Toggle();"), "the home's Theme slot switches");
  // The phone's pages are dark already and have no theme of their own: nothing to follow.
  const std::string web = Read("platform/ps5/web_settings.cpp");
  Expect(Has(web, ":root{color-scheme:dark}") && Has(web, "background:#0a0c11"), "the web settings page is dark");
  Expect(!Has(web, "ui-dark") && !Has(web, "ui-light"), "the web settings page has no theme switch");
  const std::string phone = Read("port/phone_camera_page.hpp");
  Expect(Has(phone, "background:#05070a") && !Has(phone, "prefers-color-scheme: light"),
         "the phone camera page is dark");
}

}  // namespace

int main() {
  CheckMarkers();
  CheckTree();
  if (failures) {
    std::printf("check-dark-default: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-dark-default: ok\n");
  return 0;
}
