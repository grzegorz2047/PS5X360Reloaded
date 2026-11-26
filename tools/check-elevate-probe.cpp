// SPDX-License-Identifier: MIT
// Fix 186 / 186b (tools/source-1.0.2/fixes/186-elevate-probe.patch): the opt-in, off-by-default
// elevation probe that measures the 148i/148j write bench as UID 0 for issue #80. Primary backend:
// etaHEN's documented non-whitelist jailbreak over 127.0.0.1:9028 (reimplemented clean-room; etaHEN
// is GPL-3.0, no code copied). Secondary: a resident Lapy daemon's /download0/elevate_proc marker.
// This drives the portable core (elevate_probe::Run) with a fake console (no sockets, no syscalls)
// and text-checks the assembled header and launcher:
//  (a) Parse: "on" on/off/empty/comment, attempts=/timeout_ms= read and clamped;
//  (b) the Lapy marker path/body and the data-probe path (in the app's own data folder);
//  (c) Run: off -> no request; etaHEN accepts + euid 0 after -> elevated (backend etahen); already
//      root at start -> elevated (backend already-root), no request; no backend -> not elevated, app
//      carries on; etaHEN refuses but a resident Lapy marker + euid 0 -> elevated (backend lapy); the
//      per-process cap; the mirrored data_mount fields;
//  (d) the tree: the header speaks etaHEN's 9028 HijackerCommand wire protocol (magic 0xDEADBEEF,
//      JAILBREAK cmd 5, connect 127.0.0.1:9028, 2576-byte struct), writes the Lapy marker as a
//      secondary, uses umask(0)+0666, a SIGSYS handler and the attempt cap, logs the data_mount
//      field names, and credits etaHEN's protocol with no GPL copied; launcher.cpp calls Attempt
//      before write_bench::Launch on the same thread.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#define XBOX360PS5_ELEVATE_PROBE_NO_SYSTEM 1  // the host check needs only the portable core
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

// A fake console. euid starts at `euid` and jumps to 0 when a configured backend accepts. Every call
// is recorded in `order`.
struct FakeOps {
  std::string switch_text;
  unsigned uid = 1, euid = 1;
  bool etahen_ok = false;   // etaHEN accepts the 9028 jailbreak (and euid -> 0)
  bool etahen_connect = true;  // the 9028 server answers at all
  int etahen_ret = 0;
  bool lapy_ok = false;     // a resident Lapy daemon consumes the marker (and euid -> 0)
  bool data_ok = true;      // the app can write its own data folder
  int saved_errno = 0;
  long long clock = 1000;
  std::vector<std::string> order;

  int last_errno() const { return saved_errno; }
  std::string read_text(const std::string& path) {
    order.push_back("read_text");
    return switch_text;
  }
  unsigned getuid() { return uid; }
  unsigned geteuid() { return euid; }
  int seteuid(unsigned want) {
    order.push_back("seteuid");
    return 0;
  }
  bool data_write(const std::string& path) {
    order.push_back("data_write:" + path);
    return data_ok;
  }
  ep::EtahenResult etahen_jailbreak(long pid) {
    order.push_back("etahen_jailbreak:" + std::to_string(pid));
    ep::EtahenResult r;
    if (!etahen_connect) {
      r.err = 111;
      return r;
    }
    r.attempted = true;
    r.ret = etahen_ret;
    r.ok = etahen_ok;
    if (etahen_ok) euid = 0;
    return r;
  }
  bool write_lapy_marker(const std::string& temp, const std::string& marker, const std::string& body) {
    order.push_back("lapy:" + temp + "->" + marker + ":" + body);
    if (lapy_ok) euid = 0;
    return true;
  }
  bool wait_for_root(int timeout_ms, int* waited_ms) {
    order.push_back("wait");
    *waited_ms = euid == 0 ? 10 : timeout_ms;
    return euid == 0;
  }
  long long now_ms() { return clock += 5; }
};

FakeOps OnOps() {
  FakeOps ops;
  ops.switch_text = "on\n";
  return ops;
}
bool Has(const std::vector<std::string>& v, const std::string& prefix) {
  for (const auto& s : v)
    if (s.rfind(prefix, 0) == 0) return true;
  return false;
}

