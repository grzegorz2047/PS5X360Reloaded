// SPDX-License-Identifier: MIT
// Fix 114b (tools/source-1.0.2/fixes/114b-osk-modes.patch), after the console run of fix 114 in The
// Sims 3: the game asks for its saved game's name with flags 0x20000044 (VKBD_LATIN_GAMERTAG |
// VKBD_LATIN_ALPHABET, the default text selected), the player typed "AŚ..." from the accents
// page, the game refused the name ("letters (a-z), numbers (0-9) and spaces") and drew the
// letter as a box; and the keyboard's legend and counter read "Usu?" / "Przesu? kursor": they took
// the core's monospace font, loaded at the same size, which has no Polish letters.
//  (a) AllowedBy: what each limiting mode lets through, several modes together, no mode = all;
//  (b) the editor in The Sims 3's mode: only the letters page, the other keys dim and typing
//      nothing, a refused character not counted as the length limit, Shift still there; digits
//      only: no Shift, no Space, no letters; e-mail: the symbols page for "@"; the full keyboard
//      (and Russian) unchanged;
//  (c) KeyboardFont: the interface's family (FontDefault) at the nearest size, never another family
//      loaded at that size, the default font when the nearest size lacks a letter of the text;
//  (d) the real view drawn with ImGui in each mode, everything on the screen; the tree's view
//      draws every text through KeyboardFont.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#include "xenia/ui/controller_keyboard.h"
#include "xenia/ui/controller_keyboard_view.h"

#include "third_party/imgui/imgui.cpp"
#include "third_party/imgui/imgui_draw.cpp"
#include "third_party/imgui/imgui_tables.cpp"
#include "third_party/imgui/imgui_widgets.cpp"
#include "xenia/base/string.cc"
#include "xenia/ui/controller_keyboard_view.cc"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace kb = xe::ui::controller_keyboard;
int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
std::string U8(char16_t c) { return xe::to_utf8(std::u16string(1, c)); }

constexpr uint32_t kSims3 = 0x20000044;  // the flags The Sims 3's "Edit Text" passes

void CheckAllowed() {
  const uint32_t sims = kSims3 & kb::flags::kLimiting;
  Expect(sims == (kb::flags::kGamertag | kb::flags::kAlphabet), "0x44 is gamertag | alphabet");
  for (char16_t c : std::u16string(u"azAZ09 "))
    Expect(kb::AllowedBy(sims, c), "The Sims 3's mode allows " + U8(c));
  for (char16_t c : std::u16string(u"Śàßж-.,'@!_"))
    Expect(!kb::AllowedBy(sims, c), "The Sims 3's mode refuses " + U8(c));
  Expect(kb::AllowedBy(kb::flags::kNumeric, u'7') && !kb::AllowedBy(kb::flags::kNumeric, u'a') &&
             !kb::AllowedBy(kb::flags::kNumeric, u' '),
         "numeric: digits only");
  Expect(kb::AllowedBy(kb::flags::kEmail, u'@') && kb::AllowedBy(kb::flags::kEmail, u'.') &&
             kb::AllowedBy(kb::flags::kEmail, u'_') && !kb::AllowedBy(kb::flags::kEmail, u' ') &&
             !kb::AllowedBy(kb::flags::kEmail, 0xe9),
         "e-mail: letters, digits and @ . - _ +");
  Expect(kb::AllowedBy(kb::flags::kPhone, u'+') && kb::AllowedBy(kb::flags::kPhone, u'#') &&
             !kb::AllowedBy(kb::flags::kPhone, u'a'),
         "phone: digits and + - ( ) * #");
  Expect(kb::AllowedBy(kb::flags::kIpAddress, u'.') && !kb::AllowedBy(kb::flags::kIpAddress, u'-'),
         "IP address: digits and dots");
  Expect(kb::AllowedBy(kb::flags::kAlphabet, u'q') && !kb::AllowedBy(kb::flags::kAlphabet, u'5'),
         "alphabet alone: no digits");
  Expect(kb::AllowedBy(kb::flags::kNumeric | kb::flags::kIpAddress, u'.'), "two modes: either allows");
  for (uint32_t full : {0u, 0x1u, 0x10000u, 0x80u, 0x1000u, 0x8000u, 0x20000000u})
    Expect(kb::AllowedBy(full, 0x15a) && kb::AllowedBy(full, u'!') && kb::AllowedBy(full, 0x436),
           "a full mode allows everything");
}

