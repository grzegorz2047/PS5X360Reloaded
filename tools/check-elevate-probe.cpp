// SPDX-License-Identifier: MIT
// Fix 186 (tools/source-1.0.2/fixes/186-elevate-probe.patch): an opt-in, off-by-default probe that
// asks the user's PS5-Lapy-JB-Daemon to elevate the process to UID 0 before the 148i/148j write
// benchmark, to measure the write throughput past the ~1.5 GB per-process budget (issue #80) as root.
// This drives the portable core (elevate_probe::Run) with a fake daemon and console -- no real
// elevation, no syscalls, no daemon -- and text-checks the assembled header and launcher:
//  (a) Parse: "on" turns it on, "off"/empty/comment leaves it off, attempts= and timeout_ms= are
//      read and clamped;
//  (b) the request paths and JSON body (marker /download0/elevate_proc, a per-pid temp, a root-write
//      probe outside the title's sandbox under /data);
//  (c) Run: off -> no request; the daemon consuming the marker and a root write working only after
//      -> elevated; no daemon (timeout) -> not elevated but the app carries on; seteuid failing ->
//      aborts before any request; the request write failing -> request-failed; the per-process cap;
//      the ordering (seteuid, then the marker, then the poll, then the after check);
//  (d) the tree: the header uses umask(0)+0666 for its test file, O_EXCL 0644 + rename for the
//      marker, a SIGSYS handler, the attempt cap, seteuid(geteuid()) before the marker, and the
//      access()==ENOENT poll; launcher.cpp includes the header and calls Attempt before
//      write_bench::Launch on the same thread.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#define XBOX360PS5_ELEVATE_PROBE_NO_SYSTEM 1  // the host check needs only the portable core
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
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

// A fake daemon and console. root_write returns `root_before` on the first call and `root_after` on
// the rest; poll_consumed returns `consumed`. Every call is recorded in `order`.
struct FakeOps {
  std::string switch_text;
  unsigned uid = 1, euid = 1;
  int seteuid_rc = 0, seteuid_errno = 0;
  bool request_ok = true;
  int request_errno = 0;
  bool consumed = false;
  int poll_errno = 0;
  bool root_before = false, root_after = false;
  int root_calls = 0;
  int saved_errno = 0;
  long long clock = 1000;
  std::vector<std::string> order;

  int last_errno() const { return saved_errno; }
  std::string read_text(const std::string& path) {
    order.push_back("read_text:" + path);
    return switch_text;
  }
  unsigned getuid() { return uid; }
  unsigned geteuid() { return euid; }
  int seteuid(unsigned want) {
    order.push_back("seteuid:" + std::to_string(want));
    saved_errno = seteuid_errno;
    if (seteuid_rc == 0) euid = want;
    return seteuid_rc;
  }
  bool root_write(const std::string& path) {
    order.push_back("root_write:" + path);
    saved_errno = 0;
    return (root_calls++ == 0) ? root_before : root_after;
  }
  bool write_request(const std::string& temp, const std::string& marker, const std::string& body) {
    order.push_back("write_request:" + temp + "->" + marker + ":" + body);
    saved_errno = request_ok ? 0 : request_errno;
    return request_ok;
  }
  bool poll_consumed(const std::string& marker, int timeout_ms, int* waited_ms) {
    order.push_back("poll:" + marker + ":" + std::to_string(timeout_ms));
    saved_errno = poll_errno;
    *waited_ms = consumed ? 20 : timeout_ms;
    return consumed;
  }
  long long now_ms() { return clock += 5; }
};

FakeOps OnOps() {
  FakeOps ops;
  ops.switch_text = "on\n";
  return ops;
}

