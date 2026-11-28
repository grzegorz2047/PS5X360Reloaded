// SPDX-License-Identifier: MIT
// Fix 169 (tools/source-1.0.2/fixes/169-phone-pose-steady.patch; issue 74): the phone's pose
// frames steadier, the link in the log, and the player's Kinect identity kept through short gaps.
//
//   check-phone-link        run from an assembled tree's root
//
//  (a) phone_link.hpp, the server's side in the log: connection events (opened, closed with why,
//      refused) and their 64-line limit; a frame POST slow in the console gets its own line; the
//      10 s summary (frames and rate, the console's read/handle/write medians and maxima, the
//      waits for the next request, the gaps between frames and those of 500 ms or more, the
//      connection counts), only when due, each window on its own;
//  (b) the page's counts ("link" in its 2 s report): parsed, refused when malformed; a line at
//      once when something failed, timed out, went lost or came back, else every 10 s;
//  (c) SkeletonIdentity: a body back within 2 s in the session it was lost in keeps its tracking
//      id; later, or in another session, or a session switch while tracked, gives a new one; the
//      emergency resume as before; the counts and their log line;
//  (d) the identify answer's profile: the one the body was bound to (PublishNuiFrame remembers
//      it), else the frame's, else the first signed-in slot; none signed in: none; the log budget
//      the 156l lines read as "< 8" for 64 lines;
//  (e) the TV panel's flips to "no phone connected" and back, logged with the quiet time;
//  (f) the tree: the server's per-POST timing, connection events and limit, the page's line;
//      the identify answer and the 156l budgets in the core; the panel flip in the overlay.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/phone_link.hpp"
#include "xbox360ps5/virtual_nui.hpp"
#include "xbox360ps5/body_identity.hpp"
#include "xbox360ps5/phone_qr_panel.hpp"

namespace pl = xbox360ps5::phone_link;
namespace m = xbox360ps5::motion;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  std::string out;
  for (char c : s.str())
    if (c != '\r') out += c;
  return out;
}
static bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

