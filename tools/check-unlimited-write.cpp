// SPDX-License-Identifier: MIT
// Fix 186 (tools/source-1.0.2/fixes/186-elevate-probe.patch): the player-facing "Unlimited write
// speed (root)" option (issue #80) and the gate that reads it. The option is global, off by default,
// and raises the app to UID 0 at launch (through the isolated LGPL ELV1/elfldr client) to remove the
// PS5's per-app write-speed limit. check-elevate-probe.cpp covers the elevation core and the LGPL
// module; this check covers the new toggle's wiring and its gate:
//  (a) SettingsWantElevation: the "unlimited_write=<n>" line enables it only when non-zero; other
//      keys, comments, an empty file and a plain "unlimited_write" with no value do not;
//  (b) the launcher: Settings has the unlimited_write field, and Options() exposes it as a System
//      (category 4), global-only (per_game false), off-by-default (recommended 0) Off/On switch
//      labelled "Unlimited write speed (root)" (translated via the language files, like every option);
//  (c) the gate: AttemptEarly enables the elevation from EITHER the setting (settings.txt, read from
//      the data folder and the pre-fix-165 internal-storage fallback) OR the developer switch file,
//      and is a silent no-op otherwise.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#define XBOX360PS5_ELEVATE_PROBE_NO_SYSTEM 1  // the host check needs only the portable MIT core
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/elevate_probe.hpp"

namespace {
namespace fs = std::filesystem;
namespace ep = xbox360ps5::elevate_probe;

int failures = 0;
void Expect(bool c, const std::string& what) {
  if (!c) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}
std::string Read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream t;
  t << f.rdbuf();
  return t.str();
}
bool Has(const std::string& s, const std::string& w) { return s.find(w) != std::string::npos; }

void Gate() {
  // (a) only a non-zero unlimited_write enables it.
  Expect(ep::SettingsWantElevation("unlimited_write=1"), "(a) unlimited_write=1 enables");
  Expect(ep::SettingsWantElevation("unlimited_write=2"), "(a) any non-zero enables");
  Expect(ep::SettingsWantElevation("unlimited_write = 1"), "(a) spaces around = allowed");
  Expect(!ep::SettingsWantElevation("unlimited_write=0"), "(a) =0 stays off");
  Expect(!ep::SettingsWantElevation(""), "(a) empty file: off");
  Expect(!ep::SettingsWantElevation("unlimited_write"), "(a) no value: off");
  Expect(!ep::SettingsWantElevation("#unlimited_write=1"), "(a) a comment line does not enable");
  Expect(!ep::SettingsWantElevation("other_unlimited_write=1"), "(a) only the exact key matches");
  Expect(!ep::SettingsWantElevation("language=1\nmute=0\nvsync=1\n"), "(a) absent key: off");
  // A realistic settings.txt (Settings::Save writes one key=value per line).
  Expect(ep::SettingsWantElevation("language=0\ninterface_language=0\nunlimited_write=1\nweb_page=1\n"),
         "(a) found among other options");
  Expect(!ep::SettingsWantElevation("language=0\nunlimited_write=0\nweb_page=1\n"),
         "(a) off among other options");
  // The file it reads sits next to the developer switch file in the data folder.
  Expect(ep::SettingsFile("/data/homebrew/PPSA83600") == "/data/homebrew/PPSA83600/settings.txt",
         "(a) settings file path");
  Expect(std::string(ep::kInternalStorage) == "/download0/xbox360ps5",
         "(a) internal-storage fallback path (pre-fix-165)");
}

void Launcher() {
  const std::string hpp = Read("include/xbox360ps5/launcher.hpp");
  const std::string cpp = Read("src/launcher.cpp");
  Expect(Has(hpp, "int unlimited_write = 0;"), "(b) Settings has the off-by-default field");
  // The Options() row: System (4), global (false), recommended 0 (off), Off/On.
  const size_t row = cpp.find("{\"unlimited_write\", &S::unlimited_write,");
  Expect(row != std::string::npos, "(b) Options() exposes unlimited_write");
  if (row != std::string::npos) {
    const std::string line = cpp.substr(row, cpp.find('\n', row) - row);
    Expect(Has(line, ", 4, W::start, 0, false,"),
           "(b) System category, start-time, recommended off, global-only");
    Expect(Has(line, "\"Unlimited write speed (root)\""), "(b) the English label");
    const size_t about = cpp.find("off_on},", row);
    Expect(about != std::string::npos && about - row < 1200, "(b) uses the Off/On choices");
  }
}

void GateWiring() {
  const std::string h = Read("include/xbox360ps5/elevate_probe.hpp");
  // (c) AttemptEarly enables from the setting OR the developer switch file, else a silent no-op.
  Expect(Has(h, "SettingsWantElevation(slurp(SettingsFile(data_root)))"),
         "(c) the gate reads the setting from the data folder");
  Expect(Has(h, "kInternalStorage") && Has(h, "/settings.txt"),
         "(c) the gate also reads the internal-storage fallback");
  Expect(Has(h, "bool want = r.config.on;"),
         "(c) the developer switch file still enables it");
  Expect(Has(h, "if (!want) { r.outcome = Outcome::kOff; return r; }"),
         "(c) off by default: a no-op without the setting or the switch");
  const std::string canary = Read("src/canary_game_main.cpp");
  Expect(Has(canary, "elevate_probe::AttemptEarly(XBOX360PS5_DATA_ROOT,"),
         "(c) main() runs the gate at the early single-threaded point");
}
}  // namespace

int main() {
  Gate();
  if (fs::exists("include/xbox360ps5/launcher.hpp") && fs::exists("src/launcher.cpp")) {
    Launcher();
    GateWiring();
  } else {
    std::printf("note: no assembled tree here, tree checks skipped\n");
  }
  if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
  std::printf("unlimited write: all checks passed\n");
  return 0;
}
