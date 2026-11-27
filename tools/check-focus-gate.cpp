// SPDX-License-Identifier: MIT
// Host check of fix 190 (tools/source-1.0.2/fixes/190-app-focus-input-gate.patch): while the
// system's UI is on top of the app (control center, quick menu) or the app runs in the background,
// NativePad::Read hands out a resting pad, and back on top no press the app did not get comes
// through. Drives include/xbox360ps5/focus_gate.hpp with a sample of PadSample's fields, then
// checks that src/canary_game_main.cpp of the assembled tree (run from the tree's root) wires it:
// the status call looked up at run time (no weak import), the poll in Read, the gate on Read's sample, the switch file.
#if !__has_include("xbox360ps5/focus_gate.hpp")
#include <cstdio>
int main() {
  std::puts("skip check-focus-gate.cpp (fix 190 left out of this tree)");
  return 0;
}
#else
#include "xbox360ps5/focus_gate.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fg = xbox360ps5::focus_gate;

// PadSample's fields (include/xbox360ps5/dualsense_input.hpp), without the core's headers.
struct Sample {
  uint32_t buttons = 0;
  uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
  bool connected = false;
};

constexpr uint32_t kUp = 0x10, kCross = 0x4000, kCircle = 0x2000, kSquare = 0x8000;

int failures = 0;
void Expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

Sample Pressed(uint32_t buttons, uint8_t lx = 128, uint8_t l2 = 0) {
  Sample s;
  s.connected = true;
  s.buttons = buttons;
  s.lx = lx;
  s.l2 = l2;
  return s;
}

bool Same(const Sample& a, const Sample& b) {
  return a.buttons == b.buttons && a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry &&
         a.l2 == b.l2 && a.r2 == b.r2 && a.connected == b.connected;
}

bool Resting(const Sample& s) {
  return s.buttons == 0 && s.lx == 128 && s.ly == 128 && s.rx == 128 && s.ry == 128 && s.l2 == 0 &&
         s.r2 == 0;
}

fg::Status Reading(bool overlaid, bool background) {
  fg::Status s;
  s.valid = true;
  s.overlaid = overlaid;
  s.background = background;
  return s;
}
const fg::Status kOnTop = Reading(false, false);
const fg::Status kOverlaid = Reading(true, false);
const fg::Status kBackground = Reading(false, true);

// The launcher's key edges as NativeWindow::SubmitUiPad makes them from the samples.
struct UiKeys {
  uint32_t last = 0;
  int downs = 0;
  void Submit(const Sample& s) {
    const uint32_t current = s.connected ? s.buttons : 0;
    for (uint32_t bit : {kUp, kCross, kCircle, kSquare})
      if ((current & bit) && !(last & bit)) ++downs;
    last = current;
  }
};

void CheckParse() {
  std::array<uint8_t, fg::kStatusBufferSize> record{};
  Expect(fg::Parse(0, record.data(), record.size()).valid, "a zero record is a reading");
  Expect(!fg::Parse(0, record.data(), record.size()).overlaid, "a zero record is on top");
  record[4] = 1;
  auto s = fg::Parse(0, record.data(), record.size());
  Expect(s.valid && s.overlaid && !s.background, "byte 4 is isSystemUiOverlaid");
  record[4] = 0;
  record[5] = 1;
  s = fg::Parse(0, record.data(), record.size());
  Expect(s.valid && !s.overlaid && s.background, "byte 5 is isInBackgroundExecution");
  record[0] = 0xff;  // eventNum is not a flag
  record[6] = 0xff;  // nor are the bytes past the two flags
  Expect(fg::Parse(0, record.data(), record.size()).valid, "eventNum and later bytes do not matter");
  Expect(!fg::Parse(int(0x80A10003u), record.data(), record.size()).valid, "a failed call is no reading");
  Expect(!fg::Parse(1, record.data(), record.size()).valid, "a call that does not return 0 is no reading");
  record[5] = 2;
  Expect(!fg::Parse(0, record.data(), record.size()).valid, "a flag byte that is not 0 or 1 is no reading");
  record[5] = 0;
  record[4] = 0x80;
  Expect(!fg::Parse(0, record.data(), record.size()).valid, "an odd overlaid byte is no reading");
  Expect(!fg::Parse(0, nullptr, record.size()).valid, "no record is no reading");
  Expect(!fg::Parse(0, record.data(), 5).valid, "a record too short for both flags is no reading");
  Expect(fg::kStatusBufferSize >= 134, "the buffer holds the 134-byte SceSystemServiceStatus");
}