static void CheckServerLines() {
  pl::Link link;
  const auto a = link.Open(1);
  CHECK(a.id == 1 && a.line && *a.line == "Phone link: connection 1 opened (1 open) (fix 169)");
  const auto b = link.Open(2);
  CHECK(b.id == 2);
  auto closed = link.Closed(1, pl::Close::kIdle, 21500, 412, 0, 1);
  CHECK(closed && Has(*closed, "connection 1 closed after 21.5 s and 412 request(s): idle timeout") &&
        Has(*closed, "(1 open) (fix 169)"));
  closed = link.Closed(2, pl::Close::kRead, 900, 3, 104, 0);
  CHECK(closed && Has(*closed, "read error (errno 104)"));
  closed = link.Closed(3, pl::Close::kHandshake, 50, 0, -0x7780, 0);
  CHECK(closed && Has(*closed, "TLS handshake failed (-0x7780)"));
  const auto refused = link.Refused(12);
  CHECK(refused && Has(*refused, "a new connection refused, 12 already open"));

  // Frames: 20 a second for 10 s, one slow in the console, one gap of 1.2 s.
  int64_t t = 100000;
  std::vector<std::string> lines;
  int slow = 0, summaries = 0;
  for (int n = 0; n < 200; ++n) {
    t += n == 100 ? 1200 : 50;
    pl::FrameTimes times{30000, 400, 100, 200};
    if (n == 50) times.read_us = 300000;  // a stalled read in the console
    for (const auto& line : link.Frame(1, times, t)) {
      lines.push_back(line);
      if (Has(line, "a frame POST on connection 1 took")) ++slow;
      if (Has(line, " frame POSTs (")) ++summaries;
    }
  }
  CHECK(slow == 1);
  bool slow_text = false;
  for (const auto& line : lines)
    slow_text |= line == "Phone link: a frame POST on connection 1 took 300 ms in the console (read 300, handle "
                         "0.1, write 0.2 ms) (fix 169)";
  CHECK(slow_text);
  CHECK(summaries == 1);  // 10 s in, not before
  std::string summary;
  for (const auto& line : lines)
    if (Has(line, " frame POSTs (")) summary = line;
  // Due at the frame 10 s after the window's first: 178 frames (one gap of 1.2 s among them).
  CHECK(Has(summary, "Phone link: 10.0 s: 178 frame POSTs (17.8/s);"));
  CHECK(Has(summary, "read/handle/write median 0.4/0.1/0.2 ms, max 300/0.1/0.2 ms"));
  CHECK(Has(summary, "wait for the next request on a connection median 30 ms, max 30 ms"));
  CHECK(Has(summary, "between frames median 50 ms, max 1200 ms, 1 gap(s) of 500 ms or more"));
  CHECK(Has(summary, "connections opened 2, closed 3 (idle 1, errors 2), refused 1 (fix 169)"));
  // The next window: nothing until it is due, unless forced with something in it.
  CHECK(!link.Summary(t + 100));
  const auto rest = link.Summary(t + 100, true);
  CHECK(rest && Has(*rest, ": 22 frame POSTs (") && Has(*rest, "connections opened 0, closed 0"));
  CHECK(!link.Summary(t + 150, true));  // nothing happened since
  const auto c = link.Open(1);
  CHECK(c.id == 3);
  const auto forced = link.Summary(t + 200, true);
  CHECK(forced && Has(*forced, ": 0 frame POSTs") && Has(*forced, "connections opened 1, closed 0"));

  // The limits: 64 connection events, 32 slow frames.
  pl::Link busy;
  int events = 0;
  for (int n = 0; n < 50; ++n) {
    if (busy.Open(1).line) ++events;
    if (busy.Closed(uint32_t(n + 1), pl::Close::kPeer, 10, 1, 0, 0)) ++events;
  }
  CHECK(events == int(pl::kEventLines));
  CHECK(!busy.Refused(12));
  int slow_lines = 0;
  for (int n = 0; n < 50; ++n)
    for (const auto& line : busy.Frame(1, {0, 400000, 0, 0}, 1000 + n))
      if (Has(line, "took")) ++slow_lines;
  CHECK(slow_lines == int(pl::kSlowLines));
  CHECK(std::string(pl::CloseText(pl::Close::kPeer)) == "the phone closed it");
  CHECK(pl::Median({}) == 0 && pl::Median({5, 1, 3}) == 3 && pl::Median({4, 1, 3, 2}) == 2);
}

static void CheckPageLines() {
  const std::string normal = "{\"fps\":20,\"link\":[60,60,0,0,4,90,0,0,0]}";
  const std::string bad = "{\"fps\":20,\"link\":[60,58,1,1,4,940,1,0,0]}";
  const auto parsed = pl::ParsePage(bad);
  CHECK(parsed && parsed->sent == 60 && parsed->answered == 58 && parsed->failed == 1 && parsed->timeouts == 1 &&
        parsed->replaced == 4 && parsed->slowest_ms == 940 && parsed->status_failed == 1 && parsed->Alarm());
  CHECK(!pl::ParsePage(normal)->Alarm());
  CHECK(!pl::ParsePage("{\"fps\":20}"));                          // a page without fix 169
  CHECK(!pl::ParsePage("{\"link\":[1,2,3]}"));                    // too short
  CHECK(!pl::ParsePage("{\"link\":[1,2,3,4,5,6,7,8,-1]}"));       // negative
  CHECK(!pl::ParsePage("[\"link\"]"));
  CHECK(!pl::ParsePage("{\"link\":[1,2,3,4,5,6,7,8,9]}" + std::string(3000, ' ')));  // too long
  pl::PageLines lines;
  const auto first = lines.Line(normal, 1000);
  CHECK(first && *first ==
                     "Phone link (page): frames sent 60, answered 60, failed 0, given up after the timeout 0, "
                     "replaced by a newer one while waiting 4, slowest answer 90 ms; status polls failed 0; the "
                     "page took the PS5 as lost 0 time(s), came back in the same session 0 time(s) (fix 169)");
  CHECK(!lines.Line(normal, 3000));        // a normal report: every 10 s
  CHECK(lines.Line(bad, 5000));            // trouble: at once
  CHECK(lines.Line(bad, 7000));
  CHECK(!lines.Line(normal, 9000));
  CHECK(lines.Line(normal, 17001));
  CHECK(!lines.Line("{\"fps\":1}", 40000));
  const auto lost = pl::ParsePage("{\"link\":[3,0,3,0,0,0,1,1,1]}");
  CHECK(lost && lost->lost == 1 && lost->resumed == 1 && lost->Alarm());
}