void Parsing() {
  Expect(ep::Parse("on\n").on && ep::Parse("  on  ").on, "(a) \"on\" turns it on");
  Expect(!ep::Parse("").on && !ep::Parse("off\n").on && !ep::Parse("# on\n").on && !ep::Parse("ona").on,
         "(a) empty, off, comment or another word: off");
  const ep::Config set = ep::Parse("on\nattempts=2\ntimeout_ms=1500\n");
  Expect(set.max_attempts == 2 && set.timeout_ms == 1500, "(a) attempts and timeout_ms are read");
  const ep::Config clamp = ep::Parse("on\nattempts=999\ntimeout_ms=999999\n");
  Expect(clamp.max_attempts == ep::kMaxAttemptsDefault && clamp.timeout_ms == ep::kTimeoutMsDefault,
         "(a) out-of-range values fall back to the default");
}

void Paths() {
  Expect(ep::SwitchFile("/data/homebrew/PPSA83600") == "/data/homebrew/PPSA83600/elevate-probe.txt",
         "(b) switch file under the data root");
  Expect(std::string(ep::kLapyMarker) == "/download0/elevate_proc", "(b) the Lapy marker path");
  Expect(ep::LapyTemp(7) == "/download0/.elevate_proc.7", "(b) the Lapy temp file");
  Expect(ep::LapyBody(7) == "{\"PID\":7}\n", "(b) the Lapy JSON body");
  Expect(ep::DataProbeFile("/data/homebrew/PPSA83600", 7).rfind("/data/homebrew/PPSA83600/", 0) == 0,
         "(b) the data probe is in the app's own data folder");
  Expect(ep::kEtahenPort == 9028 && ep::kJailbreakCmd == 5 && ep::kHijackerMagic == 0xDEADBEEFu,
         "(b) etaHEN protocol constants");
}

void Running() {
  ep::AttemptCounter().store(0);
  {  // off
    FakeOps ops;
    ops.switch_text = "off";
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 7);
    Expect(r.outcome == ep::Outcome::kOff && ops.order.size() == 1, "(c) off: switch read, nothing else");
  }
  ep::AttemptCounter().store(0);
  {  // etaHEN accepts
    FakeOps ops = OnOps();
    ops.etahen_ok = true;
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 42);
    Expect(r.outcome == ep::Outcome::kElevated && r.backend == ep::Backend::kEtahen,
           "(c) etaHEN accepts + euid 0 -> elevated via etahen-9028");
    Expect(r.etahen_attempted && r.helper_completed && r.elevated_after && !r.elevated_before,
           "(c) the elevated result records the etaHEN request and euid 0 after");
    Expect(Has(ops.order, "etahen_jailbreak:42") && !Has(ops.order, "lapy:"),
           "(c) etaHEN first; no Lapy marker once root");
    Expect(r.attempts == 1, "(c) first request is attempt 1");
  }
  ep::AttemptCounter().store(0);
  {  // already root at start (etaHEN auto-jailbreak)
    FakeOps ops = OnOps();
    ops.euid = 0;
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 42);
    Expect(r.outcome == ep::Outcome::kElevated && r.backend == ep::Backend::kAlready &&
               r.elevated_before && !Has(ops.order, "etahen_jailbreak:"),
           "(c) already root -> elevated (already-root), no request made");
  }
  ep::AttemptCounter().store(0);
  {  // no backend: etaHEN unreachable, no Lapy daemon
    FakeOps ops = OnOps();
    ops.etahen_connect = false;
    ops.lapy_ok = false;
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 9);
    Expect(r.outcome == ep::Outcome::kNotElevated && r.backend == ep::Backend::kNone &&
               !r.helper_completed && r.lapy_requested,
           "(c) no backend: not elevated, the Lapy marker still tried, app carries on");
  }
  ep::AttemptCounter().store(0);
  {  // etaHEN refuses but a resident Lapy daemon elevates
    FakeOps ops = OnOps();
    ops.etahen_connect = true;
    ops.etahen_ok = false;
    ops.lapy_ok = true;
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 9);
    Expect(r.outcome == ep::Outcome::kElevated && r.backend == ep::Backend::kLapy && r.lapy_requested,
           "(c) etaHEN refuses, Lapy marker elevates -> backend lapy-marker");
  }
  {  // per-process cap
    ep::AttemptCounter().store(0);
    int tries = 0, capped = 0;
    for (int i = 0; i < 5; ++i) {
      FakeOps ops = OnOps();
      ops.switch_text = "on\nattempts=2\n";
      ops.etahen_connect = false;
      const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 100 + i);
      if (r.outcome == ep::Outcome::kCapped) {
        ++capped;
        Expect(!Has(ops.order, "etahen_jailbreak:"), "(c) a capped run sends no request");
      } else {
        ++tries;
      }
    }
    Expect(tries == 2 && capped == 3, "(c) only max_attempts requests per process; the rest capped");
  }
  ep::AttemptCounter().store(0);
}