void CheckSwitch() {
  Expect(!fg::ParseSwitch("off"), "\"off\" turns the gate off");
  Expect(!fg::ParseSwitch("OFF\r\n"), "\"OFF\" with a CRLF turns it off");
  Expect(!fg::ParseSwitch("  Off \n"), "spaces around \"Off\"");
  Expect(!fg::ParseSwitch("0\n"), "\"0\" turns it off");
  Expect(fg::ParseSwitch("on"), "\"on\" leaves it on");
  Expect(fg::ParseSwitch(""), "an empty line leaves it on");
  Expect(fg::ParseSwitch("offset"), "\"offset\" is not \"off\"");
  Expect(fg::ParseSwitch("of"), "\"of\" is not \"off\"");
}

void CheckOnTopPassesThrough() {
  fg::Gate gate;
  Expect(gate.Update(kOnTop) == fg::Change::kNone, "on top at the start: no change");
  const Sample raw = Pressed(kCross | kUp, 20, 200);
  Expect(Same(gate.Apply(0, raw), raw), "on top: the sample passes as read");
  Sample other = raw;
  other.rx = 250;
  other.r2 = 90;
  other.buttons = kSquare | fg::kR2;
  Expect(Same(gate.Apply(3, other), other), "on top: slot 4 too");
  Expect(Same(gate.Apply(7, other), other), "a slot past the four passes as read");
  // A gate that never had a reading (status call slow or failing) passes as before.
  fg::Gate fresh;
  Expect(Same(fresh.Apply(0, raw), raw), "no reading yet: the sample passes");
}

void CheckOverlaidRests() {
  fg::Gate gate;
  gate.Update(kOnTop);
  Expect(gate.Update(kOverlaid) == fg::Change::kPaused, "the control center pauses the pad");
  Expect(gate.Blocked(), "blocked while overlaid");
  Sample raw = Pressed(kCross | kUp | fg::kL2, 3, 255);
  raw.ry = 0;
  raw.r2 = 77;
  const Sample out = gate.Apply(0, raw);
  Expect(Resting(out), "overlaid: no buttons, sticks centred, triggers released");
  Expect(out.connected, "overlaid: the pad stays connected (no disconnect for the game)");
  Expect(Resting(gate.Apply(2, raw)), "overlaid: every player's pad rests");
  Expect(gate.Update(kOverlaid) == fg::Change::kNone, "a second overlaid reading changes nothing");
  Expect(gate.Update(Reading(true, true)) == fg::Change::kNone, "overlaid and background: still paused");
  Expect(Resting(gate.Apply(0, raw)), "still resting");
}

void CheckBackgroundRests() {
  fg::Gate gate;
  gate.Update(kOnTop);
  Expect(gate.Update(kBackground) == fg::Change::kPaused, "the app in the background pauses the pad");
  Expect(Resting(gate.Apply(0, Pressed(kSquare, 255))), "background: resting pad");
  Expect(gate.Update(kOnTop) == fg::Change::kResumed, "back on top resumes");
  Expect(Same(gate.Apply(0, Pressed(0, 255)), Pressed(0, 255)), "after the return, the stick at once");
}

void CheckNoPhantomPress() {
  fg::Gate gate;
  UiKeys keys;
  gate.Update(kOnTop);
  keys.Submit(gate.Apply(0, Pressed(0)));
  // The player opens the control center and moves through it; Cross picks an item there.
  gate.Update(kOverlaid);
  for (uint32_t b : {kUp, 0u, kUp, 0u, kCross}) keys.Submit(gate.Apply(0, Pressed(b)));
  Expect(keys.downs == 0, "no key reaches the launcher while overlaid");
  // The menu closes while Cross is still down: the app must not see a Cross press.
  gate.Update(kOnTop);
  Sample out = gate.Apply(0, Pressed(kCross, 128, 0));
  keys.Submit(out);
  Expect(!(out.buttons & kCross), "Cross held across the return is masked");
  Expect(keys.downs == 0, "no phantom press on the return to the top");
  // Up pressed afresh while Cross is still held passes; Cross does not.
  out = gate.Apply(0, Pressed(kCross | kUp));
  keys.Submit(out);
  Expect(out.buttons == kUp, "a new press passes while the held one stays masked");
  Expect(keys.downs == 1, "the new press is one key down");
  // Cross released and pressed again: a real press.
  keys.Submit(gate.Apply(0, Pressed(kUp)));
  keys.Submit(gate.Apply(0, Pressed(0)));
  out = gate.Apply(0, Pressed(kCross));
  keys.Submit(out);
  Expect(out.buttons == kCross, "Cross pressed again after its release passes");
  Expect(keys.downs == 2, "and is one key down");
}