static m::Frame Body(uint32_t session) {
  m::Frame f;
  f.session = session;
  f.sequence = 1;
  f.tracked = true;
  for (auto& j : f.joints) j.confidence = 1;
  return f;
}
static m::Frame NoBody(uint32_t session) {
  m::Frame f;
  f.session = session;
  return f;
}

static void CheckIdentity() {
  const Clock::time_point t0 = Clock::now();
  auto at = [&](int ms) { return t0 + milliseconds(ms); };
  const uint64_t new0 = m::identity_new.load(), kept0 = m::identity_kept.load();
  m::SkeletonIdentity id;
  const uint32_t first = id.Update(Body(7), false, at(0));
  CHECK(first == 1);
  CHECK(id.Update(Body(7), false, at(33)) == first);
  // A gap: the body gone (Snapshot's 500 ms rule), back 1.5 s later in the same session.
  CHECK(id.Update(NoBody(7), false, at(600)) == 0);
  CHECK(id.Update(NoBody(7), false, at(1500)) == 0);
  CHECK(id.Update(Body(7), false, at(2100)) == first);
  CHECK(m::identity_kept.load() == kept0 + 1);
  // Back after more than 2 s: a new body.
  id.Update(NoBody(7), false, at(3000));
  const uint32_t second = id.Update(Body(7), false, at(5001));
  CHECK(second != first && second != 0);
  // Back within 2 s, but in another session (the page re-paired): a new body.
  id.Update(NoBody(7), false, at(6000));
  id.Update(NoBody(8), false, at(6100));  // the new session's first frames have no body
  const uint32_t third = id.Update(Body(8), false, at(6200));
  CHECK(third != second);
  // A session switch while tracked: a new body, as before.
  const uint32_t fourth = id.Update(Body(9), false, at(6300));
  CHECK(fourth != third);
  // Exactly 2 s: kept.
  id.Update(NoBody(9), false, at(7000));
  CHECK(id.Update(Body(9), false, at(9000)) == fourth);
  // The emergency resume (fix 107) keeps the id whenever it comes.
  id.Update(NoBody(9), false, at(10000));
  CHECK(id.Update(Body(9), true, at(60000)) == fourth);
  // A clock earlier than the loss (never in the game): a new body, not a kept one.
  id.Update(NoBody(9), false, at(70000));
  CHECK(id.Update(Body(9), false, at(69000)) != fourth);
  CHECK(m::identity_new.load() == new0 + 5 && m::identity_kept.load() == kept0 + 2);
  const auto line = m::IdentityLine();
  CHECK(line && Has(*line, "new, ") && Has(*line, "kept for a body back within 2000 ms in the same phone session"));
  CHECK(!m::IdentityLine());  // nothing new since

  // The profile each body was bound to; the identify answer's profile.
  m::bound_users.Clear();
  CHECK(m::bound_users.Of(5) == m::kUserIndexNone);
  const auto bound = m::PublishNuiFrame(Body(7), 5, 0b0010);
  CHECK(bound.user_index == 1 && m::bound_users.Of(5) == 1);
  m::PublishNuiFrame(NoBody(7), 5, 0b0010);  // a frame in the gap: the binding is gone ...
  CHECK(m::nui_binding.user_index == m::kUserIndexNone && m::bound_users.Of(5) == 1);  // ... the memory is not
  // 156l answered with the frame's binding: FE in a gap. Now the body's profile.
  CHECK(m::AnswerUser(m::bound_users.Of(5), m::kUserIndexNone, 0b0010) == 1);
  CHECK(m::AnswerUser(m::kUserIndexNone, 2, 0b0100) == 2);          // no memory: the frame's
  CHECK(m::AnswerUser(1, 2, 0b0100) == 2);                          // the body's profile signed out
  CHECK(m::AnswerUser(m::kUserIndexNone, m::kUserIndexNone, 0b1000) == 3);  // the first signed in
  CHECK(m::AnswerUser(1, 1, 0) == m::kUserIndexNone);               // nobody signed in
  CHECK(m::AnswerUser(0xFFFFFFFF, 7, 0b0001) == 0);
  for (uint32_t n = 0; n < 20; ++n) m::bound_users.Remember(100 + n, n % 4);
  CHECK(m::bound_users.Of(5) == m::kUserIndexNone);                 // the oldest went
  CHECK(m::bound_users.Of(119) == 3 && m::bound_users.Of(112) == 0);
  m::bound_users.Remember(119, 9);  // not a slot: ignored
  CHECK(m::bound_users.Of(119) == 3);
  m::bound_users.Remember(0, 1);
  CHECK(m::bound_users.Of(0) == m::kUserIndexNone);

  // The 156l lines' budget: "fetch_add(1) < 8" holds 64 times.
  m::LogBudget<m::kIdentifyLogLines> shown;
  int logged = 0;
  for (int n = 0; n < 200; ++n)
    if (shown.fetch_add(1) < 8) ++logged;
  CHECK(logged == 64);
}