void CheckEditor() {
  {
    kb::Editor editor(u"", 17, kb::OptionsFromFlags(kSims3));
    Expect(editor.limit() == 0x44, "the editor keeps the game's mode");
    Expect(editor.page() == kb::Page::letters && !editor.CanChangePage(), "only the letters page");
    editor.NextPage();
    editor.Press(kb::pad::kR2);
    Expect(editor.page() == kb::Page::letters, "R2 stays on the letters page");
    Expect(editor.Enabled(1, 0) && editor.Enabled(0, 9) && !editor.Enabled(2, 9) && !editor.Enabled(3, 7) &&
               !editor.Enabled(3, 9),
           "q and 0 are keys; ' , and - are dim");
    editor.Press(kb::pad::kL2);
    Expect(editor.CanShift() && editor.shift() == kb::Shift::once, "Shift works");
    editor.Press(kb::pad::kDown);  // to q
    editor.Press(kb::pad::kCross);
    Expect(editor.text() == u"Q" && editor.shift() == kb::Shift::off, "types a capital Q");
    const uint32_t refused = editor.refused();
    Expect(!editor.Insert(u"Ś") && editor.text() == u"Q", "Ś cannot be typed");
    Expect(editor.refused() == refused, "a refused letter is not the length warning");
    editor.Press(kb::pad::kTriangle);
    Expect(editor.text() == u"Q ", "a space is allowed");
    for (int i = 0; i < 2; ++i) editor.Press(kb::pad::kDown);  // to z (row 3)
    for (int i = 0; i < 9; ++i) editor.Press(kb::pad::kRight);  // to - (row 3, column 9)
    editor.Press(kb::pad::kCross);
    Expect(editor.text() == u"Q ", "a dim key types nothing");
  }
  {
    kb::Editor editor(u"Player", 17, kb::OptionsFromFlags(kSims3));
    Expect(!editor.Insert(u"é") && editor.highlighted() && editor.text() == u"Player",
           "a refused letter keeps the selected default text");
    kb::Editor accented(u"Café", 17, kb::OptionsFromFlags(kSims3));
    Expect(accented.text() == u"Café", "the game's own default text is kept as it is");
  }
  {
    kb::Editor digits(u"", 9, kb::OptionsFromFlags(kb::flags::kNumeric));
    Expect(!digits.CanShift() && !digits.CanChangePage(), "numeric: no Shift, no other page");
    digits.Press(kb::pad::kL2);
    Expect(digits.shift() == kb::Shift::off, "L2 does nothing");
    digits.Press(kb::pad::kTriangle);
    Expect(digits.text().empty(), "no space");
    digits.Press(kb::pad::kCross);
    Expect(digits.text() == u"1", "digits type");
    Expect(!digits.Enabled(1, 0), "letters are dim");
  }
  {
    kb::Editor mail(u"", 33, kb::OptionsFromFlags(kb::flags::kEmail));
    Expect(mail.Usable(kb::Page::symbols) && !mail.Usable(kb::Page::accents) && !mail.Usable(kb::Page::cyrillic),
           "e-mail: the symbols page (for @), no accents or Russian");
    mail.Press(kb::pad::kR2);
    Expect(mail.page() == kb::Page::symbols, "R2 goes straight to the symbols page");
    Expect(mail.Enabled(0, 1) && !mail.Enabled(0, 0), "@ is a key there, ! is dim");
  }
  {
    kb::Editor full(u"", 17, kb::OptionsFromFlags(0x10000));
    Expect(full.limit() == 0 && full.CanChangePage(), "a full mode: every page");
    for (int page = 0; page < kb::kPages; ++page) Expect(full.Usable(kb::Page(page)), "every page usable");
    Expect(full.Insert(u"Ś"), "and every letter");
    kb::Editor russian(u"", 17, kb::OptionsFromFlags(kb::flags::kRussian));
    Expect(russian.page() == kb::Page::cyrillic, "VKBD_RUSSIAN_FULL still opens on the Russian page");
    kb::Editor limited_russian(u"", 17, kb::OptionsFromFlags(kb::flags::kRussian | kb::flags::kGamertag));
    Expect(limited_russian.page() == kb::Page::letters, "a limiting mode overrides an unusable first page");
  }
}