void CheckHeldTrigger() {
  fg::Gate gate;
  gate.Update(kOnTop);
  gate.Update(kOverlaid);
  Sample held = Pressed(fg::kL2, 128, 255);
  held.r2 = 40;
  gate.Apply(0, held);
  gate.Update(kOnTop);
  Sample out = gate.Apply(0, held);
  Expect(!(out.buttons & fg::kL2) && out.l2 == 0, "L2 held across the return: button and analog masked");
  Expect(out.r2 == 40, "R2's analog without its button passes");
  out = gate.Apply(0, Pressed(0, 128, 30));
  Expect(out.l2 == 30, "L2 released: its analog value is back");
}

void CheckSlotsApart() {
  fg::Gate gate;
  gate.Update(kOnTop);
  gate.Update(kOverlaid);
  gate.Update(kOnTop);
  // Player 2 holds Square at the return, player 1 holds nothing.
  Expect(gate.Apply(1, Pressed(kSquare)).buttons == 0, "player 2's held Square is masked");
  Expect(gate.Apply(0, Pressed(0)).buttons == 0, "player 1 holds nothing");
  Expect(gate.Apply(0, Pressed(kSquare)).buttons == kSquare, "player 1's new Square passes");
  Expect(gate.Apply(1, Pressed(kSquare)).buttons == 0, "player 2's Square stays masked until released");
  // A slot that did not read during a short pause still masks what it holds after it.
  gate.Update(kOverlaid);
  gate.Update(kOnTop);
  Expect(gate.Apply(2, Pressed(kCircle)).buttons == 0, "a pause no read saw still masks the held button");
}

void CheckNoGateBeforeOnTop() {
  // A record misread as "always overlaid" must never leave the app without a pad.
  fg::Gate gate;
  Expect(gate.Update(kOverlaid) == fg::Change::kNone, "overlaid before any on-top reading: no pause");
  Expect(!gate.Blocked(), "not blocked before an on-top reading");
  Expect(Same(gate.Apply(0, Pressed(kCross)), Pressed(kCross)), "the pad passes");
  Expect(gate.Update(kBackground) == fg::Change::kNone, "background before on top: no pause");
  Expect(gate.Update(kOnTop) == fg::Change::kNone, "the first on-top reading");
  Expect(gate.Update(kOverlaid) == fg::Change::kPaused, "then overlaid pauses");
}

void CheckFailures() {
  fg::Gate gate;
  gate.Update(kOnTop);
  gate.Update(kOverlaid);
  Expect(gate.Update(fg::Status{}) == fg::Change::kResumed, "a failed reading lets the pad through");
  Expect(!gate.Blocked(), "not blocked after a failed reading");
  Expect(gate.Apply(0, Pressed(kCross)).buttons == 0, "the button held across it is masked");
  Expect(gate.Update(kOverlaid) == fg::Change::kPaused, "a good reading pauses again");
  // Failures in a row turn the gate off for good, and it stops reading.
  fg::Change last = fg::Change::kNone;
  for (int n = 0; n < fg::kMaxFailures; ++n) last = gate.Update(fg::Status{});
  Expect(last == fg::Change::kDisabled, "kMaxFailures failed readings turn the gate off");
  Expect(gate.Disabled() && !gate.Blocked(), "off and not blocked");
  Expect(!gate.PollDue(1000000), "off: the status is not read again");
  Expect(gate.Update(kOverlaid) == fg::Change::kNone, "off: an overlaid reading changes nothing");
  gate.Apply(0, Pressed(0));  // the first read after the pause ends (nothing held)
  Expect(Same(gate.Apply(0, Pressed(kUp, 9)), Pressed(kUp, 9)), "off: the pad passes as read");
  // Fewer failures than that, with good readings between, never turn it off.
  fg::Gate flaky;
  flaky.Update(kOnTop);
  for (int round = 0; round < 10; ++round) {
    for (int n = 0; n < fg::kMaxFailures - 1; ++n) flaky.Update(fg::Status{});
    flaky.Update(kOnTop);
  }
  Expect(!flaky.Disabled(), "failures broken by good readings do not turn the gate off");
}

void CheckDisable() {
  fg::Gate gate;
  gate.Update(kOnTop);
  gate.Update(kOverlaid);
  gate.Apply(0, Pressed(kCross));
  Expect(gate.Disable() == fg::Change::kDisabled, "Disable turns the gate off");
  Expect(gate.Disable() == fg::Change::kNone, "a second Disable changes nothing");
  Expect(!gate.Blocked(), "a pad paused at Disable comes back");
  Expect(gate.Apply(0, Pressed(kCross)).buttons == 0, "with the held button masked");
  Expect(gate.Apply(0, Pressed(0)).buttons == 0 && gate.Apply(0, Pressed(kCross)).buttons == kCross,
         "and a real press after its release");
  // The status call missing: off before any reading, never polled.
  fg::Gate missing;
  missing.Disable();
  Expect(!missing.PollDue(0), "a gate turned off at the start never reads the status");
  Expect(Same(missing.Apply(0, Pressed(kSquare)), Pressed(kSquare)), "and passes the pad");
}

