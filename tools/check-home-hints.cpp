// SPDX-License-Identifier: MIT
// Fix 40 (reconstructed files, no patch): what the official 1.0.2 eboot has where the rebuild had
// stand-ins. reconstructed/home_navigation.hpp's kinect_ui::HintRgb gives the button hints the
// official colours (cross 0x55a329, circle 0xd84639, square 0x287ac3, triangle 0xe3b52d: Canvas::Hint
// at 0x210f86-0x211349 and NxeHint at 0x211f82-0x211fb0), HomeNavigation stays empty and
// launcher.cpp's two callers keep their own colour for any other button (kMuted, 0x55604d).
// reconstructed/ps5x360_reconstructed.cpp defines cvars::ps5_frame_interpolation as the official
// registers it (0x1230a6-0x1230be: default false, the description at 0x1dc1a53, category "Vulkan"),
// and port/presenter_sidecar.hpp still sets it from framegen.conf's first line.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "home_navigation.hpp"          // the assembled tree's reconstructed/ (-Ireconstructed)
#include "../port/framegen_config.hpp"  // the assembled tree's port/ (run from <tree>/tools)

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using kinect_ui::FaceButton;
using kinect_ui::HintRgb;

constexpr const char* kDescription =
    "Independent Vulkan midpoint interpolation prototype. Requires the presenter sidecar and "
    "measured display pacing; disabled by default.";

std::string ReadTree(const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file && "run from the assembled tree's root");
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

// Every run of white space as one space, so a statement can be found however it is wrapped.
std::string Squash(const std::string& text) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

size_t Count(const std::string& text, const std::string& what) {
  size_t count = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++count;
  return count;
}

void Colours() {
  assert(HintRgb(FaceButton::cross) == 0x55a329);
  assert(HintRgb(FaceButton::circle) == 0xd84639);
  assert(HintRgb(FaceButton::square) == 0x287ac3);
  assert(HintRgb(FaceButton::triangle) == 0xe3b52d);
  // None of the colours the rebuild chose before fix 40.
  for (const FaceButton button :
       {FaceButton::cross, FaceButton::circle, FaceButton::square, FaceButton::triangle}) {
    const uint32_t rgb = HintRgb(button);
    assert(rgb != 0x7fa6e8 && rgb != 0xe8636b && rgb != 0xd88ccf && rgb != 0x3fbf9b);
    assert(rgb <= 0xffffff);
  }
  static_assert(std::is_empty_v<kinect_ui::HomeNavigation>, "HomeNavigation stays empty");
  std::printf("  HintRgb: cross 55a329, circle d84639, square 287ac3, triangle e3b52d; "
              "HomeNavigation empty\n");
}

// The conditional a launcher.cpp hint picks its colour with: `declaration`, then each button with
// its FaceButton in the order given, then the caller's own colour for any other button.
std::string Picker(const char* declaration,
                   std::initializer_list<std::pair<char, const char*>> buttons,
                   const char* fallback) {
  std::string text = declaration;
  for (const auto& [button, face] : buttons) {
    text += std::string(" button == '") + button + "' ? kinect_ui::HintRgb(kinect_ui::FaceButton::" +
            face + ") :";
  }
  return text + " " + fallback + ";";
}

void Callers() {
  const std::string launcher = Squash(ReadTree("src/launcher.cpp"));
  assert(launcher.find("#include \"home_navigation.hpp\"") != std::string::npos);
  assert(launcher.find("kinect_ui::HomeNavigation home_navigation;") != std::string::npos);
  // Canvas::Hint (the menus): kMuted for any other button.
  const std::string canvas = Picker(
      "const uint32_t face_rgb =",
      {{'x', "cross"}, {'o', "circle"}, {'s', "square"}, {'t', "triangle"}}, "kMuted");
  // NxeHint (the NXE home), in the official's order x, t, s, o: 0x55604d for any other button,
  // as the official's last branch (0x211fb0).
  const std::string nxe = Picker(
      "const uint32_t ink =",
      {{'x', "cross"}, {'t', "triangle"}, {'s', "square"}, {'o', "circle"}}, "0x55604d");
  for (const std::string* picker : {&canvas, &nxe}) {
    if (Count(launcher, *picker) != 1) {
      std::fprintf(stderr, "  launcher.cpp lacks (once): %s\n", picker->c_str());
      assert(false);
    }
  }
  // Fix 109: Canvas::Raised (the "Remove DLC?" window's buttons) takes its pad colour from
  // HintRgb too, once, for the button it was given (a build without fix 109 has no Raised).
  const int raised = Count(launcher, "list->AddCircleFilled(centre, 16 * scale, Color(kinect_ui::HintRgb(pad)), 32);");
  assert(raised == Count(launcher, "float Raised(char button, const std::string& label,") && raised <= 1);
  assert(Count(launcher, "kinect_ui::HintRgb(") == 8 + raised);  // no other caller
  std::printf("  launcher.cpp: Canvas::Hint and NxeHint take the four colours from HintRgb, "
              "their own for other buttons; Canvas::Raised (fix 109) its button's\n");
}