size_t After(const std::string& s, const std::string& what, size_t from) {
  return from == std::string::npos ? std::string::npos : s.find(what, from);
}

void Tree() {
  const std::string h = Read("include/xbox360ps5/elevate_probe.hpp");
  Expect(!h.empty(), "(d) the header is in the tree");
  // etaHEN 9028 wire protocol
  Expect(h.find("9028") != std::string::npos && h.find("0xDEADBEEF") != std::string::npos &&
             h.find("struct HijackerCommand") != std::string::npos,
         "(d) the header speaks etaHEN's 9028 HijackerCommand protocol");
  Expect(h.find("kJailbreakCmd = 5") != std::string::npos &&
             h.find("sizeof(HijackerCommand) == 16 + 2 * kHijackerMsg") != std::string::npos,
         "(d) JAILBREAK cmd 5 and the 2576-byte wire size");
  Expect(h.find("::connect(") != std::string::npos && h.find("inet_pton") != std::string::npos &&
             h.find("cmd.ret == 0 || cmd.ret == kRetSentinel") != std::string::npos,
         "(d) it connects to the command server and reads the jailbreak verdict");
  Expect(h.find("no etaHEN") != std::string::npos && h.find("GPL-3.0") != std::string::npos &&
             h.find("no etaHEN code is copied") != std::string::npos,
         "(d) it credits etaHEN's protocol and states no GPL code is copied");
  // Lapy secondary + safeguards
  Expect(h.find("write_lapy_marker") != std::string::npos && h.find("/download0/elevate_proc") != std::string::npos,
         "(d) the Lapy marker is a secondary backend");
  Expect(h.find("umask(0)") != std::string::npos && h.find("O_CREAT | O_TRUNC, 0666") != std::string::npos,
         "(d) the data probe uses umask(0) and 0666");
  Expect(h.find("sigaction(SIGSYS") != std::string::npos && h.find("siglongjmp(") != std::string::npos,
         "(d) a SIGSYS handler guards the request");
  Expect(h.find("AttemptCounter().fetch_add(1)") != std::string::npos &&
             h.find("Outcome::kCapped") != std::string::npos,
         "(d) a per-process attempt cap");
  // data_mount field names (comparable to TrinityXSX)
  for (const char* f : {"data_before", "elevated_before", "helper_completed", "helper_errno",
                        "data_after", "elevated_after", "waited_ms", "settled_ms"}) {
    Expect(h.find(std::string(f) + "=") != std::string::npos,
           std::string("(d) the log mirrors the data_mount field ") + f);
  }

  const std::string launcher = Read("src/launcher.cpp");
  Expect(launcher.find("xbox360ps5/elevate_probe.hpp") != std::string::npos,
         "(d) launcher.cpp includes the probe header");
  const size_t attempt = launcher.find("elevate_probe::Attempt(");
  const size_t bench = After(launcher, "write_bench::Launch(", attempt);
  Expect(attempt != std::string::npos && bench != std::string::npos && attempt < bench,
         "(d) Attempt runs before write_bench::Launch, on the same thread");
}

}  // namespace

int main() {
  Parsing();
  Paths();
  Running();
  if (fs::exists("include/xbox360ps5/elevate_probe.hpp")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("elevate probe: all checks passed\n");
  return 0;
}
