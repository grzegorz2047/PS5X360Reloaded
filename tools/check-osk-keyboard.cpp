// SPDX-License-Identifier: MIT
// Fix 114 (tools/source-1.0.2/fixes/114-osk-keyboard.patch): the on-screen keyboard games open with
// XamShowKeyboardUI (The Sims 3, Culdcept Saga, Bakugan: no keyboard, the game stuck), and capital
// letters in the launcher's new-profile name (issue #117).
//  (a) controller_keyboard.h, the real header: the UTF-16 buffer (capacity with its NUL, surrogate
//      pairs never split, big-endian, zeros to the end), bounded guest reads and page checks, the
//      editor (length limit, Shift once and locked, the four pages and the letters of the
//      interface's languages with their capitals, moving over the keys and the action row, the
//      pad's buttons, Done/Cancel, the game's flags: password, Done first, selected default text);
//  (b) what the game is told when the keyboard closes (CompletionOf): Done writes the text and
//      answers success; Cancel leaves the buffer and answers ERROR_CANCELLED (through an
//      XOVERLAPPED: its HRESULT as the extended error, completed by KernelState::CancelOverlapped);
//      a freed buffer is refused; and in the assembled tree the completion path it relies on: the
//      kernel completes a running request the game (or the keyboard) cancels with exactly those
//      codes, XamShowKeyboardUI returns IO_PENDING with an XOVERLAPPED and takes the controller
//      only once its request runs (a request cancelled before it runs left it taken in 1.0.2),
//      accepts any player index, and writes the text only on Done;
//  (c) the pad bridge: any player's controller for XUSER_INDEX_ANY, only the asked player's
//      otherwise, a release before typing, the triggers as L2/R2, held keys repeating, the
//      game's input held while open and until each controller is let go;
//  (d) the real view (controller_keyboard_view.cc) drawn with ImGui at 1080p and 720p: every
//      vertex on the screen, long texts and every page;
//  (e) the texts: every word the keyboard shows has a CoreUiText row and is in all 16 language
//      files; the launcher's name keyboard types capitals while R1 has them on.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with the
// language folder as its argument.
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
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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
// The text between two markers (empty when either is missing).
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b - a);
}
std::string U8(std::u16string_view text) { return xe::to_utf8(text); }

// --- (a) the buffer and the editor ---------------------------------------------------------------

void CheckBuffer() {
  uint8_t buffer[16];
  std::fill(std::begin(buffer), std::end(buffer), 0xAA);
  kb::WriteBE(buffer, 4, u"abcdef");
  Expect(buffer[0] == 0 && buffer[1] == 'a' && buffer[2] == 0 && buffer[3] == 'b' &&
             buffer[4] == 0 && buffer[5] == 'c',
         "WriteBE: big-endian units");
  Expect(buffer[6] == 0 && buffer[7] == 0, "WriteBE: capacity 4 keeps 3 units and the NUL");
  Expect(buffer[8] == 0xAA, "WriteBE: nothing past the game's buffer");
  std::fill(std::begin(buffer), std::end(buffer), 0xAA);
  kb::WriteBE(buffer, 8, u"x");
  bool zeros = true;
  for (int i = 2; i < 16; ++i) zeros = zeros && buffer[i] == 0;
  Expect(buffer[1] == 'x' && zeros, "WriteBE: zeros from the text's end to the buffer's end");
  std::fill(std::begin(buffer), std::end(buffer), 0xAA);
  kb::WriteBE(buffer, 1, u"abc");
  Expect(buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0xAA, "WriteBE: capacity 1 is only the NUL");
  // "Zażółć": Polish letters are single UTF-16 units, written big-endian.
  std::fill(std::begin(buffer), std::end(buffer), 0xAA);
  kb::WriteBE(buffer, 8, u"Zażółć");
  const uint8_t expected[] = {0, 'Z', 0, 'a', 0x01, 0x7c, 0x00, 0xf3, 0x01, 0x42, 0x01, 0x07, 0, 0, 0, 0};
  Expect(std::equal(std::begin(expected), std::end(expected), buffer), "WriteBE: accented letters as UTF-16 BE");

  Expect(kb::Fit(u"a\U0001F600b", 3) == u"a", "Fit: a surrogate pair is never split");
  Expect(kb::Fit(u"a\U0001F600b", 4) == u"a\U0001F600", "Fit: a whole pair fits");
  Expect(kb::Fit(std::u16string(u"x") + char16_t(0xd800) + u"y", 8) == u"x�y", "Fit: a lone surrogate becomes U+FFFD");
  Expect(kb::Fit(u"abc", 0).empty(), "Fit: no capacity, no text");

  // Bounded reads of guest text.
  const std::u16string memory = u"Name";
  const auto reader = [&](uint32_t at, char16_t& c) {
    const size_t index = (at - 0x1000) / 2;
    c = index < memory.size() ? memory[index] : 0;
    return true;
  };
  std::u16string text;
  Expect(kb::ReadText(0x1000, 16, reader, text) && text == u"Name", "ReadText: up to the NUL");
  Expect(kb::ReadText(0, 16, reader, text) && text.empty(), "ReadText: a null pointer is no text");
  Expect(!kb::ReadText(0x1001, 16, reader, text), "ReadText: an odd address is refused");
  Expect(!kb::ReadText(0x1000, 3, reader, text), "ReadText: no NUL within the limit is refused");
  int pages = 0;
  Expect(kb::ValidRange(0x1ffe, 4, [&](uint32_t) { ++pages; return true; }) && pages == 2,
         "ValidRange: every page the range touches");
  Expect(!kb::ValidRange(0xfffffffe, 4, [](uint32_t) { return true; }), "ValidRange: past 4 GiB");
  Expect(!kb::ValidRange(0x1000, 0, [](uint32_t) { return true; }), "ValidRange: an empty range");
}