// The arguments of the macro call whose '(' is at `open`. An argument made only of string
// literals is given as their concatenated value (`literal`), any other with its white space and
// comments dropped.
struct Argument {
  std::string text;
  bool literal = true;
};

std::vector<Argument> MacroArguments(const std::string& source, size_t open) {
  assert(open < source.size() && source[open] == '(');
  std::vector<Argument> arguments;
  Argument current;
  int depth = 0;
  for (size_t i = open + 1; i < source.size(); ++i) {
    const char c = source[i];
    if (c == '"') {
      for (++i; i < source.size() && source[i] != '"'; ++i) {
        assert(source[i] != '\n' && "a line break inside a string literal");
        if (source[i] != '\\') {
          current.text += source[i];
          continue;
        }
        ++i;
        assert(i < source.size());
        const char escaped = source[i];
        current.text += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
      }
      assert(i < source.size() && "an unterminated string literal");
      continue;
    }
    if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
      i = source.find('\n', i);
      assert(i != std::string::npos);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    if (depth == 0 && (c == ',' || c == ')')) {
      arguments.push_back(current);
      current = Argument();
      if (c == ')') return arguments;
      continue;
    }
    if (c == '(') ++depth;
    if (c == ')') --depth;
    current.literal = false;
    current.text += c;
  }
  assert(false && "no closing parenthesis");
  return arguments;
}

void ParserSelfTest() {
  const std::string sample =
      "DEFINE_bool(name, f(1, 2),\n  \"a \\\"b\\\" \" // note, (\n  \"c\", \"Cat\");";
  const auto arguments = MacroArguments(sample, sample.find('('));
  assert(arguments.size() == 4);
  assert(!arguments[0].literal && arguments[0].text == "name");
  assert(!arguments[1].literal && arguments[1].text == "f(1,2)");
  assert(arguments[2].literal && arguments[2].text == "a \"b\" c");
  assert(arguments[3].literal && arguments[3].text == "Cat");
}

void FrameInterpolationCvar() {
  ParserSelfTest();
  const std::string source = ReadTree("reconstructed/ps5x360_reconstructed.cpp");
  assert(source.find("#include \"xenia/base/cvar.h\"") != std::string::npos);
  const std::string head = "DEFINE_bool(ps5_frame_interpolation";
  const size_t at = source.find(head);
  assert(at != std::string::npos);
  assert(source.find(head, at + 1) == std::string::npos);  // defined once
  const auto arguments = MacroArguments(source, at + std::strlen("DEFINE_bool"));
  assert(arguments.size() == 4);
  assert(!arguments[0].literal && arguments[0].text == "ps5_frame_interpolation");
  assert(!arguments[1].literal && arguments[1].text == "false");
  if (!arguments[2].literal || arguments[2].text != kDescription) {
    std::fprintf(stderr, "  description: %s\n", arguments[2].text.c_str());
    assert(false);
  }
  assert(arguments[3].literal && arguments[3].text == "Vulkan");
  std::printf("  cvars::ps5_frame_interpolation: default false, the official description, "
              "category Vulkan\n");

  // Still set from framegen.conf's first line alone, as before fix 40.
  const std::string sidecar = Squash(ReadTree("port/presenter_sidecar.hpp"));
  assert(Count(sidecar, "cvars::ps5_frame_interpolation = valid && config.enabled;") == 1);
  namespace fg = ps5x360::framegen;
  const auto value = [](const std::string& file) {
    const fg::Config config = fg::ParseConfig(file);
    return config.valid && config.enabled;
  };
  assert(value(std::string(fg::kSwitchOn) + "\n"));
  assert(value("ps5_frame_interpolation=true\r\nps5_frame_multiplier=3\n"));
  assert(!value(std::string(fg::kSwitchOff) + "\n"));
  assert(!value(""));
  assert(!value("ps5_frame_interpolation=yes\n"));
  std::printf("  presenter_sidecar.hpp: on only for framegen.conf's ps5_frame_interpolation=true\n");
}
}  // namespace

int main() {
  Colours();
  Callers();
  FrameInterpolationCvar();
  std::puts("PASS: official button-hint colours and frame interpolation cvar (fix 40)");
}