ImFont* AddFont(const char* name, float size, const ImWchar* ranges) {
  ImFontConfig config;
  std::snprintf(config.Name, sizeof(config.Name), "%s, %dpx", name, int(size));
  config.SizePixels = size;
  config.GlyphRanges = ranges;
  return ImGui::GetIO().Fonts->AddFontDefault(&config);
}

void CheckFonts() {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  static const ImWchar ascii[] = {0x20, 0x7e, 0};
  static const ImWchar latin1[] = {0x20, 0xff, 0};
  // The core's monospace font is loaded first, at the legend's size (as on the console).
  ImFont* mono20 = AddFont("mono.ttf", 20, latin1);
  ImFont* body20 = AddFont("Roboto-Medium.ttf", 20, ascii);
  ImFont* body28 = AddFont("Roboto-Medium.ttf", 28, latin1);
  ImFont* body36 = AddFont("Roboto-Medium.ttf", 36, latin1);
  unsigned char* pixels;
  int w, h;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  io.FontDefault = body28;
  io.DisplaySize = ImVec2(1920, 1080);
  io.DeltaTime = 1.0f / 60.0f;
  io.IniFilename = nullptr;
  ImGui::NewFrame();
  Expect(xe::ui::KeyboardFont(20, "Usu") == body20, "size 20: the interface's family, not the monospace font");
  Expect(xe::ui::KeyboardFont(20) == body20 && xe::ui::KeyboardFont(21) == body20, "the nearest size of the family");
  Expect(xe::ui::KeyboardFont(36, "abc") == body36 && xe::ui::KeyboardFont(48) == body36, "bigger sizes");
  Expect(xe::ui::KeyboardFont(20, "Caf\xc3\xa9") == body28, "a size without a letter of the text gives way to the default font");
  Expect(xe::ui::KeyboardFont(20, "Usu") != mono20 && xe::ui::KeyboardFont(13) != mono20, "never the other family");
  ImGui::Render();
  ImGui::DestroyContext();
}

void DrawFrames() {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1920, 1080);
  io.DeltaTime = 1.0f / 60.0f;
  io.IniFilename = nullptr;
  unsigned char* pixels;
  int w, h;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  const uint32_t modes[] = {kSims3, kb::flags::kNumeric, kb::flags::kEmail, kb::flags::kPhone, 0};
  for (uint32_t mode : modes) {
    kb::Editor editor(u"Name", 17, kb::OptionsFromFlags(mode));
    editor.Press(kb::pad::kUp);  // the action row, with its dim keys
    ImGui::NewFrame();
    xe::ui::DrawControllerKeyboard(editor, "The Sims 3", "Enter a name for your saved game:", {}, 0.2, -100, io);
    ImGui::Render();
    int vertices = 0;
    bool inside = true;
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
      for (const ImDrawVert& v : list->VtxBuffer) {
        ++vertices;
        if (v.pos.x < -1 || v.pos.y < -1 || v.pos.x > 1921 || v.pos.y > 1081) inside = false;
      }
    char what[64];
    std::snprintf(what, sizeof(what), "view in mode %08X", mode);
    Expect(vertices > 500 && inside, std::string(what) + ": drawn, on the screen");
  }
  ImGui::DestroyContext();
}

void CheckTree(const std::filesystem::path& tree) {
  const std::string view = Read(tree / ".deps/xenia-canary/src/xenia/ui/controller_keyboard_view.cc");
  Expect(Has(view, "static ImFont* Font(float size, const char* text = nullptr) { return KeyboardFont(size, text); }"),
         "the view's texts go through KeyboardFont");
  Expect(Has(view, "list->AddText(Font(size, text.c_str()), size * scale"), "texts with their own letters");
  Expect(Has(view, "Font(size, text.c_str())->CalcTextSizeA("), "widths measured with the same font");
  Expect(Has(view, "const bool enabled = editor.Enabled(row, column);"), "dim keys in the view");
  Expect(!Has(view, "for (ImFont* font : ImGui::GetIO().Fonts->Fonts) {\n      const float d"),
         "no longer the nearest font of any family");
}

}  // namespace

int main() {
  CheckAllowed();
  CheckEditor();
  CheckFonts();
  DrawFrames();
  CheckTree(std::filesystem::current_path());
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("PASS: keyboard modes and its font (fix 114b)\n");
  return 0;
}
