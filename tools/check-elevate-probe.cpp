// SPDX-License-Identifier: MIT
// Fix 186d (tools/source-1.0.2/fixes/186-elevate-probe.patch): the opt-in, off-by-default probe that
// elevates to UID 0 before the 148i/148j write bench for issue #80. The console runs etaHEN, whose
// 9028 ptrace-hijack killed our process (186b/c); PATH B is used instead: an isolated LGPL-2.1
// ELV1/elfldr client (port/vendor/lapy, adapted from prospero-win + PS5-Lapy-JB-Daemon) streams a
// bundled one-shot helper ELF to the local elfldr (127.0.0.1:9021) and elevates cooperatively.
// This drives the MIT orchestration core (elevate_probe::Run) with a fake client (no sockets) and
// text-checks the assembled header, the LGPL module, the launcher and canary:
//  (a) Parse (on/off/empty/comment, attempts=/timeout_ms= clamped);
//  (b) the data-probe path (the app's own data folder) and backend/outcome names;
//  (c) Run: off -> no request; already root -> elevated (already-root), no request; the client
//      succeeding + euid 0 -> elevated (lapy-elfldr); the client failing -> not elevated, app carries
//      on; the per-process cap; the mirrored data_mount fields;
//  (d) the tree: the MIT header calls the LGPL client (lapy_elevation_request) through the vendor
//      header, uses AttemptEarly + a std::ofstream progress file and std file I/O (fix 186f: raw
//      ::umask/::open were null imports on the console firmware), has NO etaHEN/9028/hijack code;
//      the LGPL module is SPDX LGPL-2.1-or-later and speaks ELV1 over 9021 with seteuid(geteuid());
//      canary_game_main.cpp calls AttemptEarly before InstallCrashReport; launcher.cpp makes no
//      request; CMakeLists builds the client.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#define XBOX360PS5_ELEVATE_PROBE_NO_SYSTEM 1  // the host check needs only the portable MIT core
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
size_t After(const std::string& s, const std::string& w, size_t from) {
  return from == std::string::npos ? std::string::npos : s.find(w, from);
}

struct FakeOps {
  std::string switch_text;
  unsigned uid = 1, euid = 1;
  bool client_ok = false;  // the ELV1 client elevates (euid -> 0)
  int client_err = 0;
  bool data_ok = true;
  std::vector<std::string> order;
  std::string read_text(const std::string&) { order.push_back("read_text"); return switch_text; }
  unsigned getuid() { return uid; }
  unsigned geteuid() { return euid; }
  bool data_write(const std::string&) { order.push_back("data_write"); return data_ok; }
  ep::ElevateResult elevate_request(long) {
    order.push_back("elevate_request");
    ep::ElevateResult r;
    r.attempted = true;
    r.ok = client_ok;
    r.err = client_err;
    if (client_ok) euid = 0;
    return r;
  }
};
FakeOps OnOps() { FakeOps o; o.switch_text = "on\n"; return o; }
bool Has(const std::vector<std::string>& v, const std::string& s) {
  for (auto& x : v) if (x == s) return true;
  return false;
}

