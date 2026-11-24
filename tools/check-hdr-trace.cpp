// SPDX-License-Identifier: MIT
// Fix 158 (tools/source-1.0.2/fixes/158-hdr-trace.patch): "HDR trace", a diagnostic behind
// <data>/hdr-trace.txt for Grand Theft Auto V's brief whole-picture darkening.
//  (a) the switch file: a first word "on" (any case, alone) turns it on, anything else is off;
//      "key <hex>" and "max-bytes <n>" (at most 8 MiB) lines; unknown lines are counted;
//  (b) value decoding: IEEE halves (normal, denormal, 65504, infinity, NaN) and the guest's byte
//      orders (8in16 for 16_16_16_16_FLOAT, 8in32 for 32_FLOAT, 16in32, none);
//  (c) a copy's statistics: first texel, R/G/B mean (alpha left out), largest, NaN, infinity,
//      negative and over-31.875 counts;
//  (d) the jump rule: 8 copies of history first, a value outside 2x of the median of up to 16
//      recent copies is a jump, back within 1.5x of where it left;
//  (e) a model of the darkening: a 1x1 32_FLOAT luminance key at 0.2 for 20 frames, 1.6 for 3
//      frames, then 0.2 again gives one jump line (console clock, frame, last frame length, source)
//      and one "back" line; steady and slowly changing values give none; non-float and too large
//      keys are not traced unless listed; the 30 s summary;
//  (f) occlusion queries: a fast-mode guess "visible" for a hidden query gives a line once a second
//      and counts in the summary; rebases are counted;
//  (g) off: no lines and no state without the file; on -> off says so;
//  (h) the assembled tree: the switch is polled in IssueCopy after the resolve, the source is taken
//      from RB_COPY_CONTROL / RB_COLOR_INFO / RB_SURFACE_INFO, the copy's bytes are traced right
//      after they go back to guest memory, and the fast mode's guesses are compared in
//      PumpPendingRetire, all under XE_PLATFORM_PS5.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/hdr_trace.hpp"

namespace {
namespace fs = std::filesystem;
namespace ht = xbox360ps5::hdr_trace;

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
bool Has(const std::vector<std::string>& lines, const std::string& part) {
  for (const std::string& line : lines) {
    if (line.find(part) != std::string::npos) return true;
  }
  return false;
}
size_t Count(const std::vector<std::string>& lines, const std::string& part) {
  size_t n = 0;
  for (const std::string& line : lines) n += line.find(part) != std::string::npos;
  return n;
}
void Dump(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) std::printf("  | %s\n", line.c_str());
}

void Switch() {
  Expect(ht::ParseConfig("on").on && ht::ParseConfig("  ON\r\n").on && ht::ParseConfig("\nOn\n").on,
         "(a) \"on\" in any case turns it on");
  Expect(!ht::ParseConfig("").on && !ht::ParseConfig("off").on && !ht::ParseConfig("onx").on &&
             !ht::ParseConfig("1").on && !ht::ParseConfig("on please").on &&
             !ht::ParseConfig("key 061D3000\non").on,
         "(a) anything else is off");
  const ht::Config config =
      ht::ParseConfig("on\r\nkey 061D3000\nKEY 0x1F0E8000\nmax-bytes 262144\nwhatever 1\nkey zz\n");
  Expect(config.on && config.keys.size() == 2 && config.keys[0] == 0x061D3000u &&
             config.keys[1] == 0x1F0E8000u && config.max_bytes == 262144 &&
             config.not_understood == 2,
         "(a) key and max-bytes lines, two not understood");
  Expect(ht::ParseConfig("on").max_bytes == 65536 &&
             ht::ParseConfig("on\nmax-bytes 99999999").max_bytes == 8u * 1024 * 1024,
         "(a) 64 KiB by default, at most 8 MiB");
}