static void CheckPanelFlips() {
  const Clock::time_point t0 = Clock::now();
  auto at = [&](int ms) { return t0 + milliseconds(ms); };
  m::PanelFlips flips;
  CHECK(!flips.Note(m::PanelView::kPhoneQr, {}, at(0)));  // the first view is not a flip
  const auto back = flips.Note(m::PanelView::kSkeleton, at(5000), at(5000));
  CHECK(back && *back == "Motion panel: phone frames again after 5.0 s of the code (fix 169)");
  CHECK(!flips.Note(m::PanelView::kSkeleton, at(5000), at(6000)));
  const auto quiet = flips.Note(m::PanelView::kPhoneQr, at(6000), at(9100));
  CHECK(quiet && *quiet == "Motion panel: \"no phone connected\" (the phone's last frame came 3100 ms ago) (fix 169)");
  m::PanelFlips fresh;
  fresh.Note(m::PanelView::kSkeleton, at(0), at(0));
  const auto never = fresh.Note(m::PanelView::kPhoneQr, {}, at(10));
  CHECK(never && Has(*never, "(no phone frame yet)"));
  int lines = 0;
  for (int n = 0; n < 200; ++n)
    if (fresh.Note(n % 2 ? m::PanelView::kPhoneQr : m::PanelView::kSkeleton, at(n), at(n))) ++lines;
  CHECK(lines == int(m::kPanelFlipLines) - 1);
}

