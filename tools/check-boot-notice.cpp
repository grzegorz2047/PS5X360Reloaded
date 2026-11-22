// SPDX-License-Identifier: MIT
// Fix 93c (tools/source-1.0.2/fixes/93c-boot-notice.patch): the leaving notice across the restart.
// On 1.0.5-work-fix93b the console showed "Returning to the menu...", then black, then the menu:
// the system shows black from LoadExec until the new process draws. What was shown is written
// before LoadExec and the new process shows it again from its first frame with a presenter until
// the launcher's first frame (or a game's start); the start's stages are logged with their times.
//  (a) include/xbox360ps5/leaving_notice.hpp: Remember writes the kind and the time, nothing for
//      none; Take reads them back once (the file is gone after), and gives none for a missing
//      file, a time over two minutes old or in the future, an unknown kind, or a broken file;
//  (b) the assembled tree: the file taken at main's start with the restart's length logged, the
//      notice made and painted before the start-up picture goes, painted again before the library
//      scans, ended after the launcher's first frame and when a game's launch returns; written
//      before both LoadExec restarts (the return to the launcher and the title switch hook);
//      BOOT timing lines around the start's stages.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/leaving_notice.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
namespace notice = xbox360ps5::leaving_notice;

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
void WriteFile(const fs::path& path, const std::string& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << data;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a);
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}

void CheckFile(const fs::path& folder) {
  const fs::path file = folder / "leaving-notice.txt";
  const int64_t now = notice::NowMs();
  Expect(now > 1700000000000ll, "(a) NowMs is the time since 1970 in milliseconds");
  Expect(!notice::Remember(file, notice::Kind::none, now) && !fs::exists(file), "(a) nothing is written for none");
  Expect(notice::Remember(file, notice::Kind::menu, now - 2500), "(a) Remember writes");
  Expect(Read(file) == "kind=1\nat=" + std::to_string(now - 2500) + "\n", "(a) the file's text");
  const notice::Remembered back = notice::Take(file, now);
  Expect(back.kind == notice::Kind::menu && back.at_ms == now - 2500, "(a) Take gives back the kind and the time");
  Expect(!fs::exists(file), "(a) and removes the file");
  Expect(notice::Take(file, now).kind == notice::Kind::none, "(a) a missing file gives none");
  notice::Remember(file, notice::Kind::restart_game, now - 119000);
  Expect(notice::Take(file, now).kind == notice::Kind::restart_game, "(a) a restart under two minutes ago");
  notice::Remember(file, notice::Kind::menu, now - 121000);
  Expect(notice::Take(file, now).kind == notice::Kind::none && !fs::exists(file), "(a) over two minutes ago: none, removed");
  notice::Remember(file, notice::Kind::menu, now + 5000);
  Expect(notice::Take(file, now).kind == notice::Kind::none, "(a) a time in the future: none");
  WriteFile(file, "kind=9\nat=" + std::to_string(now) + "\n");
  Expect(notice::Take(file, now).kind == notice::Kind::none, "(a) an unknown kind: none");
  WriteFile(file, "garbage");
  Expect(notice::Take(file, now).kind == notice::Kind::none && !fs::exists(file), "(a) a broken file: none, removed");
  WriteFile(file, "kind=3\r\nat=" + std::to_string(now - 10) + "\r\n");
  Expect(notice::Take(file, now).kind == notice::Kind::closing, "(a) CR LF lines read");
}

void CheckTree() {
  const std::string main = Read("src/canary_game_main.cpp");
  const size_t entered = main.find("Stage(\"BOOT main entered\");");
  const size_t taken = main.find("xbox360ps5::leaving_notice::Take(");
  Expect(entered != std::string::npos && taken != std::string::npos && taken > entered &&
             taken < main.find("Stage(\"BOOT window\");"),
         "(b) the file is taken as main starts, before the window");
  Expect(Has(main, "BOOT timing: %lld ms from the restart's LoadExec to main"), "(b) the restart's length is logged");
  const size_t made = main.find("boot_notice_overlay = new LeavingNoticeOverlay(&drawer);");
  const size_t presenter = main.find("drawer.SetPresenterAndImmediateDrawer(emulator.graphics_system()->presenter(), immediate.get());");
  const size_t splash = main.find("sceSystemServiceHideSplashScreen();\r\n      xbox360ps5::motion::SetEnabled") != std::string::npos
                            ? main.find("sceSystemServiceHideSplashScreen();\r\n      xbox360ps5::motion::SetEnabled")
                            : main.find("sceSystemServiceHideSplashScreen();\n      xbox360ps5::motion::SetEnabled");
  Expect(made != std::string::npos && presenter < made && made < splash && splash != std::string::npos,
         "(b) the notice is made once there is a presenter, before the start-up picture goes");
  const std::string scan = Between(main, "Stage(\"BOOT launcher\");", "dialog = new xbox360ps5::LauncherDialog(&drawer, launcher);");
  Expect(Has(scan, "paint_boot_notice();") && Has(scan, "BootTiming(\"library scan\");") &&
             Has(scan, "BootTiming(\"library scanned\");"),
         "(b) painted before the library's scan, which is timed");
  Expect(Has(main, "context.Tick(); window.Paint();\r\n            end_boot_notice(\"first launcher frame\");") ||
             Has(main, "context.Tick(); window.Paint();\n            end_boot_notice(\"first launcher frame\");"),
         "(b) ended after the launcher's first frame");
  Expect(Has(main, "end_boot_notice(\"game launched\");"), "(b) or when a game's launch returns");
  const std::string end = Between(main, "const auto end_boot_notice = ", "};");
  Expect(Has(end, "leaving_notice::Show(xbox360ps5::leaving_notice::Kind::none);") && Has(end, "delete boot_notice_overlay;"),
         "(b) ending it hides and removes the overlay");
  const std::string restart = Between(main, "XELOGI(\"ENGINE RESTART\");", "sceSystemServiceLoadExec(\"/app0/eboot.bin\", nullptr);");
  Expect(Has(restart, "xbox360ps5::leaving_notice::Remember(storage / \"leaving-notice.txt\""),
         "(b) written before the return's LoadExec");
  const std::string hook = Between(main, "XamModule::restart_hook = [exit_cache, xam, storage] {", "sceSystemServiceLoadExec(");
  Expect(Has(hook, "xbox360ps5::leaving_notice::Remember(storage / \"leaving-notice.txt\""),
         "(b) and before the title switch's");
  for (const char* step : {"BootTiming(\"main entered\");", "BootTiming(\"window\");", "BootTiming(\"emulator setup\");",
                           "BootTiming(\"emulator setup returned\");",
                           "BootTiming(boot_notice_overlay ? \"presenter ready, notice shown\" : \"presenter ready\");"})
    Expect(Has(main, step), std::string("(b) timed: ") + step);
}
}  // namespace

int main() {
  const fs::path folder = fs::temp_directory_path() /
      ("check-boot-notice-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(folder);
  CheckFile(folder);
  CheckTree();
  std::error_code error;
  fs::remove_all(folder, error);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-boot-notice: all checks passed\n");
  return 0;
}