void Decoding() {
  Expect(ht::HalfToFloat(0x3C00) == 1.0f && ht::HalfToFloat(0xC000) == -2.0f &&
             ht::HalfToFloat(0x7BFF) == 65504.0f && ht::HalfToFloat(0x0000) == 0.0f &&
             ht::HalfToFloat(0x0001) == std::ldexp(1.0f, -24) &&
             ht::HalfToFloat(0x03FF) == std::ldexp(1023.0f, -24) &&
             ht::HalfToFloat(0x4FF0) == 31.75f,
         "(b) halves: 1, -2, 65504, 0, the smallest and largest denormals, 31.75");
  Expect(std::isinf(ht::HalfToFloat(0x7C00)) && ht::HalfToFloat(0x7C00) > 0 &&
             std::isinf(ht::HalfToFloat(0xFC00)) && ht::HalfToFloat(0xFC00) < 0 &&
             std::isnan(ht::HalfToFloat(0x7E00)),
         "(b) halves: infinities and NaN");
  // 16_16_16_16_FLOAT, 8in16: each half big-endian. R=1, G=2, B=0.5, A=1.
  const uint8_t e1[8] = {0x3C, 0x00, 0x40, 0x00, 0x38, 0x00, 0x3C, 0x00};
  const ht::FloatLayout f32 = ht::LayoutOf(32);
  Expect(ht::ElementValue(e1, 0, f32, 1) == 1.0f && ht::ElementValue(e1, 2, f32, 1) == 2.0f &&
             ht::ElementValue(e1, 4, f32, 1) == 0.5f && ht::ElementValue(e1, 6, f32, 1) == 1.0f,
         "(b) 16_16_16_16_FLOAT with 8in16");
  // The same halves, 8in32 (each 32-bit word reversed: G,R order inside the word).
  const uint8_t e2h[4] = {0x40, 0x00, 0x3C, 0x00};
  Expect(ht::ElementValue(e2h, 0, f32, 2) == 1.0f && ht::ElementValue(e2h, 2, f32, 2) == 2.0f,
         "(b) halves with 8in32");
  const uint8_t e3h[4] = {0x00, 0x40, 0x00, 0x3C};
  Expect(ht::ElementValue(e3h, 0, f32, 3) == 1.0f && ht::ElementValue(e3h, 2, f32, 3) == 2.0f,
         "(b) halves with 16in32");
  const uint8_t e0h[4] = {0x00, 0x3C, 0x00, 0x40};
  Expect(ht::ElementValue(e0h, 0, f32, 0) == 1.0f && ht::ElementValue(e0h, 2, f32, 0) == 2.0f,
         "(b) halves with no swap");
  // 32_FLOAT, 8in32: big-endian floats. 0.5 = 3F000000, -3 = C0400000.
  const uint8_t e2f[8] = {0x3F, 0x00, 0x00, 0x00, 0xC0, 0x40, 0x00, 0x00};
  const ht::FloatLayout f36 = ht::LayoutOf(36);
  Expect(f36.channels == 1 && f36.bytes == 4 && ht::ElementValue(e2f, 0, f36, 2) == 0.5f &&
             ht::ElementValue(e2f, 4, f36, 2) == -3.0f,
         "(b) 32_FLOAT with 8in32");
  Expect(ht::LayoutOf(6).channels == 0 && ht::LayoutOf(7).channels == 0 &&
             ht::LayoutOf(30).channels == 1 && ht::LayoutOf(31).channels == 2 &&
             ht::LayoutOf(37).channels == 2 && ht::LayoutOf(38).channels == 4 &&
             ht::LayoutOf(38).bytes == 4,
         "(b) only the float destination formats are decoded");
}

std::vector<uint8_t> Float32Be(const std::vector<float>& values) {
  std::vector<uint8_t> bytes;
  for (float value : values) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    bytes.push_back(uint8_t(bits >> 24));
    bytes.push_back(uint8_t(bits >> 16));
    bytes.push_back(uint8_t(bits >> 8));
    bytes.push_back(uint8_t(bits));
  }
  return bytes;
}

std::vector<uint8_t> Half4Be(const std::vector<uint16_t>& halves) {
  std::vector<uint8_t> bytes;
  for (uint16_t half : halves) {
    bytes.push_back(uint8_t(half >> 8));
    bytes.push_back(uint8_t(half));
  }
  return bytes;
}

