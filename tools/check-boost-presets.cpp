// SPDX-License-Identifier: MIT
// Fix 95 (tools/source-1.0.2/fixes/95-boost-presets.patch): "Optimized video memory"
// (memory_boost) recommended for Modern Warfare 2 (41560817) and Red Dead Redemption (5454082B)
// by the launcher's built-in presets, the player's general setting left off. 1.0.5 perf builds on
// the development console: Modern Warfare 2 26.5 frames a second without it, 30.9 to 33.5 with it
// and fix 87 (20.6 with it and without fix 87); Red Dead Redemption 22 to 28 without, 24 to 30 with.
// The assembled tree:
//  (a) GamePreset's built-in table gives each of the two exactly memory_boost=1 and keeps the
//      earlier rows (Grand Theft Auto IV, Left 4 Dead 2, Battlefield 3) as they were;
//  (b) memory_boost may be set per game and switches the GPU diagnostics' memory paths
//      (gpu_diag::enabled), presets sit below the player's own choices for the game;
//  (c) fix 87 is in the tree: without it Modern Warfare 2 is slower with the preset than without.
// Fix 122 adds The Sims 3 (454108E2) with readback Fast ({"readback", 1}): six rows.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

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

// The built-in table's rows: title ID -> its values as written ("{"key", n}, ...").
std::map<std::string, std::string> PresetRows(const std::string& launcher) {
  std::map<std::string, std::string> rows;
  const size_t fn = launcher.find("GameOverrides GamePreset(const std::string& title_id) {");
  if (fn == std::string::npos) return rows;
  const size_t start = launcher.find("built_in[] = {", fn);
  const size_t end = launcher.find("\n  };", start);
  if (start == std::string::npos || end == std::string::npos) return rows;
  const std::string table = launcher.substr(start, end - start);
  const std::regex row(R"re(\{"([0-9A-Fa-f]{8})", \{(\{[^}]*\}(?:, \{[^}]*\})*)\}\})re");
  for (std::sregex_iterator it(table.begin(), table.end(), row), done; it != done; ++it)
    rows[(*it)[1].str()] = (*it)[2].str();
  return rows;
}

void CheckTree() {
  const std::string launcher = Read("src/launcher.cpp");
  const auto rows = PresetRows(launcher);
  const auto value = [&rows](const char* id) {
    const auto it = rows.find(id);
    return it == rows.end() ? std::string("(none)") : it->second;
  };
  Expect(value("41560817") == R"({"memory_boost", 1})",
         "Modern Warfare 2 is recommended Optimized video memory alone (" + value("41560817") + ")");
  Expect(value("5454082B") == R"({"memory_boost", 1})",
         "Red Dead Redemption is recommended Optimized video memory alone (" + value("5454082B") + ")");
  Expect(value("545407F2") == R"({"fast_locks", 1}, {"memory_boost", 1})", "Grand Theft Auto IV's row as it was");
  Expect(value("454108D4") == R"({"released_memory", 1}, {"readback", 2}, {"occlusion", 2})",
         "Left 4 Dead 2's row as it was");
  Expect(value("45410950") == R"({"memexport", 1})", "Battlefield 3's row as it was");
  Expect(value("454108E2") == R"({"readback", 1})",
         "The Sims 3 is recommended readback Fast alone, fix 122 (" + value("454108E2") + ")");
  Expect(rows.size() == 6, "six built-in rows (" + std::to_string(rows.size()) + ")");

  const std::regex option(R"re(\{"memory_boost", &S::memory_boost, (\d+), W::(\w+), (\d+), (true|false),)re");
  std::smatch m;
  Expect(std::regex_search(launcher, m, option) && m[4] == "true", "memory_boost may be set per game");
  Expect(launcher.find("WithOverrides(WithOverrides(general, GamePreset(title_id)), own)") != std::string::npos,
         "the presets sit below the player's own choices for the game");
  const std::string game_main = Read("src/canary_game_main.cpp");
  Expect(game_main.find("xbox360ps5::gpu_diag::enabled = settings.memory_boost != 0;") != std::string::npos,
         "memory_boost switches the memory paths");

  const std::string frame_cost = Read("include/xbox360ps5/frame_cost.hpp");
  const std::string shared_memory = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.cc");
  Expect(frame_cost.find("inline uint32_t command_chunk_epoch = 1;") != std::string::npos &&
             shared_memory.find("unit_chunk_[unit] == chunk") != std::string::npos,
         "fix 87 is in the tree (Modern Warfare 2 is slower with the preset without it)");
}

}  // namespace

int main() {
  CheckTree();
  if (failures) {
    std::printf("check-boost-presets: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-boost-presets: ok\n");
  return 0;
}
