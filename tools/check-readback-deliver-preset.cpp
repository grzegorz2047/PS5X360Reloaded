// SPDX-License-Identifier: MIT
// Fix 184: The Sims 3 (454108E2) gets fix 142i's delivery of fast readback copies and fix 157's
// reader wait by a per-game preset (xenia/gpu/vulkan/readback_deliver_preset.h), with the values
// of its console A/B (1.0.6-work-fix157): on, wait-below 0, wait-after-early off, reader-wait
// on, 20 ms. Every other game keeps the settings' values (both switches off by default).
//  (a) the preset's values for The Sims 3, and no preset for Tekken Tag Tournament 2, GTA V
//      or other titles;
//  (b) the preset applies only while both switches are off in the settings: a player who set
//      either keeps all of the settings' values;
//  (c) the tree: fix 142i's Refresh reads the settings, then the preset for the running title,
//      then readback-deliver.txt, whose words set the same values after the preset (the file
//      overrides it); EndSubmission hands Refresh the running title; the switches still default
//      to off; the settings line names the preset.
// Runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("xenia/gpu/vulkan/readback_deliver_preset.h")
#include "xenia/gpu/vulkan/readback_deliver_preset.h"
#define HAVE_FIX_184 1
#else
#define HAVE_FIX_184 0
#endif

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
std::string Body(const std::string& text, const std::string& signature) {
  const size_t at = text.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t end = text.find("\n}\n", at);
  return end == std::string::npos ? std::string() : text.substr(at, end - at);
}
// The offset of `part` in `text` at or after `from`, or npos (reported).
size_t At(const std::string& text, const std::string& part, size_t from = 0) {
  const size_t at = text.find(part, from);
  if (at == std::string::npos) std::printf("  (not found: %s)\n", part.substr(0, 100).c_str());
  return at;
}

#if HAVE_FIX_184
namespace rp = xbox360ps5::readback_deliver_preset;

void CheckPreset() {
  // (a)
  rp::Values v;
  Expect(rp::For(0x454108E2, &v), "The Sims 3 has a preset");
  Expect(v.on, "The Sims 3: fix 142i on");
  Expect(v.wait_below_kib == 0, "The Sims 3: wait-below 0");
  Expect(!v.wait_after_early, "The Sims 3: wait-after-early off");
  Expect(v.reader_wait, "The Sims 3: reader-wait on");
  Expect(v.reader_wait_ms == 20, "The Sims 3: 20 ms at most");
  for (uint32_t other : {0x4E4D0859u /* Tekken Tag Tournament 2 */, 0x545408A7u /* GTA V */,
                         0x454109CBu, 0x45410950u, 0u, 0xFFFFFFFFu}) {
    rp::Values w;
    Expect(!rp::For(other, &w), "no preset for another title");
    Expect(!w.on && w.wait_below_kib == 64 && w.wait_after_early && !w.reader_wait &&
               w.reader_wait_ms == 20,
           "another title's values stay the settings' defaults");
    Expect(std::string(rp::From(other)) == "the settings", "another title's line says the settings");
  }
  Expect(std::string(rp::From(0x454108E2)).find("454108E2 (fix 184)") != std::string::npos,
         "the settings line names The Sims 3's preset");
  // (b)
  rp::Values p;
  Expect(rp::Applies(0x454108E2, false, false, &p) && p.on && p.reader_wait,
         "the preset applies with both switches off in the settings");
  rp::Values q;
  Expect(!rp::Applies(0x454108E2, true, false, &q) && !q.on,
         "fix 142i on in the settings: the settings' values stand");
  Expect(!rp::Applies(0x454108E2, false, true, &q), "fix 157 on in the settings: the settings stand");
  Expect(!rp::Applies(0x4E4D0859, false, false, &q) && !q.on && !q.reader_wait,
         "Tekken Tag Tournament 2: no preset");
}
#endif

void CheckTree() {
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  Expect(!cp.empty(), "the tree is there (run from the assembled tree's root)");
  Expect(cp.find("#include \"xenia/gpu/vulkan/readback_deliver_preset.h\"") != std::string::npos,
         "the command processor includes the preset");
  // (c) the order in Refresh: settings, preset, file.
  const std::string refresh = Body(cp, "\nvoid Refresh() {\n");
  Expect(!refresh.empty(), "fix 142i's Refresh is there");
  const size_t settings_on = At(refresh, "bool next = cvars::readback_deliver_when_done;");
  const size_t settings_reader = At(refresh, "bool next_reader = cvars::readback_reader_wait;");
  const size_t preset = At(refresh, "rp::Applies(title_id, next, next_reader, &preset)");
  const size_t file = At(refresh, "std::fopen(path, \"rb\")");
  Expect(settings_on != std::string::npos && settings_reader != std::string::npos &&
             preset != std::string::npos && file != std::string::npos &&
             settings_on < preset && settings_reader < preset && preset < file,
         "Refresh reads the settings, then the preset, then readback-deliver.txt");
  if (preset != std::string::npos && file != std::string::npos) {
    const std::string block = refresh.substr(preset, file - preset);
    for (const char* set : {"next = preset.on;", "next_below = preset.wait_below_kib * 1024;",
                            "next_after_early = preset.wait_after_early;",
                            "next_reader = preset.reader_wait;", "next_reader_ms = std::clamp",
                            "from = rp::From(title_id);"}) {
      Expect(block.find(set) != std::string::npos, std::string("the preset sets ") + set);
    }
    // The file's words set the same values after the preset: the file overrides it.
    const std::string words = refresh.substr(file);
    for (const char* set : {"next = w == \"1\" || w == \"on\";", "next_below = uint32_t(",
                            "next_after_early = value == \"on\";", "next_reader = value == \"on\";",
                            "next_reader_ms =", "from = path;"}) {
      Expect(words.find(set) != std::string::npos,
             std::string("readback-deliver.txt's words set (after the preset) ") + set);
    }
  }
  // EndSubmission hands Refresh the running title.
  const std::string end_submission = Body(cp, "bool VulkanCommandProcessor::EndSubmission(");
  const size_t title = At(end_submission, "fix142i::title_id = kernel_state_ ? kernel_state_->title_id() : 0;");
  const size_t call = At(end_submission, "fix142i::Refresh();");
  Expect(title != std::string::npos && call != std::string::npos && title < call,
         "EndSubmission sets the running title before Refresh");
  // The switches' defaults: off, so every game without a preset behaves as without fix 184.
  Expect(cp.find("DEFINE_bool(readback_deliver_when_done, false,") != std::string::npos,
         "fix 142i is off by default");
  Expect(cp.find("DEFINE_bool(readback_reader_wait, false,") != std::string::npos,
         "fix 157 is off by default");
  Expect(cp.find("DEFINE_uint32(readback_deliver_wait_below_kib, 64,") != std::string::npos &&
             cp.find("DEFINE_bool(readback_deliver_wait_after_early, true,") != std::string::npos &&
             cp.find("DEFINE_uint32(readback_reader_wait_ms, 20,") != std::string::npos,
         "the other settings keep their defaults");
}

}  // namespace

int main() {
#if HAVE_FIX_184
  CheckPreset();
#else
  Expect(false, "fix 184's xenia/gpu/vulkan/readback_deliver_preset.h is in the tree");
#endif
  CheckTree();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("ok: The Sims 3's readback delivery preset (fix 184)\n");
  return 0;
}