void Parsing() {
  Expect(ep::Parse("on\n").on && ep::Parse("  on  ").on, "(a) on turns it on");
  Expect(!ep::Parse("").on && !ep::Parse("off").on && !ep::Parse("# on").on && !ep::Parse("ona").on,
         "(a) empty/off/comment/other: off");
  const ep::Config s = ep::Parse("on\nattempts=2\ntimeout_ms=1500\n");
  Expect(s.max_attempts == 2 && s.timeout_ms == 1500, "(a) attempts/timeout_ms read");
  const ep::Config c = ep::Parse("on\nattempts=999\ntimeout_ms=999999\n");
  Expect(c.max_attempts == ep::kMaxAttemptsDefault && c.timeout_ms == ep::kTimeoutMsDefault,
         "(a) out-of-range clamped to default");
}
void Paths() {
  Expect(ep::SwitchFile("/data/homebrew/PPSA83600") == "/data/homebrew/PPSA83600/elevate-probe.txt",
         "(b) switch file path");
  Expect(ep::DataProbeFile("/data/homebrew/PPSA83600", 7).rfind("/data/homebrew/PPSA83600/", 0) == 0,
         "(b) data probe in the app's own data folder");
  Expect(std::string(ep::kHelperPath) == "/app0/lapy.elf", "(b) bundled helper path");
}
void Running() {
  ep::AttemptCounter().store(0);
  { FakeOps o; o.switch_text = "off";
    const ep::Result r = ep::Run(o, "/data/homebrew/PPSA83600", 7);
    Expect(r.outcome == ep::Outcome::kOff && o.order.size() == 1, "(c) off: switch read only, no request"); }
  ep::AttemptCounter().store(0);
  { FakeOps o = OnOps(); o.euid = 0;
    const ep::Result r = ep::Run(o, "/data/homebrew/PPSA83600", 42);
    Expect(r.outcome == ep::Outcome::kElevated && r.backend == ep::Backend::kAlready &&
               r.elevated_before && !Has(o.order, "elevate_request"),
           "(c) already root -> elevated (already-root), no request"); }
  ep::AttemptCounter().store(0);
  { FakeOps o = OnOps(); o.client_ok = true;
    const ep::Result r = ep::Run(o, "/data/homebrew/PPSA83600", 42);
    Expect(r.outcome == ep::Outcome::kElevated && r.backend == ep::Backend::kLapyElfldr &&
               r.helper_completed && r.elevated_after && !r.elevated_before && Has(o.order, "elevate_request"),
           "(c) client ok + euid 0 -> elevated via lapy-elfldr"); }
  ep::AttemptCounter().store(0);
  { FakeOps o = OnOps(); o.client_ok = false; o.client_err = 13;
    const ep::Result r = ep::Run(o, "/data/homebrew/PPSA83600", 9);
    Expect(r.outcome == ep::Outcome::kNotElevated && r.backend == ep::Backend::kNone &&
               !r.helper_completed && r.helper_errno == 13,
           "(c) client fails -> not elevated, app carries on"); }
  { ep::AttemptCounter().store(0); int tries = 0, capped = 0;
    for (int i = 0; i < 5; ++i) {
      FakeOps o = OnOps(); o.switch_text = "on\nattempts=2\n"; o.client_ok = false;
      const ep::Result r = ep::Run(o, "/data/homebrew/PPSA83600", 100 + i);
      if (r.outcome == ep::Outcome::kCapped) { ++capped; Expect(!Has(o.order, "elevate_request"), "(c) capped: no request"); }
      else ++tries;
    }
    Expect(tries == 2 && capped == 3, "(c) only max_attempts requests per process"); }
  ep::AttemptCounter().store(0);
}
void Tree() {
  const std::string h = Read("include/xbox360ps5/elevate_probe.hpp");
  Expect(!h.empty(), "(d) header present");
  Expect(h.find("vendor/lapy/lapy_elevation_client.h") != std::string::npos &&
             h.find("lapy_elevation_request(") != std::string::npos,
         "(d) the MIT header calls the LGPL client through the vendor interface");
  Expect(h.find("inline Result AttemptEarly(") != std::string::npos &&
             h.find("std::ofstream prog(") != std::string::npos &&
             h.find("elevate-probe-progress.txt") != std::string::npos,
         "(d) AttemptEarly writes a std::ofstream progress file");
  // 186f: the probe uses ONLY std:: file I/O. 186e's raw ::umask/::open were unresolved (null)
  // imports on the console firmware, so the call jumped to address 0 (SIGSEGV, RIP=0) before any
  // step could log. These assertions keep the crash-prone raw-POSIX forms out for good.
  Expect(h.find("std::ofstream f(path") != std::string::npos &&
             h.find("std::filesystem::remove(path") != std::string::npos,
         "(d) data probe writes via std::ofstream + std::filesystem::remove");
  Expect(h.find("::umask(") == std::string::npos && h.find("O_CREAT") == std::string::npos &&
             h.find("::fsync(") == std::string::npos,
         "(d) no raw ::umask/::open(O_CREAT)/::fsync (null imports on PS5; fix 186f)");
  Expect(h.find("AttemptCounter().fetch_add(1)") != std::string::npos && h.find("Outcome::kCapped") != std::string::npos,
         "(d) per-process attempt cap");
  Expect(h.find("HijackerCommand") == std::string::npos && h.find("0xDEADBEEF") == std::string::npos &&
             h.find("sigaction") == std::string::npos && h.find("EtahenJailbreakLogged") == std::string::npos,
         "(d) the dead etaHEN 9028/hijack/SIGSYS code is gone (only a comment may mention it)");
  for (const char* f : {"data_before", "elevated_before", "helper_completed", "helper_errno",
                        "data_after", "elevated_after", "waited_ms", "settled_ms"})
    Expect(h.find(std::string(f) + "=") != std::string::npos,
           std::string("(d) data_mount field ") + f);

  const std::string c = Read("port/vendor/lapy/lapy_elevation_client.c");
  const std::string ch = Read("port/vendor/lapy/lapy_elevation_client.h");
  const std::string proto = Read("port/vendor/lapy/lapy_elevation_protocol.h");
  Expect(c.find("SPDX-License-Identifier: LGPL-2.1-or-later") == 0 ||
             c.find("SPDX-License-Identifier: LGPL-2.1-or-later") != std::string::npos,
         "(d) the client module is LGPL-2.1-or-later");
  Expect(proto.find("SPDX-License-Identifier: LGPL-2.1-or-later") != std::string::npos,
         "(d) the ELV1 protocol header is LGPL-2.1-or-later");
  Expect(c.find("9021") != std::string::npos && c.find("connect(") != std::string::npos &&
             c.find("inet_pton") != std::string::npos && c.find("127.0.0.1") != std::string::npos,
         "(d) the client connects to the local elfldr at 127.0.0.1:9021");
  Expect(c.find("LAPY_ELEVATION_REQUEST") != std::string::npos &&
             c.find("LAPY_ELEVATION_PREPARE") != std::string::npos &&
             c.find("LAPY_ELEVATION_PREPARED") != std::string::npos &&
             c.find("LAPY_ELEVATION_RESPONSE") != std::string::npos,
         "(d) the client runs the full ELV1 handshake");
  Expect(c.find("seteuid(geteuid())") != std::string::npos,
         "(d) the client clones the credential cooperatively (seteuid)");
  Expect(c.find("prospero-win") != std::string::npos && c.find("404cac6") != std::string::npos,
         "(d) the client credits prospero-win at the pinned rev");

  const std::string cmake = Read("canary/CMakeLists.txt");
  Expect(cmake.find("port/vendor/lapy/lapy_elevation_client.c") != std::string::npos,
         "(d) the build compiles the LGPL client");
  const std::string canary = Read("src/canary_game_main.cpp");
  const size_t cmain = canary.find("int main(");
  const size_t aCrash = After(canary, "InstallCrashReport(", cmain);
  const size_t aAttempt = After(canary, "elevate_probe::AttemptEarly(", cmain);
  const size_t aNet = After(canary, "StartNetLog(", cmain);
  const size_t aInit = After(canary, "InitializeLogging(", cmain);
  const size_t aWeb = After(canary, "StartWebSettings(", cmain);
  // 186g: elevate at the earliest SINGLE-THREADED point -- after InstallCrashReport (boot.log up and
  // /data writable), but BEFORE StartNetLog, InitializeLogging and the web server, which start the
  // first worker threads. The Lapy helper refuses a multi-threaded target (APPLY_FAILED), which is
  // why 186e/186f (after InitializeLogging) failed with euid still 1.
  Expect(cmain != std::string::npos && aCrash != std::string::npos && aAttempt != std::string::npos &&
             aNet != std::string::npos && aInit != std::string::npos && aWeb != std::string::npos &&
             aCrash < aAttempt && aAttempt < aNet && aAttempt < aInit && aAttempt < aWeb,
         "(d) AttemptEarly runs single-threaded: after InstallCrashReport, before StartNetLog/logger/web");
  Expect(canary.find("AttemptEarly(XBOX360PS5_DATA_ROOT, &Xbox360PS5ElevateProbeLog)") != std::string::npos &&
             canary.find("void Xbox360PS5ElevateProbeLog(") != std::string::npos,
         "(d) the probe is given the boot.log step sink (Xbox360PS5ElevateProbeLog)");
  Expect(h.find("void (*&LogCb())") != std::string::npos && h.find("if (LogCb()) LogCb()(text)") != std::string::npos,
         "(d) ProgLine emits each step to the fallback sink too (survives a /data open failure)");
  const std::string launcher = Read("src/launcher.cpp");
  Expect(launcher.find("elevate_probe::Attempt") == std::string::npos,
         "(d) launcher.cpp makes no elevation request");
}
}  // namespace

int main() {
  Parsing();
  Paths();
  Running();
  if (fs::exists("include/xbox360ps5/elevate_probe.hpp")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
  std::printf("elevate probe: all checks passed\n");
  return 0;
}