void CheckPollPace() {
  fg::Gate gate;
  Expect(gate.PollDue(5000), "the first read polls");
  Expect(!gate.PollDue(5000), "a second read in the same moment does not");
  Expect(!gate.PollDue(5000 + fg::kPollIntervalMs - 1), "nor within the interval");
  Expect(gate.PollDue(5000 + fg::kPollIntervalMs), "the interval later it polls again");
  Expect(gate.PollDue(5000 + 10 * fg::kPollIntervalMs), "and after a long gap");
  Expect(fg::kPollIntervalMs <= 20, "about once a frame (60 frames a second)");
}

std::string ReadFile(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

void CheckTreeWiring() {
  const std::string main_cpp = ReadFile("src/canary_game_main.cpp");
  if (main_cpp.empty()) {
    std::printf("FAIL: src/canary_game_main.cpp not found (run from the assembled tree's root)\n");
    ++failures;
    return;
  }
  const auto has = [&](const char* text) { return main_cpp.find(text) != std::string::npos; };
  Expect(has("#include \"xbox360ps5/focus_gate.hpp\""), "canary_game_main.cpp includes the gate");
  // The status call is looked up at run time (as notify.cpp does), never imported: the build turns
  // every weak undefined symbol outside Mesa into an error, and a strong import could be missing.
  Expect(!has("__attribute__((weak)) int sceSystemServiceGetStatus"),
         "no weak import of the status call (the build refuses weak references outside Mesa)");
  Expect(!has("int sceSystemServiceGetStatus("), "no import of the status call at all");
  Expect(has("sceKernelDlsym(handle, \"sceSystemServiceGetStatus\", &address)"),
         "the status call is looked up in the system library");
  Expect(has("return found >= 0 && address != nullptr ? reinterpret_cast<StatusCall>(address) : nullptr;"),
         "a failed lookup gives no call");
  Expect(has("status_call_ = FindStatusCall();\r\n    if (status_call_ == nullptr) {\r\n      focus_gate_.Disable();") ||
             has("status_call_ = FindStatusCall();\n    if (status_call_ == nullptr) {\n      focus_gate_.Disable();"),
         "without the call the gate is off (the pad is read as before)");
  Expect(has("PollFocus(now);"), "Read polls the status");
  Expect(has("if (!focus_gate_.PollDue(now_ms) || status_call_ == nullptr) return;"),
         "the poll is paced, off when disabled, and never calls a null address");
  Expect(has("return focus_gate_.Apply(slot, state);"), "Read's sample goes through the gate");
  Expect(has("XBOX360PS5_DATA_ROOT \"/focus-gate.txt\""), "the switch file turns it off");
  // The status is read only behind the pace check, never before it in PollFocus.
  const size_t poll = main_cpp.find("void PollFocus(");
  const size_t due = main_cpp.find("focus_gate_.PollDue(", poll);
  const size_t call = main_cpp.find("status_call_(record.data())", poll);
  Expect(poll != std::string::npos && due != std::string::npos && call != std::string::npos && due < call,
         "PollFocus checks PollDue and the call before it calls the status");
  // Every sample of a read pad goes through the gate: Read returns the read sample nowhere else.
  const size_t read = main_cpp.find("xbox360ps5::PadSample Read(size_t slot = 0) {");
  const size_t read_end = main_cpp.find("\n  bool Rumble(", read);
  Expect(read != std::string::npos && read_end != std::string::npos, "NativePad::Read found");
  if (read != std::string::npos && read_end != std::string::npos) {
    const std::string body = main_cpp.substr(read, read_end - read);
    const size_t filled = body.find("state.buttons = ");
    const size_t gated = body.find("return focus_gate_.Apply(slot, state);");
    Expect(filled != std::string::npos && gated != std::string::npos && filled < gated,
           "the read sample is returned through the gate");
    Expect(body.find("return state;", filled) == std::string::npos,
           "no path returns the read sample without the gate");
  }
}

}  // namespace

int main() {
  CheckParse();
  CheckSwitch();
  CheckOnTopPassesThrough();
  CheckOverlaidRests();
  CheckBackgroundRests();
  CheckNoPhantomPress();
  CheckHeldTrigger();
  CheckSlotsApart();
  CheckNoGateBeforeOnTop();
  CheckFailures();
  CheckDisable();
  CheckPollPace();
  CheckTreeWiring();
  if (failures) {
    std::printf("check-focus-gate: %d failure(s)\n", failures);
    return 1;
  }
  std::puts("check-focus-gate: all checks passed");
  return 0;
}
#endif