void Statistics() {
  // Two texels of 16_16_16_16_FLOAT: (1, 2, NaN, 40) and (-1, inf, 33, 0.5). Alpha 40 is over
  // 31.875 but not in the mean.
  const std::vector<uint8_t> bytes =
      Half4Be({0x3C00, 0x4000, 0x7E00, 0x5100, 0xBC00, 0x7C00, 0x5020, 0x3800});
  const ht::Stats stats = ht::Scan(bytes.data(), uint32_t(bytes.size()), 32, 1);
  Expect(stats.valid && stats.texels == 2 && stats.first == 1.0f && stats.nan == 1 &&
             stats.inf == 1 && stats.negative == 1 && stats.over_7e3 == 2 &&
             stats.largest == 40.0f,
         "(c) first texel, NaN, infinity, negative, over 31.875 and largest");
  Expect(std::fabs(stats.mean - (1.0f + 2.0f - 1.0f + 33.0f) / 4.0f) < 1e-5f,
         "(c) the mean is over the finite R, G and B values");
  Expect(!ht::Scan(bytes.data(), uint32_t(bytes.size()), 6, 2).valid &&
             !ht::Scan(nullptr, 16, 36, 2).valid && !ht::Scan(bytes.data(), 2, 36, 2).valid,
         "(c) non-float formats, no bytes and short copies give nothing");
}

void JumpRule() {
  ht::Series series;
  float factor = 0, reference = 0;
  int result = 0;
  for (int i = 0; i < 7; ++i) result |= series.Add(i % 2 ? 0.1f : 5.0f, &factor, &reference, i);
  Expect(result == 0, "(d) no jump before 8 copies of history, however much the values vary");
  ht::Series steady;
  for (int i = 0; i < 40; ++i) result |= steady.Add(0.2f * (1.0f + 0.02f * i), nullptr, nullptr, i);
  Expect(result == 0, "(d) a value changing 2% a frame (adaptation, a tunnel) is no jump");
  Expect(steady.Add(0.70f, &factor, &reference, 40) == 1 && factor > 2.0f,
         "(d) 2.1x the median (0.328, of the ramp's last 16) is a jump");
  Expect(steady.Add(0.55f, &factor, &reference, 41) == 0 && steady.away,
         "(d) still away above 1.5x");
  Expect(steady.Add(0.45f, &factor, &reference, 42) == -1 && !steady.away && steady.jumps == 1,
         "(d) back within 1.5x of where it left");
  ht::Series dark;
  for (int i = 0; i < 10; ++i) dark.Add(0.5f, nullptr, nullptr, i);
  Expect(dark.Add(0.2f, &factor, &reference, 10) == 1 && std::fabs(factor - 2.5f) < 1e-4f,
         "(d) a drop to 0.4x is a jump too");
  ht::Series nan;
  for (int i = 0; i < 10; ++i) nan.Add(0.5f, nullptr, nullptr, i);
  Expect(nan.Add(std::nanf(""), &factor, &reference, 10) == 1 && std::isinf(factor),
         "(d) NaN is a jump");
  Expect(ht::Factor(0.0f, 0.0f) == 1.0f && std::isinf(ht::Factor(0.0f, 1.0f)) &&
             std::isinf(ht::Factor(-1.0f, 1.0f)) && ht::Factor(2.0f, 1.0f) == 2.0f,
         "(d) factors: both zero, one zero, sign change, 2x");
  Expect(ht::ClockText(1728052680123LL) == "14:38:00.123 UTC" &&
             ht::ClockText(86399999LL) == "23:59:59.999 UTC",
         "(e) the console clock as the video files have it");
}

ht::Source LuminanceSource() {
  ht::Source source;
  source.select = 0;
  source.format = 14;  // 32_FLOAT render target
  source.dest_format = 36;
  source.dest_endian = 2;
  return source;
}

