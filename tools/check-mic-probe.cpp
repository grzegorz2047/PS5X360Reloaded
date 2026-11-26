// SPDX-License-Identifier: MIT
// Fix 179 (tools/source-1.0.2/fixes/179-mic-probe.patch; diagnostic, issue 76): the DualSense
// microphone probe. See include/xbox360ps5/mic_probe.hpp.
//
//   check-mic-probe        run from an assembled tree's root
//
//  1. the switch file: "on" as its first word (any case, a byte-order mark, spaces and line ends
//     around it); "off", "one", "o", empty or anything else is off;
//  2. the open attempts: the voice input for the initial user first (16 kHz, 256 samples, 16-bit
//     mono), then the other types, index 1, the system user, and the high-quality open last;
//  3. the statistics: silence is -120 dBFS with every block zero; a full-scale square wave is
//     0 dBFS RMS and peak; a sine is -3 dBFS RMS; blocks a second, the time between blocks (mean,
//     largest, spread), a window that keeps the last block's time; one line every 2 s for 150
//     lines, then every 30 s;
//  4. the tree: the game's main starts the probe after the log is up and stops it before both
//     log shutdowns (exit and title restart); the probe reads mic-probe.txt in the data root,
//     loads libSceAudioIn.sprx at run time and looks its functions up, writes the first 10 s to
//     logs/mic-probe.raw and hands nothing to the virtual Kinect;
//  5. fix 179b: NIDs as the payload SDK computes them, and the lookup routes (by name and NID, the
//     system module service, the full path, the other audio libraries).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/mic_probe.hpp"

namespace mp = xbox360ps5::mic_probe;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

static void CheckSwitch() {
  CHECK(mp::SwitchOn("on"));
  CHECK(mp::SwitchOn("ON\n"));
  CHECK(mp::SwitchOn("  On \r\n"));
  CHECK(mp::SwitchOn("\xEF\xBB\xBFon"));
  CHECK(mp::SwitchOn("on # probe the DualSense microphone"));
  CHECK(!mp::SwitchOn(""));
  CHECK(!mp::SwitchOn("off"));
  CHECK(!mp::SwitchOn("o"));
  CHECK(!mp::SwitchOn("one"));
  CHECK(!mp::SwitchOn("1"));
  CHECK(!mp::SwitchOn("\n\n"));
}

static void CheckAttempts() {
  const auto attempts = mp::Attempts(0x10000001);
  CHECK(attempts.size() >= 5);
  const auto& first = attempts.front();
  CHECK(first.user == 0x10000001 && first.type == 0 && first.index == 0 && first.grain == 256 &&
        first.rate == 16000 && first.param == 0 && !first.high_quality);
  bool system_user = false, other_types = false;
  for (size_t i = 0; i + 1 < attempts.size(); ++i) {
    CHECK(!attempts[i].high_quality);  // the high-quality open is the last resort
    if (attempts[i].user == 0xFF) system_user = true;
    if (attempts[i].type != 0) other_types = true;
  }
  CHECK(system_user && other_types && attempts.back().high_quality);
  const std::string text = mp::Describe(first);
  CHECK(text.find("user 10000001") != std::string::npos &&
        text.find("256 samples at 16000 Hz") != std::string::npos);
}