// The key that types c on the editor's current page (row, column), or (-1, -1).
std::pair<int, int> Find(const kb::Editor& editor, char16_t c) {
  for (int row = 0; row < kb::kCharacterRows; ++row)
    for (int column = 0; column < kb::kColumns; ++column)
      if (editor.Key(row, column) == c) return {row, column};
  return {-1, -1};
}
// Moves the selection to (row, column) with the pad's directions, as a player would.
void MoveTo(kb::Editor& editor, int row, int column) {
  for (int guard = 0; guard < 20 && editor.row() != row; ++guard) editor.Press(kb::pad::kDown);
  for (int guard = 0; guard < 20 && editor.column() != column; ++guard) editor.Press(kb::pad::kRight);
}
bool Type(kb::Editor& editor, char16_t c) {
  for (int page = 0; page < kb::kPages; ++page) {
    const auto [row, column] = Find(editor, c);
    if (row >= 0) {
      MoveTo(editor, row, column);
      const size_t before = editor.text().size();
      editor.Press(kb::pad::kCross);
      return editor.text().size() == before + 1;
    }
    editor.Press(kb::pad::kR2);
  }
  return false;
}

void CheckEditor() {
  {
    kb::Editor editor(u"", 6);  // the game's buffer: 6 units, so 5 characters
    Expect(editor.max_length() == 5, "max length is the buffer less its NUL");
    for (char16_t c : std::u16string(u"hello")) Expect(Type(editor, c), "types the letters of hello");
    Expect(editor.text() == u"hello", "the text is hello");
    Expect(!editor.Insert(u"!") && editor.refused() == 1 && editor.text() == u"hello",
           "a sixth character is refused and counted (the view warns)");
    editor.Press(kb::pad::kSquare);
    Expect(editor.text() == u"hell", "Square deletes");
    editor.Press(kb::pad::kTriangle);
    Expect(editor.text() == u"hell ", "Triangle types a space");
    editor.Press(kb::pad::kL1);
    editor.Press(kb::pad::kL1);
    Expect(editor.cursor() == 3, "L1 moves the cursor left");
    editor.Press(kb::pad::kSquare);
    editor.Insert(u"X");
    Expect(editor.text() == u"heXl " && editor.cursor() == 3, "Delete and typing work at the cursor");
    editor.Press(kb::pad::kR1);
    Expect(editor.cursor() == 4, "R1 moves the cursor right");
  }
  {
    // Capital letters (issue #117's request, here in the game keyboard): Shift for one letter,
    // pressed twice for all of them (Caps).
    kb::Editor editor(u"", 32);
    editor.Press(kb::pad::kL2);
    Expect(editor.shift() == kb::Shift::once && editor.Key(1, 0) == u'Q', "L2: Shift shows capitals");
    Expect(Type(editor, u'J') && editor.shift() == kb::Shift::off, "Shift types one capital, then turns off");
    Expect(Type(editor, u'o') && Type(editor, u'e'), "then small letters");
    editor.Press(kb::pad::kL2);
    editor.Press(kb::pad::kL2);
    Expect(editor.shift() == kb::Shift::lock, "Shift twice: Caps");
    Expect(Type(editor, u'A') && Type(editor, u'B') && editor.shift() == kb::Shift::lock, "Caps stays on");
    Expect(editor.Key(0, 0) == u'1', "digits are not changed by Shift");
    editor.Press(kb::pad::kL2);
    Expect(editor.shift() == kb::Shift::off, "a third press turns capitals off");
    Expect(editor.text() == u"JoeAB", "Joe, then AB");
    // Shift on the action row, as a key.
    MoveTo(editor, kb::kRows - 1, 0);
    Expect(kb::ActionAt(editor.column()).action == kb::Action::shift, "the action row starts with Shift");
    editor.Press(kb::pad::kCross);
    Expect(editor.shift() == kb::Shift::once, "the Shift key works as L2");
  }
  {
    // Every page: 40 keys, none twice; the letters each interface language needs.
    std::set<char16_t> all;
    for (int page = 0; page < kb::kPages; ++page) {
      const auto keys = kb::PageKeys(kb::Page(page));
      Expect(keys.size() == size_t(kb::kCharacterRows * kb::kColumns), "a page has 4 rows of 10 keys");
      std::set<char16_t> unique(keys.begin(), keys.end());
      Expect(unique.size() == keys.size(), "no key twice on a page");
      all.insert(keys.begin(), keys.end());
    }
    const std::map<std::string, std::u16string> languages = {
        {"English", u"abcdefghijklmnopqrstuvwxyz0123456789"},
        {"German", u"äöüß"},
        {"Spanish", u"áéíóúñü¿¡"},
        {"French", u"àâæçéèêëîïôœùûüÿ"},
        {"Italian", u"àèéìíîòóùú"},
        {"Dutch", u"éëïóöü"},
        {"Polish", u"ąćęłńóśźż"},
        {"Portuguese", u"áâãàçéêíóôõú"},
        {"Turkish", u"çğıöşü"},
        {"Russian", u"абвгдеёжзийклм"
                    u"нопрстуфхцчшщъ"
                    u"ыьэюя"},
        {"symbols", u"!@#$%&*()-_=+?/:;'\".,"},
    };
    for (const auto& [language, letters] : languages)
      for (char16_t c : letters) Expect(all.count(c) == 1, language + ": a key for " + U8(std::u16string(1, c)));
    // Their capitals.
    const std::pair<char16_t, char16_t> capitals[] = {
        {u'a', u'A'}, {0xe4, 0xc4}, {0xe9, 0xc9}, {0xf1, 0xd1}, {0xff, 0x178}, {0x153, 0x152},
        {0x105, 0x104}, {0x107, 0x106}, {0x119, 0x118}, {0x142, 0x141}, {0x144, 0x143},
        {0x15b, 0x15a}, {0x17a, 0x179}, {0x17c, 0x17b}, {0x11f, 0x11e}, {0x15f, 0x15e},
        {0x131, 0x130}, {0x430, 0x410}, {0x44f, 0x42f}, {0x451, 0x401}, {0xba, 0xaa},
        {0xdf, 0xdf}, {u'1', u'1'}, {u'?', u'?'}, {0x104, 0x104}, {0x141, 0x141}, {0x17b, 0x17b}};
    for (const auto& [small, capital] : capitals)
      Expect(kb::Upper(small) == capital, "capital of " + U8(std::u16string(1, small)));
    for (int page : {int(kb::Page::letters), int(kb::Page::accents), int(kb::Page::cyrillic)})
      for (char16_t c : kb::PageKeys(kb::Page(page)))
        if ((c >= u'a' && c <= u'z') || c > 0x7f)
          Expect(c == 0xdf || kb::Upper(c) != c, "every letter has a capital: " + U8(std::u16string(1, c)));
    // Typing a Polish word with capitals across pages, then the buffer the game gets.
    kb::Editor editor(u"", 8);
    editor.Press(kb::pad::kL2);
    for (char16_t c : std::u16string(u"Zażółć")) Expect(Type(editor, c), "types " + U8(std::u16string(1, c)));
    Expect(editor.text() == u"Zażółć", "Zażółć typed with the pad");
  }
  {
    // Moving over the keys: wrapping, the action row key by key, back up to the same column.
    kb::Editor editor(u"", 16);
    editor.Press(kb::pad::kUp);
    Expect(editor.row() == kb::kRows - 1, "up from the top row goes to the action row");
    editor.Press(kb::pad::kDown);
    Expect(editor.row() == 0, "down from the action row goes to the top row");
    editor.Press(kb::pad::kLeft);
    Expect(editor.column() == kb::kColumns - 1, "left from the first key goes to the last");
    MoveTo(editor, 3, 3);
    editor.Press(kb::pad::kDown);
    Expect(editor.row() == 4 && editor.column() == 2 && kb::ActionAt(2).action == kb::Action::space,
           "down from column 3 selects Space (columns 2-3)");
    std::vector<kb::Action> order;
    for (int i = 0; i < kb::kActionCount; ++i) {
      order.push_back(kb::ActionAt(editor.column()).action);
      editor.Press(kb::pad::kRight);
    }
    Expect(order.size() == 8 && order[0] == kb::Action::space && order[5] == kb::Action::done &&
               order[6] == kb::Action::shift && order[7] == kb::Action::page,
           "right goes key by key along the action row and wraps");
    Expect(kb::ActionAt(editor.column()).action == kb::Action::space, "eight steps come back to Space");
    editor.Press(kb::pad::kUp);
    Expect(editor.row() == 3 && editor.column() == 2, "up from Space comes back to column 2");
    int columns = 0;
    for (const auto& key : kb::kActions) columns += key.span;
    Expect(columns == kb::kColumns, "the action row spans the 10 columns");
    MoveTo(editor, 4, 2);
    editor.Press(kb::pad::kCross);
    Expect(editor.text() == u" ", "the Space key");
  }
  {
    kb::Editor editor(u"abc", 16);
    editor.Press(kb::pad::kCircle | kb::pad::kOptions);
    Expect(editor.result() == kb::Result::cancelled, "Circle cancels, and wins over OPTIONS in the same sample");
    Expect(!editor.Insert(u"x") && editor.text() == u"abc", "nothing is typed after closing");
    kb::Editor done(u"abc", 16);
    done.Press(kb::pad::kOptions);
    Expect(done.result() == kb::Result::accepted, "OPTIONS is Done");
    kb::Editor keys(u"", 16);
    MoveTo(keys, 4, 7);
    keys.Press(kb::pad::kCross);
    Expect(keys.result() == kb::Result::cancelled, "the Cancel key");
    kb::Editor first(u"", 16, kb::OptionsFromFlags(kb::flags::kSelectOk));
    Expect(first.row() == 4 && kb::ActionAt(first.column()).action == kb::Action::done, "VKBD_SELECT_OK: Done selected first");
    first.Press(kb::pad::kCross);
    Expect(first.result() == kb::Result::accepted, "the Done key");
  }
  {
    // The game's default text: kept, cut to the buffer, selected with VKBD_HIGHLIGHT_TEXT.
    kb::Editor plain(u"Player", 16);
    Expect(plain.text() == u"Player" && plain.cursor() == 6 && !plain.highlighted(), "the default text, caret at its end");
    kb::Editor cut(u"Player One", 7);
    Expect(cut.text() == u"Player", "a default text longer than the buffer is cut");
    kb::Editor selected(u"Player", 16, kb::OptionsFromFlags(kb::flags::kHighlightText));
    Expect(selected.highlighted(), "VKBD_HIGHLIGHT_TEXT selects the default text");
    selected.Insert(u"A");
    Expect(selected.text() == u"A" && !selected.highlighted(), "typing replaces a selected default text");
    kb::Editor erase(u"Player", 16, kb::OptionsFromFlags(kb::flags::kHighlightText));
    erase.Backspace();
    Expect(erase.text().empty(), "Delete clears a selected default text");
    kb::Editor keep(u"Player", 16, kb::OptionsFromFlags(kb::flags::kHighlightText));
    keep.Left();
    Expect(keep.text() == u"Player" && !keep.highlighted(), "moving the cursor keeps it");
    kb::Editor secret(u"abc", 16, kb::OptionsFromFlags(kb::flags::kPassword));
    Expect(secret.Shown() == u"•••" && secret.text() == u"abc", "VKBD_LATIN_PASSWORD shows dots");
    Expect(kb::OptionsFromFlags(kb::flags::kRussian).page == kb::Page::cyrillic, "VKBD_RUSSIAN_FULL opens the Russian page");
    Expect(kb::OptionsFromFlags(0x1).page == kb::Page::letters && !kb::OptionsFromFlags(0x1).password,
           "VKBD_LATIN_FULL: the letters");
    kb::Editor pair(u"a\U0001F600", 16);
    pair.Backspace();
    Expect(pair.text() == u"a", "Delete removes a whole surrogate pair");
  }
}