void Darkening() {
  ht::Tracker tracker;
  std::vector<std::string> lines;
  tracker.Configure("on\n", true, "/data/homebrew/PPSA83600/hdr-trace.txt", &lines);
  Expect(tracker.Enabled() && Count(lines, "HDR trace (fix 158): on from") == 1,
         "(e) the switch on is logged once");
  lines.clear();
  const ht::Source luminance = LuminanceSource();
  ht::Source hdr;  // a 640x384 7e3 picture resolved to 16_16_16_16_FLOAT: too large by default
  hdr.format = 3;
  hdr.dest_format = 32;
  hdr.dest_endian = 1;
  ht::Source color = luminance;  // an 8_8_8_8 resolve: not traced
  color.format = 0;
  color.dest_format = 6;
  double now = 1000.0;
  const int64_t clock0 = 1728052680123LL - 21 * 70;
  std::vector<float> values;
  for (int frame = 1; frame <= 40; ++frame) {
    const float value = frame >= 22 && frame <= 24 ? 1.6f : 0.2f + 0.001f * (frame % 3);
    values.push_back(value);
    now += frame == 22 ? 0.120 : 0.070;
    std::vector<float> texels(1024, 0.05f);  // a 32x32 tile; the rest is old contents
    texels[0] = value;
    const std::vector<uint8_t> bytes = Float32Be(texels);
    tracker.NoteResolve(0x061D3000, 4096, luminance, frame, now, &lines);
    tracker.NoteResolve(0x0623A000, 1966080, hdr, frame, now, &lines);
    tracker.NoteResolve(0x06708000, 3768320, color, frame, now, &lines);
    tracker.NoteBytes(0x061D3000, 4096, bytes.data(), 36, 2, frame, now,
                      clock0 + int64_t(frame - 1) * 70, &lines);
    std::vector<uint8_t> big(1966080, 0);
    tracker.NoteBytes(0x0623A000, 1966080, big.data(), 32, 1, frame, now, clock0, &lines);
  }
  Expect(Count(lines, "key 061D3000/4096 traced, from rt0 32_FLOAT, 1x MSAA, exp bias 0 -> f36 e2") ==
             1 &&
             !Has(lines, "0623A000/1966080 traced") && !Has(lines, "06708000"),
         "(e) the small float key is traced once with its source; the 1.9 MB and 8_8_8_8 ones not");
  Expect(Count(lines, "first texel 0.201 -> 1.6") + Count(lines, "first texel 0.2 -> 1.6") +
                 Count(lines, "first texel 0.202 -> 1.6") ==
             1,
         "(e) one jump line for the first texel");
  Expect(Has(lines, "14:38:00.123 UTC frame 22 (the last frame took 120 ms): key 061D3000/4096"),
         "(e) the jump line has the console clock, the frame and the last frame's length");
  Expect(Count(lines, "first texel back to") == 1 && Has(lines, "after 3 frames"),
         "(e) one back line, 3 frames later");
  Expect(!Has(lines, "R,G,B mean"),
         "(e) the mean of a 32x32 tile with one live texel moves too little for a line");
  lines.clear();
  tracker.Summary(now, &lines);
  Expect(lines.size() == 2 && Has(lines, "1 float keys copied back") &&
             Has(lines, "longest 120 ms, 1 over 100 ms") &&
             Has(lines, "061D3000/4096 from rt0 32_FLOAT") && Has(lines, "40 copies") &&
             Has(lines, "max 1.6") && Has(lines, "jumps 1"),
         "(e) the summary: keys, frame lengths, the key's line with its jump");
  if (failures) Dump(lines);
  lines.clear();
  tracker.Summary(now + 30, &lines);
  Expect(lines.size() == 1 && Has(lines, "0 float keys copied back"),
         "(e) a summary covers its own window only");

  // A listed key is traced whatever its size, and max-bytes widens the float limit.
  ht::Tracker listed;
  lines.clear();
  listed.Configure("on\nkey 06708000\nmax-bytes 2000000\n", true, "x", &lines);
  Expect(Has(lines, "float resolves up to 2000000 bytes, and the keys 06708000"),
         "(e) the listed keys and the limit are logged");
  lines.clear();
  listed.NoteResolve(0x06708000, 3768320, color, 1, 1.0, &lines);
  listed.NoteResolve(0x0623A000, 1966080, hdr, 1, 1.0, &lines);
  Expect(Has(lines, "06708000/3768320 traced") && Has(lines, "0623A000/1966080 traced, from rt0 7e3"),
         "(e) listed and widened keys are traced");
}

