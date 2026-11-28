// SPDX-License-Identifier: MIT
// Fix 193 (assets/config/4D53090F.config.toml): Nike+ Kinect Training ships without fix 178's
// virtual Kinect microphone array. With the array the loading screen after the intro never ended
// (1.0.7_candidate_1: a black screen, the main thread polling the loading screen's state); with
// kinect_microphone = false from the data folder the game went on to the Kinect sign-in, the
// Xbox Fitness body profile (fix 171) and the workouts on the console.
//  (a) the file sets exactly that key, false, in its [Kinect] table, and nothing else (the file
//      stays this fix's);
//  (b) the key is the option xboxkrnl_audio.cc defines in section Kinect (fix 178; the tree's root
//      is the current folder), and VirtualMicOn reads it when the game asks for the array, so the
//      core's game config reader applies it in time;
//  (c) the package takes the repository's assets/config/*.toml (tools/binary-release/build_package.py),
//      where the core reads /app0/assets/config/<TITLE ID>.config.toml.
// Run from the assembled tree's root: check-nike-core-config <repository root>.
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
std::string Squeezed(const std::string& text) {
  std::string code;
  for (char c : text)
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') code += c;
  return code;
}

void File(const std::filesystem::path& repo) {
  const std::string text = Read(repo / "assets/config/4D53090F.config.toml");
  Expect(!text.empty(), "(a) assets/config/4D53090F.config.toml read");
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
    found = found || (table == "[Kinect]" && key == "kinect_microphone" && value == "false");
  }
  Expect(found, "(a) [Kinect] kinect_microphone = false");
  Expect(keys.size() == 1, "(a) no other keys (" + std::to_string(keys.size()) + ")");
}

void Option() {
  const std::string code =
      Squeezed(Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc"));
  const size_t option = code.find("DEFINE_bool(kinect_microphone,true,");
  Expect(option != std::string::npos && code.find("\"Kinect\");", option) != std::string::npos,
         "(b) xboxkrnl_audio.cc defines it, on by default, in section Kinect");
  const size_t fn = code.find("boolVirtualMicOn(){");
  Expect(fn != std::string::npos && code.find("cvars::kinect_microphone", fn) != std::string::npos,
         "(b) VirtualMicOn reads it when the game asks for the array");
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
  if (std::ifstream(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc")) {
    Option();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: Nike+ Kinect Training ships without the virtual Kinect microphone\n");
  return 0;
}