// --- (b) what the game is told ------------------------------------------------------------------

void CheckCompletion(const std::filesystem::path& tree) {
  const auto done = kb::CompletionOf(kb::Result::accepted, true, true);
  Expect(done.result == 0 && done.extended_error == 0 && done.write_text && !done.cancel_request,
         "Done through an XOVERLAPPED: success, the text written");
  const auto done_sync = kb::CompletionOf(kb::Result::accepted, false, true);
  Expect(done_sync.result == 0 && done_sync.write_text, "Done for a game that waits: success, the text written");
  const auto cancel = kb::CompletionOf(kb::Result::cancelled, true, true);
  Expect(cancel.result == 0x4c7 && cancel.extended_error == 0x800704c7 && !cancel.write_text &&
             cancel.cancel_request,
         "Cancel through an XOVERLAPPED: ERROR_CANCELLED, HRESULT extended error, completed as cancelled, buffer untouched");
  const auto cancel_sync = kb::CompletionOf(kb::Result::cancelled, false, true);
  Expect(cancel_sync.result == 0x4c7 && cancel_sync.extended_error == 0x4c7 && !cancel_sync.write_text &&
             !cancel_sync.cancel_request,
         "Cancel for a game that waits: ERROR_CANCELLED returned");
  const auto closed = kb::CompletionOf(kb::Result::pending, true, true);
  Expect(closed.result == 0x4c7 && !closed.write_text, "a keyboard closed by the emulator (leaving the game) is a Cancel");
  const auto gone = kb::CompletionOf(kb::Result::accepted, true, false);
  Expect(gone.result == 0x57 && !gone.write_text, "Done with a freed buffer: invalid parameter, nothing written");

  const auto core = tree / ".deps/xenia-canary/src/xenia";
  // The kernel's side: a running request marked cancelled completes as ERROR_CANCELLED with its
  // HRESULT and length 0, whatever its callback returned.
  const std::string kernel = Read(core / "kernel/kernel_state.cc");
  const std::string deferred = Between(kernel, "void KernelState::CompleteOverlappedDeferredEx(\n    std::function<X_RESULT(uint32_t&, uint32_t&)>", "bool KernelState::CancelOverlapped");
  Expect(Has(deferred, "if (it->second.cancelled) {\n          SetOverlappedCompletion(overlapped_ptr, X_ERROR_CANCELLED,\n                                  X_HRESULT_FROM_WIN32(X_ERROR_CANCELLED), 0);"),
         "kernel: a cancelled running request completes as ERROR_CANCELLED");
  const std::string cancel_fn = Between(kernel, "bool KernelState::CancelOverlapped", "bool KernelState::Save");
  Expect(Has(cancel_fn, "// Already running, it completes as cancelled once finished.\n  it->second.cancelled = true;"),
         "kernel: CancelOverlapped marks a running request (no wait with wait=false)");

  const std::string xam = Read(core / "kernel/xam/xam_ui.cc");
  const std::string entry = Between(xam, "dword_result_t XamShowKeyboardUI_entry(", "#else\n  const auto read_text");
  Expect(!entry.empty(), "xam_ui.cc: XamShowKeyboardUI's PS5 path");
  Expect(!Has(entry, "user_index >= 4"), "any player index is accepted (XUSER_INDEX_ANY)");
  const size_t run_at = entry.find("auto run = [");
  const size_t begin_at = entry.find("keyboard::Bridge().Begin(user)");
  const size_t flag_at = entry.find("is_xam_dialog_present_.store(true)");
  Expect(run_at != std::string::npos && begin_at > run_at && flag_at > run_at && begin_at != std::string::npos &&
             flag_at != std::string::npos,
         "the controller and the system interface are taken only when the request runs");
  Expect(Has(entry, "keyboard::CompletionOf(completion->result, asynchronous,"), "the run uses CompletionOf");
  Expect(Has(entry, "if (outcome.write_text) {\n      keyboard::WriteBE(output, capacity, completion->text);"),
         "the text is written only on Done");
  Expect(entry.find("WriteBE") == entry.rfind("WriteBE"), "WriteBE in one place");
  Expect(Has(entry, "if (outcome.cancel_request) state->CancelOverlapped(overlapped_address, false);"),
         "Cancel completes the XOVERLAPPED as cancelled");
  Expect(Has(entry, "extended_error = outcome.extended_error;\n    // Keep XAM's existing convention: InternalHigh is not a string length.\n    return outcome.result;"),
         "the run returns CompletionOf's codes");
  Expect(Has(entry, "state->CompleteOverlappedDeferredEx(run, overlapped, pre, post);\n    return X_ERROR_IO_PENDING;"),
         "with an XOVERLAPPED: IO_PENDING, completed when the keyboard closes");
  Expect(Has(entry, "pre();\n  uint32_t extended_error = 0, length = 0;\n  const auto result = run(extended_error, length);\n  post();\n  return result;"),
         "without one: the game waits and gets the result");
  Expect(Has(entry, "keyboard::codes::kCancelled == X_ERROR_CANCELLED"), "the codes are checked against XAM's");
  Expect(Has(entry, "keyboard::OptionsFromFlags(flags)") && Has(entry, "completion, options, labels"),
         "the game's flags and the labels reach the dialog");
  Expect(Has(entry, "Keyboard (fix 114): the game asks for text") && Has(entry, "closed with Done, {} characters"),
         "the log says what happened (never the text)");
  Expect(!Has(Between(entry, "XELOGW(\"Keyboard (fix 114): closed with Done, {} characters\"", ";"), "completion->text)"),
         "the log gives the text's length, never the text");
  const std::string dialog = Read(core / "ui/controller_keyboard_dialog.cc");
  Expect(Has(dialog, "editor_.result() == controller_keyboard::Result::accepted\n      ? controller_keyboard::Result::accepted : controller_keyboard::Result::cancelled;"),
         "dialog: anything but Done is a Cancel");
  Expect(Has(dialog, "controller_keyboard::Bridge().End();"), "dialog: the controller is given back when it closes");
  Expect(Has(dialog, "XELOGW(\"Keyboard (fix 114): on the screen\");"), "dialog: logs that it was drawn");
}