void Occlusion() {
  ht::Tracker tracker;
  std::vector<std::string> lines;
  tracker.Configure("on", true, "x", &lines);
  lines.clear();
  const ht::Source source = LuminanceSource();
  tracker.NoteResolve(0x061D3000, 4096, source, 1, 100.0, &lines);  // opens the window
  double now = 100.0;
  for (int i = 0; i < 30; ++i) {
    now += 0.05;
    const bool hidden = i >= 10 && i < 14;
    tracker.NoteZpd(0x1F0F0000 + 0x20 * (i % 3), 500, hidden ? 0 : 480, now, 1728052680123LL,
                    &lines);
  }
  Expect(Count(lines, "occlusion queries in the last second") == 1 &&
             Has(lines, "4 guessed visible but hidden"),
         "(f) a guess \"visible\" for hidden queries gives one line for that second");
  tracker.NoteZpd(0x1F0F0000, 0, 300, now + 0.01, 0, &lines);
  tracker.NoteZpd(0x1F0F0000, 10, 300, now + 0.02, 0, &lines);
  tracker.NoteZpdRebase();
  lines.clear();
  tracker.Summary(now + 1, &lines);
  Expect(Has(lines, "32 answered, 4 guessed visible but hidden, 1 guessed hidden but visible, 1 "
                    "off by more than 4x, 1 rebases"),
         "(f) the summary counts guesses and rebases");
  if (failures) Dump(lines);
  // Many seconds with wrong guesses: at most 30 lines a window.
  lines.clear();
  for (int s = 0; s < 100; ++s) {
    tracker.NoteZpd(1, 5, 0, now + 2 + s, 0, &lines);
    tracker.NoteZpd(1, 5, 0, now + 2.5 + s, 0, &lines);
  }
  Expect(Count(lines, "occlusion queries in the last second") == ht::kZpdLinesPerWindow,
         "(f) at most 30 such lines in 30 s");
}

void Off() {
  ht::Tracker tracker;
  std::vector<std::string> lines;
  tracker.NoteResolve(1, 4096, LuminanceSource(), 1, 1.0, &lines);
  tracker.NoteZpd(1, 1, 0, 3.0, 0, &lines);
  tracker.Configure("", false, "/data/homebrew/PPSA83600/hdr-trace.txt", &lines);
  tracker.Configure("off", true, "/data/homebrew/PPSA83600/hdr-trace.txt", &lines);
  Expect(!tracker.Enabled() && lines.empty(), "(g) without the file (or with \"off\"): no lines");
  tracker.Configure("on", true, "p", &lines);
  tracker.Configure("on", true, "p", &lines);
  Expect(Count(lines, "on from p") == 1, "(g) an unchanged file is not logged again");
  lines.clear();
  tracker.Configure("", false, "p", &lines);
  Expect(!tracker.Enabled() && Count(lines, "HDR trace (fix 158): off (p removed)") == 1,
         "(g) on -> off says so");
  // The rate limit of jump lines: a value alternating 0.1 / 1.0 every 9 frames on many keys.
  ht::Tracker busy;
  busy.Configure("on", true, "p", &lines);
  lines.clear();
  for (int frame = 1; frame <= 400; ++frame) {
    for (uint32_t k = 0; k < 8; ++k) {
      const float value = (frame / 9) % 2 ? 1.0f : 0.1f;
      const std::vector<uint8_t> bytes = Float32Be({value});
      busy.NoteResolve(0x1000 * k, 4, LuminanceSource(), frame, 1.0 + frame * 0.01, &lines);
      busy.NoteBytes(0x1000 * k, 4, bytes.data(), 36, 2, frame, 1.0 + frame * 0.01, 0, &lines);
    }
  }
  Expect(Count(lines, "HDR trace (fix 158): 00:00:00.000 UTC frame") <= ht::kEventLinesPerWindow,
         "(g) at most 80 jump and back lines in 30 s");
}

size_t Find(const std::string& text, const std::string& part, size_t from = 0) {
  return from == std::string::npos ? std::string::npos : text.find(part, from);
}
std::string Slice(const std::string& text, size_t from, size_t to) {
  return from < to && to <= text.size() ? text.substr(from, to - from) : std::string();
}