static void CheckStats() {
  const uint64_t ms = 1000000;
  std::vector<int16_t> block(256);
  {
    // Silence, 125 blocks in 2 s, every 16 ms.
    mp::Window w;
    w.Reset(0);
    for (int i = 0; i < 125; ++i) w.Add(block.data(), block.size(), uint64_t(i + 1) * 16 * ms);
    const auto s = w.Summarize(2000 * ms);
    CHECK(s.blocks == 125 && s.zero_blocks == 125);
    CHECK(s.rms_dbfs == -120 && s.peak == 0 && s.peak_dbfs == -120);
    CHECK(Near(s.blocks_per_second, 62.5, 0.01));
    CHECK(Near(s.interval_mean_ms, 16, 1e-9) && Near(s.interval_max_ms, 16, 1e-9) &&
          Near(s.interval_spread_ms, 0, 1e-6));
    CHECK(w.Due(2000 * ms) && !w.Due(1999 * ms));
  }
  {
    // A full-scale square wave: 0 dBFS (within the int16 asymmetry).
    for (size_t i = 0; i < block.size(); ++i) block[i] = (i & 1) ? -32767 : 32767;
    mp::Window w;
    w.Reset(0);
    w.Add(block.data(), block.size(), 16 * ms);
    const auto s = w.Summarize(16 * ms);
    CHECK(s.zero_blocks == 0 && s.peak == 32767);
    CHECK(Near(s.rms_dbfs, 0, 0.01) && Near(s.peak_dbfs, 0, 0.01));
  }
  {
    // A sine at half scale: RMS -6 - 3 = -9 dBFS, peak -6 dBFS.
    for (size_t i = 0; i < block.size(); ++i)
      block[i] = int16_t(std::lround(16384 * std::sin(2 * 3.14159265358979 * double(i) / 32)));
    mp::Window w;
    w.Reset(0);
    w.Add(block.data(), block.size(), 16 * ms);
    const auto s = w.Summarize(16 * ms);
    CHECK(Near(s.rms_dbfs, -9.03, 0.05) && Near(s.peak_dbfs, -6.02, 0.05));
  }
  {
    // Uneven timing: 16, 16, 48 ms -> mean 26.67, largest 48, spread 15.08 ms; the next window
    // measures its first block against the last one of this window.
    std::fill(block.begin(), block.end(), int16_t(0));
    block[10] = 100;
    mp::Window w;
    w.Reset(0);
    w.Add(block.data(), block.size(), 10 * ms);
    w.Add(block.data(), block.size(), 26 * ms);
    w.Add(block.data(), block.size(), 42 * ms);
    w.Add(block.data(), block.size(), 90 * ms);
    const auto s = w.Summarize(100 * ms);
    CHECK(s.blocks == 4 && s.zero_blocks == 0 && s.peak == 100);
    CHECK(Near(s.interval_mean_ms, 80.0 / 3, 1e-6) && Near(s.interval_max_ms, 48, 1e-9) &&
          Near(s.interval_spread_ms, std::sqrt(2048.0 / 9), 0.01));
    w.Next(100 * ms);
    w.Add(block.data(), block.size(), 106 * ms);
    const auto t = w.Summarize(110 * ms);
    CHECK(t.blocks == 1 && Near(t.interval_mean_ms, 16, 1e-9) && Near(t.window_ms, 10, 1e-9));
  }
  CHECK(mp::WindowFor(0) == mp::kWindowNs && mp::WindowFor(149) == mp::kWindowNs &&
        mp::WindowFor(150) == mp::kSlowWindowNs);
  mp::Summary s;
  s.window_ms = 2000;
  s.blocks = 125;
  s.zero_blocks = 25;
  const std::string line = mp::Line(s, 0, true);
  CHECK(line.find("125 blocks") != std::string::npos && line.find("= 20%") != std::string::npos &&
        line.find("mute off") != std::string::npos);
  CHECK(mp::Line(s, 1, true).find("mute on (state 00000001)") != std::string::npos);
  CHECK(mp::Line(s, 0, false).find("mute unknown") != std::string::npos);
  CHECK(mp::kRawBytes == 320000);
}

// Fix 179b: NIDs as the payload SDK computes them (known pairs from the system's own libraries).
static void CheckNid() {
#ifdef XBOX360PS5_MIC_PROBE_NID
  CHECK(mp::Nid("sceKernelDlsym") == "LwG8g3niqwA");
  CHECK(mp::Nid("sceKernelLoadStartModule") == "wzvqT4UqKX8");
  CHECK(mp::Nid("sceAudioOutOpen") == "ekNvsT22rsY");
  CHECK(mp::Nid("sceAudioOutOutput") == "QOQtbeDqsT4");
  CHECK(mp::Nid("sceAudioInOpen") == "5NE8Sjc7VC8");
  CHECK(mp::Nid("sceAudioInInput") == "LozEOU8+anM");
  CHECK(mp::Nid("sceAudioInHqOpen") == "nya-R5gDYhM");  // the "-" of the system's alphabet
  CHECK(mp::Nid("sceSysmoduleLoadModuleInternal") == "39iV5E1HoCk");
  CHECK(mp::Nid("").size() == 11);
  CHECK(mp::kAudioInModule == 0x80000002 && mp::Functions().size() == 5);
#endif
}