// --- (c) the pad bridge -------------------------------------------------------------------------

struct Pad {
  uint32_t buttons = 0;
  uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
  bool connected = true;
};

void CheckBridge() {
  kb::PadBridge bridge;
  Expect(!bridge.Begin(0), "no keyboard before the interface is ready");
  bridge.SetAvailable(true);
  Expect(bridge.Begin(0xff) && bridge.any_user(), "XUSER_INDEX_ANY: any controller types");
  Expect(!bridge.Begin(0), "one keyboard at a time");
  Pad held;
  held.buttons = kb::pad::kCross;
  bridge.Publish(2, held, 0);
  bridge.Publish(2, held, 10);
  Expect(bridge.TakePressed() == 0, "a button held from the game does not type");
  bridge.Publish(2, Pad{}, 20);
  bridge.Publish(2, held, 30);
  Expect(bridge.TakePressed() == kb::pad::kCross, "after a release, player 3's Cross types");
  bridge.Publish(0, Pad{}, 40);
  Pad trigger;
  trigger.r2 = 200;
  bridge.Publish(0, trigger, 50);
  Expect(bridge.TakePressed() == kb::pad::kR2, "player 1's R2 trigger is R2");
  for (uint32_t user = 0; user < 4; ++user) Expect(bridge.Suppressed(user), "every player's game input is held while open");
  bridge.End();
  Expect(bridge.Suppressed(0) && bridge.Suppressed(2), "after closing, held until the controller is let go");
  bridge.Publish(0, Pad{}, 60);
  Expect(!bridge.Suppressed(0) && bridge.Suppressed(2), "released per controller");

  Expect(bridge.Begin(1) && !bridge.any_user(), "player 2's keyboard");
  bridge.Publish(0, Pad{}, 0);
  bridge.Publish(0, held, 10);
  Expect(bridge.TakePressed() == 0, "another player's controller does not type");
  bridge.Publish(1, Pad{}, 20);
  Pad down;
  down.buttons = kb::pad::kDown;
  bridge.Publish(1, down, 1000);
  Expect(bridge.TakePressed() == kb::pad::kDown, "player 2's D-pad moves");
  bridge.Publish(1, down, 1300);
  Expect(bridge.TakePressed() == 0, "no repeat before 400 ms");
  bridge.Publish(1, down, 1400);
  Expect(bridge.TakePressed() == kb::pad::kDown, "held down, it repeats");
  bridge.Publish(1, down, 1460);
  Expect(bridge.TakePressed() == 0, "then every 110 ms");
  bridge.Publish(1, down, 1510);
  Expect(bridge.TakePressed() == kb::pad::kDown, "and again");
  Pad erase;
  erase.buttons = kb::pad::kSquare;
  bridge.Publish(1, erase, 2000);
  bridge.TakePressed();
  bridge.Publish(1, erase, 2400);
  Expect(bridge.TakePressed() == kb::pad::kSquare, "a held Square keeps deleting");
  bridge.SetAvailable(false);
  Expect(bridge.cancelled(), "leaving the game cancels an open keyboard");
  bridge.End();
}