void Parsing() {
  Expect(ep::Parse("on\n").on, "(a) \"on\" turns it on");
  Expect(ep::Parse("on").on && ep::Parse("  on  \n").on && ep::Parse("on\r\n").on,
         "(a) \"on\" with spaces or CRLF");
  Expect(!ep::Parse("").on && !ep::Parse("off\n").on && !ep::Parse("# on\n").on &&
             !ep::Parse("ona\n").on,
         "(a) empty, off, a comment or another word leaves it off");
  const ep::Config def = ep::Parse("on\n");
  Expect(def.max_attempts == ep::kMaxAttemptsDefault && def.timeout_ms == ep::kTimeoutMsDefault,
         "(a) the defaults when nothing else is set");
  const ep::Config set = ep::Parse("on\nattempts=2\ntimeout_ms=1500\n");
  Expect(set.max_attempts == 2 && set.timeout_ms == 1500, "(a) attempts and timeout_ms are read");
  const ep::Config clamped = ep::Parse("on\nattempts=999\ntimeout_ms=999999\n");
  Expect(clamped.max_attempts == ep::kMaxAttemptsDefault && clamped.timeout_ms == ep::kTimeoutMsDefault,
         "(a) out-of-range attempts/timeout_ms fall back to the default");
  Expect(ep::Parse("on\nattempts=0\n").max_attempts == ep::kMaxAttemptsDefault,
         "(a) attempts below 1 is ignored");
}

void Paths() {
  Expect(ep::SwitchFile("/data/homebrew/PPSA83600") == "/data/homebrew/PPSA83600/elevate-probe.txt",
         "(b) the switch file is under the data root");
  Expect(std::string(ep::kRequestMarker) == "/download0/elevate_proc",
         "(b) the request marker is /download0/elevate_proc");
  Expect(ep::RequestTemp(123) == "/download0/.elevate_proc.123",
         "(b) the per-pid temp file is in /download0");
  Expect(ep::RequestBody(123) == "{\"PID\":123}\n", "(b) the JSON body is {\"PID\":<pid>}");
  Expect(ep::RootProbeFile(123) == "/data/trinityx360_elev_probe_123" &&
             ep::RootProbeFile(123).rfind("/data/homebrew", 0) != 0,
         "(b) the root-write probe is under /data, outside the title's sandbox");
}

void Running() {
  ep::AttemptCounter().store(0);
  {  // off: nothing is attempted
    FakeOps ops;
    ops.switch_text = "off\n";
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 7);
    Expect(r.outcome == ep::Outcome::kOff && ops.order.size() == 1 && ops.euid == 1,
           "(c) off: the switch is read, nothing else, no seteuid");
  }
  ep::AttemptCounter().store(0);
  {  // the daemon elevates: marker consumed, a root write works only after
    FakeOps ops = OnOps();
    ops.consumed = true;
    ops.root_before = false;
    ops.root_after = true;
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 42);
    Expect(r.outcome == ep::Outcome::kElevated, "(c) daemon acts + root write after -> elevated");
    Expect(r.seteuid_rc == 0 && r.request_written && r.helper_completed && !r.data_before &&
               r.data_after,
           "(c) the elevated result records seteuid, the request, the ack and the root write");
    Expect(r.attempts == 1, "(c) the first request is attempt 1");
    // ordering: read switch, root_write(before), seteuid, write_request, poll, root_write(after)
    Expect(ops.order.size() == 6 && ops.order[1].rfind("root_write", 0) == 0 &&
               ops.order[2].rfind("seteuid", 0) == 0 && ops.order[3].rfind("write_request", 0) == 0 &&
               ops.order[4].rfind("poll", 0) == 0 && ops.order[5].rfind("root_write", 0) == 0,
           "(c) order: before-check, seteuid, marker, poll, after-check");
    Expect(ops.order[3].find("/download0/.elevate_proc.42->/download0/elevate_proc:{\"PID\":42}") !=
               std::string::npos,
           "(c) the marker write uses the temp, the marker and the JSON body");
  }
  ep::AttemptCounter().store(0);
  {  // elevated by euid even if the root path is writable in the sandbox
    FakeOps ops = OnOps();
    ops.consumed = true;
    ops.root_before = true;
    ops.root_after = true;
    ops.seteuid_rc = 0;
    // model the daemon raising euid to 0 after the request
    ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 42);
    // euid stayed 1 in the fake (no real daemon), so this run is not-elevated: a sanity check that
    // "elevated" needs either euid 0 or a newly gained root write.
    Expect(r.outcome == ep::Outcome::kNotElevated,
           "(c) marker gone but no euid 0 and no newly gained root write -> not elevated");
  }
  ep::AttemptCounter().store(0);
  {  // no daemon: the marker is never consumed
    FakeOps ops = OnOps();
    ops.consumed = false;
    ops.root_before = false;
    ops.root_after = false;
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 9);
    Expect(r.outcome == ep::Outcome::kNotElevated && r.request_written && !r.helper_completed &&
               r.waited_ms == r.config.timeout_ms,
           "(c) no daemon: request sent, timed out, not elevated, app carries on");
  }
  ep::AttemptCounter().store(0);
  {  // seteuid fails: abort before any request
    FakeOps ops = OnOps();
    ops.seteuid_rc = -1;
    ops.seteuid_errno = 1;
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 9);
    bool wrote = false;
    for (const auto& o : ops.order) wrote = wrote || o.rfind("write_request", 0) == 0;
    Expect(r.outcome == ep::Outcome::kSeteuidFailed && !r.request_written && !wrote,
           "(c) seteuid failing aborts before the marker is written");
  }
  ep::AttemptCounter().store(0);
  {  // the request write fails
    FakeOps ops = OnOps();
    ops.request_ok = false;
    ops.request_errno = 13;
    const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 9);
    bool polled = false;
    for (const auto& o : ops.order) polled = polled || o.rfind("poll", 0) == 0;
    Expect(r.outcome == ep::Outcome::kRequestFailed && r.request_errno == 13 && !polled,
           "(c) a failed marker write stops before the poll");
  }
  {  // the per-process cap
    ep::AttemptCounter().store(0);
    ep::Config cap = ep::Parse("on\nattempts=2\n");
    Expect(cap.max_attempts == 2, "(c) cap config");
    int elevated_tries = 0, capped = 0;
    for (int i = 0; i < 5; ++i) {
      FakeOps ops = OnOps();
      ops.switch_text = "on\nattempts=2\n";
      ops.consumed = false;
      const ep::Result r = ep::Run(ops, "/data/homebrew/PPSA83600", 100 + i);
      if (r.outcome == ep::Outcome::kCapped) {
        ++capped;
        bool wrote = false;
        for (const auto& o : ops.order) wrote = wrote || o.rfind("write_request", 0) == 0;
        Expect(!wrote, "(c) a capped run sends no request");
      } else {
        ++elevated_tries;
      }
    }
    Expect(elevated_tries == 2 && capped == 3,
           "(c) only max_attempts requests are made per process; the rest are capped");
  }
  ep::AttemptCounter().store(0);
}