void Tree() {
  const std::string vcp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/command_processor.cc");
  Expect(!vcp.empty() && !cp.empty(), "(h) the tree's command processors are readable");
  Expect(vcp.find("#include \"xbox360ps5/hdr_trace.hpp\"  // fix 158 diagnostic") <
             vcp.find("#include \"xenia/apu/audio_system.h\""),
         "(h) vulkan_command_processor.cc includes hdr_trace.hpp in its PS5 block");
  const size_t issue = Find(vcp, "bool VulkanCommandProcessor::IssueCopy() {");
  const size_t resolved = Find(vcp, "xbox360ps5::hang_watch::RecordResolve(", issue);
  const size_t poll = Find(vcp, "ht::Poll(XBOX360PS5_DATA_ROOT \"/hdr-trace.txt\", &hdr_lines);", issue);
  const size_t note = Find(vcp, "ht::Global().NoteResolve(written_address, written_length, source,", issue);
  const size_t readback = Find(vcp, "// CPU readback resolve path (if not disabled).", issue);
  Expect(issue != std::string::npos && resolved < poll && poll < note && note < readback,
         "(h) IssueCopy polls the switch and notes the resolve after it succeeded, before readback");
  const std::string block = Slice(vcp, poll, note);
  Expect(block.find("register_file_->Get<reg::RB_COPY_CONTROL>()") != std::string::npos &&
             block.find("reg::RB_COLOR_INFO::rt_register_indices[source.select]") != std::string::npos &&
             block.find("register_file_->Get<reg::RB_SURFACE_INFO>().msaa_samples") != std::string::npos &&
             block.find("copy_dest_info.copy_dest_exp_bias") != std::string::npos &&
             block.find("if (source.select < xenos::kMaxColorRenderTargets)") != std::string::npos,
         "(h) the source comes from the copy and color registers, depth has no color format");
  const size_t copy = Find(vcp, "memory::vastcpy(memory_->TranslatePhysical(written_address),", readback);
  const size_t bytes = Find(vcp, "ht::Global().NoteBytes(written_address, readback_length,", copy);
  const size_t watch = Find(vcp, "// Fix 91: a one-shot read watch on the copy", copy);
  Expect(copy != std::string::npos && copy < bytes && bytes < watch,
         "(h) the copy's bytes are traced right after they go back to guest memory");
  const std::string trace = Slice(vcp, copy, watch);
  Expect(trace.find("#if XE_PLATFORM_PS5") != std::string::npos &&
             trace.find("if (xbox360ps5::hdr_trace::Global().Enabled())") != std::string::npos &&
             trace.find("static_cast<const uint8_t*>(mapped_data)") != std::string::npos,
         "(h) only with the switch on, from the mapped copy, PS5 only");
  const size_t retire = Find(cp, "void CommandProcessor::PumpPendingRetire() {");
  const size_t speculative = Find(cp, "if (front.speculative) {", retire);
  const size_t zpd = Find(cp, "ht::Global().NoteZpd(front.address, front.speculative_delta.z_pass,", retire);
  const size_t not_speculative = Find(cp, "if (!front.speculative) {", retire);
  const size_t rebase = Find(cp, "xbox360ps5::hdr_trace::Global().NoteZpdRebase();  // fix 158", retire);
  Expect(retire != std::string::npos && speculative < zpd && zpd < not_speculative &&
             not_speculative < rebase,
         "(h) PumpPendingRetire compares each fast-mode guess and counts rebases");
  Expect(cp.find("#include \"xbox360ps5/hdr_trace.hpp\"  // fix 158 diagnostic\n#endif") !=
             std::string::npos,
         "(h) command_processor.cc includes hdr_trace.hpp under XE_PLATFORM_PS5");
}

}  // namespace

int main() {
  Switch();
  Decoding();
  Statistics();
  JumpRule();
  Darkening();
  Occlusion();
  Off();
  Tree();
  if (failures) {
    std::printf("check-hdr-trace: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-hdr-trace: all passed\n");
  return 0;
}