static void CheckTree() {
  const std::string tls = ReadText("port/phone_camera_tls.cpp");
  CHECK(Has(tls, "#include \"xbox360ps5/phone_link.hpp\""));
  CHECK(Has(tls, "constexpr int kAuthorityDays = 398, kCertificateDays = 397, kMaxConnections = 12;"));
  // Serve: the bio carries the socket's errors; the times; the reason; the events.
  CHECK(Has(tls, "mbedtls_ssl_set_bio(&ssl, &socket, SendBytes, ReceiveBytes, nullptr);"));
  CHECK(Has(tls, "if (socket.error == EAGAIN || socket.error == EWOULDBLOCK) return phone_link::Close::kIdle;"));
  const size_t open = tls.find("const auto opened = phone_link::stats.Open(connections.load());");
  const size_t frame = tls.find("const bool frame = head.compare(0, 23, \"POST /api/motion/frame \") == 0;");
  const size_t body = tls.find("const auto body_at = std::chrono::steady_clock::now();");
  const size_t route = tls.find("const std::string reply = Route(", body);
  const size_t handled = tls.find("const auto handled_at = Clock::now();", route);
  const size_t write = tls.find("mbedtls_ssl_write(&ssl,", handled);
  const size_t written = tls.find("const auto written_at = Clock::now();", write);
  const size_t timed = tls.find("phone_link::stats.Frame(opened.id, times, motion::SteadyMs())", written);
  const size_t closed = tls.find("phone_link::stats.Closed(opened.id, why,");
  const size_t decrement = tls.rfind("--connections;", closed);
  CHECK(open != std::string::npos && frame != std::string::npos && body != std::string::npos &&
        route != std::string::npos && handled != std::string::npos && write != std::string::npos &&
        written != std::string::npos && timed != std::string::npos && closed != std::string::npos);
  CHECK(open < frame && frame < body && body < route && route < handled && handled < write && write < written &&
        written < timed && timed < closed && decrement < closed);
  CHECK(Has(tls, "const phone_link::FrameTimes times{waited ? Us(first_at - answered_at) : 0, Us(body_at - first_at),"));
  CHECK(Has(tls, "if (const auto ids = motion::IdentityLine()) XELOGW(\"{}\", *ids);"));
  CHECK(Has(tls, "if (const auto line = phone_link::stats.Refused(connections.load())) XELOGW(\"{}\", *line);"));
  const size_t refused = tls.find("phone_link::stats.Refused(");
  const size_t limit = tls.rfind("if (connections.load() >= kMaxConnections) {", refused);
  CHECK(limit != std::string::npos && refused - limit < 200);
  // The page's counts: logged from its report, after the key check.
  const size_t key = tls.find("if (given != key) return Response(\"403 Forbidden\"");
  const size_t page = tls.find("phone_link::page_lines.Line(body, int64_t(phone_latency::NowMs()))");
  const size_t latency = tls.find("if (method == \"POST\" && target == \"/api/motion/latency\") {");
  CHECK(key < latency && latency < page && page != std::string::npos);

  // The core: the identify answer for the body's profile, the three 156l lines' budget.
  const std::string cam = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc");
  const size_t deliver = cam.find("void DeliverIdentityAnswers(KernelState* kernel_state, uint32_t user_index) {");
  const size_t empty = cam.find("if (requests.empty()) return;", deliver);
  const size_t answer_user = cam.find("user_index = xbox360ps5::motion::AnswerUser(\n"
                                      "      xbox360ps5::motion::bound_users.Of(requests.front().tracking_id), user_index,\n"
                                      "      SignedInMask(kernel_state));", deliver);
  const size_t answer = cam.find("const auto answer = xbox360ps5::nui_identity::AnswerFor(user_index);", deliver);
  CHECK(deliver != std::string::npos && empty != std::string::npos && answer_user != std::string::npos &&
        answer != std::string::npos && empty < answer_user && answer_user < answer);
  size_t budgets = 0;
  for (size_t at = cam.find("static xbox360ps5::motion::LogBudget<xbox360ps5::motion::kIdentifyLogLines> shown;");
       at != std::string::npos;
       at = cam.find("static xbox360ps5::motion::LogBudget<xbox360ps5::motion::kIdentifyLogLines> shown;", at + 1))
    ++budgets;
  CHECK(budgets == 3);
  CHECK(Has(cam, "source.identity.Update(pose, emergency == xbox360ps5::motion::EmergencyEvent::kOn);"));
  const std::string nui = ReadText("include/xbox360ps5/virtual_nui.hpp");
  CHECK(Has(nui, "bound_users.Remember(nui_binding.tracking_id, slot);"));

  // The overlay: the panel's flips logged before it draws.
  const std::string main_text = ReadText("src/canary_game_main.cpp");
  const size_t flip = main_text.find("if (const auto flip = xbox360ps5::motion::panel_flips.Note(");
  const size_t qr = main_text.find("if (phone_qr_.size && xbox360ps5::motion::ChoosePanelView(pose.received, now, phone_url_) ==");
  CHECK(flip != std::string::npos && qr != std::string::npos && flip < qr && qr - flip < 400);
}

int main() {
  CheckServerLines();
  CheckPageLines();
  CheckIdentity();
  CheckPanelFlips();
  CheckTree();
  if (failures) {
    std::printf("check-phone-link: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-phone-link: ok\n");
  return 0;
}
