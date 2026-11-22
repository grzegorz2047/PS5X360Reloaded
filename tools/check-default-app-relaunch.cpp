// SPDX-License-Identifier: MIT
// Fix 85 (tools/source-1.0.2/fixes/85-default-app-relaunch.patch): Red Dead Redemption (5454082B)
// went back to the launcher right after the storage device was chosen. It calls
// XamLoaderLaunchTitle with an empty name, XLAUNCH_KEYWORD_DEFAULT_APP: the title's own default
// executable again (a null name is the dashboard). Xenia took both for the dashboard, wrote no
// launch data, and the title switch's restart found nothing to start. An empty name now saves the
// launched game as what to run next.
//  (a) a model of XamModule::SaveLoaderData's path handling: an image or a folder is written as
//      it is with an empty launch path; an executable (.xex) is written as its folder with its
//      file name as the launch path, which SaveLoaderData joins back to the same executable;
//  (b) the assembled tree: the empty-name branch comes after the named one and before the
//      dashboard, only for a non-null name and a known launched game, on the console; it saves
//      the loader data before the restart hook; the restarted title starts the saved game.
// Run from the assembled tree's root (run-fix-checks.sh).
#include <cstdio>
#include <filesystem>
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
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}

// The launch path fix 85 sets for the launched game.
std::string LaunchPathFor(const std::filesystem::path& launched) {
  return launched.extension() == ".xex" ? launched.filename().string() : std::string();
}
// SaveLoaderData's host path (xam_module.cc): an executable's folder joined with the launch path.
std::filesystem::path Saved(std::filesystem::path host, const std::string& launch_path) {
  if (host.extension() == ".xex") {
    host.remove_filename();
    if (!launch_path.empty()) host /= launch_path;
  }
  return host;
}

void CheckModel() {
  const std::filesystem::path iso = "/mnt/ext1/xbox360/Red Dead Redemption (World).iso";
  Expect(LaunchPathFor(iso).empty() && Saved(iso, LaunchPathFor(iso)) == iso, "an image: itself, no launch path");
  const std::filesystem::path xex = "/data/homebrew/PPSA83600/games/RDR/default.xex";
  Expect(LaunchPathFor(xex) == "default.xex" && Saved(xex, LaunchPathFor(xex)) == xex,
         "an executable: its folder and its file name, joined back to itself");
  const std::filesystem::path folder = "/mnt/ext1/xbox360/RDR";
  Expect(LaunchPathFor(folder).empty() && Saved(folder, "") == folder, "a folder: itself");
}

void CheckTree() {
  const std::string info = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_info.cc");
  const size_t fn = info.find("void XamLoaderLaunchTitle_entry(lpstring_t raw_name_ptr, dword_t flags) {");
  const size_t named = info.find("  if (raw_name_ptr && !raw_name_ptr.value().empty()) {", fn);
  const size_t ps5 = info.find("#if XE_PLATFORM_PS5\n  } else if (raw_name_ptr && !loader_data.host_path.empty()) {", named);
  const size_t launch = info.find("launched.extension() == \".xex\"", ps5);
  const size_t save = info.find("    xam->SaveLoaderData();", launch);
  const size_t dash = info.find("    assert_always(\"Game requested exit to dashboard via XamLoaderLaunchTitle\");", save);
  const size_t hook = info.find("  if (XamModule::restart_hook) {", dash);
  Expect(fn != std::string::npos && named != std::string::npos && ps5 != std::string::npos && launch != std::string::npos &&
             save != std::string::npos && dash != std::string::npos && hook != std::string::npos,
         "an empty name after the named one and before the dashboard, saving the launched game before the restart");
  const std::string module = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_module.cc");
  const size_t saver = module.find("void XamModule::SaveLoaderData() {");
  const size_t xex = module.find("  if (host_path.extension() == \".xex\") {\n    host_path.remove_filename();", saver);
  const size_t parts = module.find("for (const auto& part : xe::utf8::split_path(launch_path)) {", xex);
  Expect(saver != std::string::npos && xex != std::string::npos && parts != std::string::npos,
         "SaveLoaderData joins an executable's folder with the launch path, as the model does");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t switched = main_code.find("const bool switched = game.empty() && !xam->loader_data().host_path.empty();");
  Expect(switched != std::string::npos, "the restarted title starts the saved game");
}

}  // namespace

int main() {
  CheckModel();
  CheckTree();
  if (failures) {
    std::printf("check-default-app-relaunch: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-default-app-relaunch: ok\n");
  return 0;
}
