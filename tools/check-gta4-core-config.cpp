// SPDX-License-Identifier: MIT
// Fix 168b (assets/config/545407F2.config.toml): Grand Theft Auto IV ships with fix 168's
// GPU.resolution_scale_half_pixel_host on, its seams at 2x gone (1.0.6-work-fix173 on the console).
//  (a) the file sets exactly that key, true, in its [GPU] table, and nothing else (the file stays this
//      fix's);
//  (b) the key is the option draw_util.cc defines in section GPU (the tree's root is the current
//      folder), so the core's game config reader applies it;
//  (c) the package takes the repository's assets/config/*.toml (tools/binary-release/build_package.py),
//      where the core reads /app0/assets/config/<TITLE ID>.config.toml.
// Run from the assembled tree's root: check-gta4-core-config <repository root>.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}
std::string Trim(std::string s) {
  const size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

void File(const std::filesystem::path& repo) {
  const std::string text = Read(repo / "assets/config/545407F2.config.toml");
  Expect(!text.empty(), "(a) assets/config/545407F2.config.toml read");
  std::string table;
  std::vector<std::string> keys;
  bool found = false;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    line = Trim(line.substr(0, line.find('#')));
    if (line.empty()) continue;
    if (line.front() == '[') {
      table = line;
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    keys.push_back(table + key);
    found = found || (table == "[GPU]" && key == "resolution_scale_half_pixel_host" && value == "true");
  }
  Expect(found, "(a) [GPU] resolution_scale_half_pixel_host = true");
  Expect(keys.size() == 1, "(a) no other keys (" + std::to_string(keys.size()) + ")");
}

void Option() {
  std::string code;
  for (char c : Read(".deps/xenia-canary/src/xenia/gpu/draw_util.cc"))
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') code += c;
  const size_t option = code.find("DEFINE_bool(resolution_scale_half_pixel_host,false,");
  Expect(option != std::string::npos && code.find("\"GPU\");", option) != std::string::npos,
         "(b) draw_util.cc defines it, off by default, in section GPU");
}

void Package(const std::filesystem::path& repo) {
  const std::string script = Read(repo / "tools/binary-release/build_package.py");
  Expect(script.find("(ROOT / \"assets/config\").glob(\"*.toml\")") != std::string::npos &&
             script.find("PREFIX + \"assets/config/\" + path.name") != std::string::npos,
         "(c) the package takes assets/config/*.toml into assets/config/");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Expect(false, "the repository root as the argument");
  } else {
    File(argv[1]);
    Package(argv[1]);
  }
  if (std::ifstream(".deps/xenia-canary/src/xenia/gpu/draw_util.cc")) {
    Option();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: GTA IV ships with fix 168's half-pixel offset in host pixels\n");
  return 0;
}