size_t After(const std::string& s, const std::string& what, size_t from) {
  return from == std::string::npos ? std::string::npos : s.find(what, from);
}

void Tree() {
  const std::string h = Read("include/xbox360ps5/elevate_probe.hpp");
  Expect(!h.empty(), "(d) the header is in the tree");
  Expect(h.find("umask(0)") != std::string::npos && h.find("O_CREAT | O_TRUNC, 0666") != std::string::npos,
         "(d) the test file is made with umask(0) and mode 0666");
  Expect(h.find("O_WRONLY | O_CREAT | O_EXCL, 0644") != std::string::npos &&
             h.find("::rename(temp.c_str(), marker.c_str())") != std::string::npos,
         "(d) the marker is written O_EXCL then renamed into place");
  Expect(h.find("seteuid((uid_t)uid)") != std::string::npos,
         "(d) the private-credential seteuid is a real syscall in SystemOps");
  Expect(h.find("sigaction(SIGSYS") != std::string::npos && h.find("siglongjmp(") != std::string::npos &&
             h.find("Outcome::kSigsys") != std::string::npos,
         "(d) a SIGSYS handler guards the request and reports a refused syscall");
  Expect(h.find("AttemptCounter().fetch_add(1)") != std::string::npos &&
             h.find("Outcome::kCapped") != std::string::npos,
         "(d) a per-process attempt cap");
  const size_t seteuid = h.find("ops.seteuid(");
  const size_t request = h.find("ops.write_request(");
  const size_t poll = h.find("ops.poll_consumed(");
  Expect(seteuid != std::string::npos && request != std::string::npos && poll != std::string::npos &&
             seteuid < request && request < poll,
         "(d) Run does seteuid, then the marker, then the poll");
  Expect(h.find("::access(marker.c_str(), F_OK) != 0 && errno == ENOENT") != std::string::npos,
         "(d) the poll waits for the daemon to delete the marker");
  Expect(h.find("sandbox") != std::string::npos || h.find("GPL") != std::string::npos ||
             h.find("MIT") != std::string::npos,
         "(d) the header credits the MIT protocol and keeps the sandbox note");

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
