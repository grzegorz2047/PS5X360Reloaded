// SPDX-License-Identifier: MIT
// Fix 166 (tools/source-1.0.2/fixes/166-game-configs.patch): the per-game core configs
// (<TITLE ID>.config.toml, Xenia's game config format) from the title's assets/game-configs and the
// user's <data>/game-configs (fix 165) are read again. The port's prepare.py had the core call
// xbox360ps5::LoadGameConfigs where it loads a game's config; the 1.0.2 source rebuild kept only the
// core's own config::LoadGameConfig (<config folder>/config, /app0/assets/config), so a file in the
// data folder did nothing (GTA IV's test C on 1.0.6_candidate_9: no "Game config:" line).
//  (a) emulator.cc includes title_switch.hpp and calls LoadGameConfigs(title_id) right after the
//      core's LoadGameConfig and before the game config callbacks (PostGameConfigLoad);
//  (b) title_switch.cpp's LoadGameConfigs reads assets/game-configs first and <data>/game-configs
//      (user_storage::Root()) after it, so the user's file wins, logs "Game config: <file>" and reads
//      it with config::ReadGameConfig;
//  (c) the key the 1.0.7 test uses, GPU.draw_resolution_scaled_texture_offsets, is a core option of
//      section GPU (a config's [GPU] table sets it).
// Run from the assembled tree's root.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Squeezed(const char* path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string out;
  for (char c : buffer.str())
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
  return out;
}

void Emulator() {
  const std::string code = Squeezed(".deps/xenia-canary/src/xenia/emulator.cc");
  Expect(!code.empty(), "(a) emulator.cc read");
  Expect(code.find("#include\"xbox360ps5/title_switch.hpp\"") != std::string::npos,
         "(a) emulator.cc includes title_switch.hpp");
  const size_t core = code.find("config::LoadGameConfig(title_id);");
  const size_t title = code.find("xbox360ps5::LoadGameConfigs(title_id);", core);
  const size_t callbacks = code.find("->PostGameConfigLoad();", core);
  Expect(core != std::string::npos && title != std::string::npos && callbacks != std::string::npos && core < title &&
             title < callbacks,
         "(a) the title's configs after the core's own, before the game config callbacks");
}

void TitleSwitch() {
  const std::string code = Squeezed("src/title_switch.cpp");
  const size_t fn = code.find("voidLoadGameConfigs(conststd::string&title_id){");
  const size_t shipped = code.find("\"/app0/assets/game-configs\"", fn);
  const size_t user = code.find("xbox360ps5::user_storage::Root()/\"game-configs\"", fn);
  Expect(fn != std::string::npos && shipped != std::string::npos && user != std::string::npos && shipped < user,
         "(b) assets/game-configs first, then <data>/game-configs (the user's file wins)");
  Expect(code.find("XELOGW(\"Gameconfig:{}\",file.string());", fn) != std::string::npos &&
             code.find("config::ReadGameConfig(file);", fn) != std::string::npos &&
             code.find("(title_id+\".config.toml\")", fn) != std::string::npos,
         "(b) <TITLE ID>.config.toml read with ReadGameConfig and logged");
}

void Key() {
  const std::string code = Squeezed(".deps/xenia-canary/src/xenia/gpu/render_target_cache.cc");
  const size_t key = code.find("DEFINE_bool(draw_resolution_scaled_texture_offsets,true,");
  const size_t section = code.find("\"GPU\");", key);
  Expect(key != std::string::npos && section != std::string::npos && section - key < 400,
         "(c) GPU.draw_resolution_scaled_texture_offsets is a core option of section GPU");
}
}  // namespace

int main() {
  Emulator();
  TitleSwitch();
  Key();
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: the per-game core configs of assets/game-configs and the data folder are read\n");
  return 0;
}