static std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  std::string out;
  for (char c : s.str())
    if (c != '\r') out += c;
  return out;
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

static void CheckTree() {
  const std::string main_cpp = ReadText("src/canary_game_main.cpp");
  const std::string probe = ReadText("include/xbox360ps5/mic_probe_ps5.hpp");
  const std::string pure = ReadText("include/xbox360ps5/mic_probe.hpp");
  if (main_cpp.empty() || probe.empty()) {
    std::printf("FAIL: no assembled tree here (src/canary_game_main.cpp, mic_probe_ps5.hpp)\n");
    ++failures;
    return;
  }
  CHECK(Has(main_cpp, "#include \"xbox360ps5/mic_probe_ps5.hpp\""));
  const size_t log_up = main_cpp.find("xe::AddLogSink(std::move(owned_game_log));");
  const size_t started = main_cpp.find("xbox360ps5::mic_probe::Start(XBOX360PS5_DATA_ROOT)");
  CHECK(log_up != std::string::npos && started != std::string::npos && log_up < started);
  // Stopped before each log shutdown that follows the start: the title restart and the exit.
  const size_t restart = main_cpp.find("Stage(\"BOOT restarting the title\");", started);
  CHECK(restart != std::string::npos &&
        main_cpp.find("xbox360ps5::mic_probe::Stop();", restart) <
            main_cpp.find("xe::ShutdownLogging();", restart));
  const size_t exit_at = main_cpp.find("XELOGI(\"ENGINE EXIT {}\", result);");
  CHECK(exit_at != std::string::npos &&
        main_cpp.find("xbox360ps5::mic_probe::Stop();", exit_at) <
            main_cpp.find("xe::ShutdownLogging();", exit_at));
  CHECK(Has(probe, "data_root / \"mic-probe.txt\""));
  CHECK(Has(probe, "if (!SwitchOn(text)) return"));
  CHECK(Has(probe, "\"libSceAudioIn.sprx\""));
  for (const char* name : {"\"sceAudioInOpen\"", "\"sceAudioInInput\"", "\"sceAudioInClose\"",
                           "\"sceAudioInGetSilentState\"", "\"sceAudioInHqOpen\""})
    CHECK(Has(probe, name) || Has(pure, name));  // fix 179b lists them in mic_probe.hpp
  CHECK(Has(probe, "data_root / \"logs\" / \"mic-probe.raw\""));
  CHECK(!Has(probe, "VirtualMicSubmit") && !Has(probe, "virtual_mic"));
#ifdef XBOX360PS5_MIC_PROBE_NID
  // Fix 179b: by name, then by NID; the system module service; the path; other audio libraries.
  CHECK(Has(probe, "Resolve(handle, false, route, lib) || Resolve(handle, true, route, lib)"));
  CHECK(Has(probe, "\"sceSysmoduleLoadModuleInternal\"") && Has(probe, "(kAudioInModule)"));
  CHECK(Has(probe, "\"/system/common/lib/libSceAudioIn.sprx\""));
  CHECK(Has(probe, "\"libSceAudioOut.sprx\", \"libSceAudioSystem.sprx\""));
  CHECK(Has(probe, "sceKernelLoadStartModule(name, 0, nullptr, 0, nullptr, &started)"));
  CHECK(Has(probe, "\"sceAudioInInit\"") &&
        probe.find("sceAudioInInit() ->") < probe.find("sceUserServiceGetInitialUser(&user)"));
#endif
}

int main() {
  CheckSwitch();
  CheckAttempts();
  CheckStats();
  CheckNid();
  CheckTree();
  if (failures) {
    std::printf("check-mic-probe: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-mic-probe: ok\n");
  return 0;
}