// --- (d) the view -------------------------------------------------------------------------------

void DrawFrames(float width, float height) {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(width, height);
  io.DeltaTime = 1.0f / 60.0f;
  io.IniFilename = nullptr;
  unsigned char* pixels;
  int w, h;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  xe::ui::KeyboardLabels german;
  german.keyboard = "Tastatur";
  german.space = "Leerzeichen";
  german.erase = "L\xc3\xb6schen";
  german.shift = "Umschalt";
  german.caps = "Feststell";
  german.cancel = "Abbrechen";
  german.done = "Fertig";
  german.other = "Weitere Zeichen";
  german.cursor = "Cursor bewegen";
  german.full = "Maximale L\xc3\xa4nge erreicht";
  const std::string long_description(900, 'w');
  for (int frame = 0; frame < 8; ++frame) {
    kb::Editor editor(frame % 2 ? u"" : std::u16string(200, u'M'), 256,
                      kb::OptionsFromFlags(frame == 3 ? kb::flags::kPassword : frame == 5 ? kb::flags::kHighlightText : 0));
    for (int page = 0; page < frame % kb::kPages; ++page) editor.NextPage();
    if (frame >= 4) editor.CycleShift();
    if (frame == 6) editor.Press(kb::pad::kUp);
    ImGui::NewFrame();
    xe::ui::DrawControllerKeyboard(editor, frame % 2 ? "" : "Name Your Saved Game",
                                   frame < 4 ? long_description : "Enter a name for your saved game:",
                                   frame % 3 ? xe::ui::KeyboardLabels{} : german, frame * 0.3, frame == 2 ? frame * 0.3 : -100.0,
                                   io);
    ImGui::Render();
    const ImDrawData* data = ImGui::GetDrawData();
    int vertices = 0;
    bool inside = true;
    for (const ImDrawList* list : data->CmdLists) {
      for (const ImDrawVert& v : list->VtxBuffer) {
        ++vertices;
        if (v.pos.x < -1 || v.pos.y < -1 || v.pos.x > width + 1 || v.pos.y > height + 1) inside = false;
      }
    }
    char what[96];
    std::snprintf(what, sizeof(what), "view %.0fx%.0f frame %d", width, height, frame);
    Expect(vertices > 500, std::string(what) + ": drawn");
    Expect(inside, std::string(what) + ": everything on the screen");
  }
  ImGui::DestroyContext();
}

// --- (e) the texts and the launcher's name keyboard --------------------------------------------

std::map<std::string, std::string> ReadLanguage(const std::filesystem::path& path) {
  std::map<std::string, std::string> entries;
  std::istringstream lines(Read(path));
  std::string line;
  const auto unescape = [](std::string text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\\' && i + 1 < text.size()) {
        const char next = text[++i];
        out += next == 's' ? ' ' : next == 'n' ? '\n' : next;
      } else {
        out += text[i];
      }
    }
    return out;
  };
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t at = line.find(" = ");
    if (line.empty() || line[0] == '#' || at == std::string::npos) continue;
    entries[unescape(line.substr(0, at))] = unescape(line.substr(at + 3));
  }
  return entries;
}

void CheckTexts(const std::filesystem::path& tree, const std::filesystem::path& lang) {
  const std::string xam = Read(tree / ".deps/xenia-canary/src/xenia/kernel/xam/xam_ui.cc");
  const std::string main = Read(tree / "src/canary_game_main.cpp");
  const std::string table = Between(main, "static const char* CoreUiText(const char* english) {", "  for (const auto& entry : entries) {");
  std::vector<std::string> words;
  for (size_t at = xam.find(" = text(\""); at != std::string::npos; at = xam.find(" = text(\"", at + 1)) {
    const size_t start = at + 9, end = xam.find('"', start);
    words.push_back(xam.substr(start, end - start));
  }
  Expect(words.size() == 11, "xam_ui.cc asks for the keyboard's 11 words");
  const char* const codes[] = {"de", "en", "es-419", "es", "fr", "it", "ja", "ko", "nl", "pl",
                               "pt-BR", "pt", "ru", "tr", "zh-Hans", "zh-Hant"};
  std::map<std::string, std::map<std::string, std::string>> files;
  for (const char* code : codes) {
    files[code] = ReadLanguage(lang / (std::string(code) + ".txt"));
    Expect(!files[code].empty(), std::string("language file ") + code);
  }
  for (const auto& word : words) {
    Expect(Has(table, "{\"" + word + "\", \"" + word + "\"}"), "CoreUiText row for " + word);
    for (const char* code : codes)
      Expect(files[code].count(word) && !files[code][word].empty(), std::string(code) + ".txt translates " + word);
  }
  Expect(files["pl"]["Keyboard"] == "Klawiatura" && files["de"]["Shift"] == "Umschalt" &&
             files["ru"]["Other characters"] == "\xd0\x94\xd1\x80\xd1\x83\xd0\xb3\xd0\xb8\xd0\xb5 "
                                                "\xd1\x81\xd0\xb8\xd0\xbc\xd0\xb2\xd0\xbe\xd0\xbb\xd1\x8b",
         "sample translations");

  // Issue #117: the launcher's new-profile name.
  const std::string launcher = Read(tree / "src/launcher.cpp");
  const std::string header = Read(tree / "include/xbox360ps5/launcher.hpp");
  Expect(Has(header, "bool name_caps_ = false;"), "launcher.hpp: the name keyboard's capitals");
  Expect(Has(launcher, "static bool NameKeyCapital(const std::string& name, bool caps) { return name.empty() || caps; }"),
         "the first letter is a capital, the others while capitals are on");
  const std::string name_mode = Between(launcher, "    case Mode::name: {", "void Launcher::RefreshProfiles()");
  Expect(Has(name_mode, "if (key == Key::r1) name_caps_ = !name_caps_;"), "R1 turns capitals on and off");
  Expect(Has(name_mode, "new_name_ += NameKeyCapital(new_name_, name_caps_) ? symbol :"), "letters typed as R1 sets them");
  Expect(!Has(name_mode, "new_name_ += new_name_.empty() ? symbol"), "no longer small letters only after the first");
  const std::string sheet = Between(launcher, "void Launcher::DrawNameSheet(Canvas& c) {", "\n}\n");
  Expect(Has(sheet, "NameKeyCapital(new_name_, name_caps_) ? symbol :"), "the keys show the letter as it will be typed");
  Expect(Has(sheet, "Tr(\"Shift\")") && Has(sheet, "name_caps_ ? kAccent : kMuted"), "the R1 hint, lit while capitals are on");
  const std::string entering = Between(launcher, "          mode_ = Mode::name;", "key_row_ = key_column_ = 0;");
  Expect(Has(entering, "name_caps_ = false;"), "a new name starts without capitals on");
  // ProfileManager accepts mixed case (it is what the launcher's create hook checks).
  const std::string profiles = Read(tree / ".deps/xenia-canary/src/xenia/kernel/xam/profile_manager.cc");
  Expect(Has(profiles, "^[A-Za-z][A-Za-z0-9]*( [A-Za-z0-9]+)*$"), "gamertags may hold capitals anywhere");
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path tree = std::filesystem::current_path();
  const std::filesystem::path lang = argc > 1 ? argv[1] : tree / "assets/lang";
  CheckBuffer();
  CheckEditor();
  CheckCompletion(tree);
  CheckBridge();
  DrawFrames(1920, 1080);
  DrawFrames(1280, 720);
  CheckTexts(tree, lang);
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("PASS: on-screen keyboard (fix 114)\n");
  return 0;
}
