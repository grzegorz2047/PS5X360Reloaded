// SPDX-License-Identifier: MIT
// Fix 189 (tools/source-1.0.2/fixes/189-xna-title-notice.patch, issue #82): Charlie Murder, an
// XNA game, went straight back to the menu. Its .NET Compact Framework runtime calls the
// kernel's user mode (KeCreateUserMode, then KeFlushUserModeCurrentTb), which is not emulated;
// the runtime's thread ends and the title asks for the dashboard. The menu now says why.
//  (a) include/xbox360ps5/xna_title.hpp: the five user-mode exports and nothing else count; the
//      first such call is reported once, later ones and other unimplemented calls are not; Seen
//      stays set; the launcher's line has the game's name, or the reason alone;
//  (b) the assembled tree: the names are the core's export names (xboxkrnl_table.inc, with
//      their ordinals); the x64 backend's UndefinedCallExtern notes every unimplemented call
//      before it returns; the title switch's restart hook (which also runs for the dashboard)
//      writes notice.txt with the translated reason before it restarts the title, and the next
//      start shows notice.txt as "Could not launch the game: ...".
// Run from the assembled tree's root (run-fix-checks.sh).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("xbox360ps5/xna_title.hpp")
#include "xbox360ps5/xna_title.hpp"
#define HAVE_FIX_189 1
#else
#define HAVE_FIX_189 0
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
bool Ordered(const std::string& text, std::initializer_list<const char*> parts, size_t from = 0) {
  size_t at = from;
  for (const char* part : parts) {
    at = text.find(part, at);
    if (at == std::string::npos) return false;
    at += std::string(part).size();
  }
  return true;
}

#if HAVE_FIX_189
void CheckHeader() {
  namespace xna = xbox360ps5::xna_title;
  for (const char* name : {"KeCreateUserMode", "KeEnterUserMode", "KeLeaveUserMode", "KeFlushUserModeCurrentTb",
                           "KeFlushUserModeTb"})
    Expect(xna::IsUserModeExport(name), std::string(name) + " is a user-mode export");
  for (const char* name : {"", "KeCreateUserModeX", "keCreateUserMode", "XamLoaderLaunchTitle", "KeFlushEntireTb",
                           "KeFlushCurrentEntireTb", "XamContentGetDefaultDevice", "ObIsTitleObject"})
    Expect(!xna::IsUserModeExport(name), std::string("'") + name + "' is not a user-mode export");
  // Unimplemented calls of other games (Charlie Murder's runtime has some too) mark nothing.
  Expect(!xna::NoteUndefinedCall("StfsCreateDevice") && !xna::NoteUndefinedCall("ObIsTitleObject") && !xna::Seen(),
         "other unimplemented calls are not taken for XNA");
  Expect(xna::NoteUndefinedCall("KeCreateUserMode") && xna::Seen(), "the first user-mode call is reported and seen");
  Expect(!xna::NoteUndefinedCall("KeFlushUserModeCurrentTb") && !xna::NoteUndefinedCall("KeCreateUserMode") && xna::Seen(),
         "later user-mode calls are not reported again; still seen");
  Expect(xna::NoticeLine("Charlie Murder", "this game uses the XNA framework, which the emulator cannot run yet.") ==
             "Charlie Murder: this game uses the XNA framework, which the emulator cannot run yet.",
         "the launcher's line: the game's name, then the reason");
  Expect(xna::NoticeLine("", "reason.") == "reason.", "no name: the reason alone");
}
#endif

void CheckTree() {
  const std::string table = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc");
  for (const char* row : {"XE_EXPORT(xboxkrnl, 0x00000058, KeCreateUserMode,", "XE_EXPORT(xboxkrnl, 0x00000060, KeEnterUserMode,",
                          "XE_EXPORT(xboxkrnl, 0x00000064, KeFlushUserModeCurrentTb,",
                          "XE_EXPORT(xboxkrnl, 0x00000065, KeFlushUserModeTb,", "XE_EXPORT(xboxkrnl, 0x0000007E, KeLeaveUserMode,"})
    Expect(table.find(row) != std::string::npos, std::string("the core names the export: ") + row);

  const std::string emitter = Read(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_emitter.cc");
  Expect(emitter.find("#include \"xbox360ps5/xna_title.hpp\"") != std::string::npos, "the x64 backend includes xna_title.hpp");
  const size_t undefined = emitter.find("uint64_t UndefinedCallExtern(void* raw_context, uint64_t function_ptr) {");
  const size_t next = emitter.find("void X64Emitter::CallExtern(", undefined);
  const size_t note = emitter.find("if (xbox360ps5::xna_title::NoteUndefinedCall(function->name())) {", undefined);
  const size_t ret = emitter.find("  return 0;\n}", undefined);
  Expect(undefined != std::string::npos && note != std::string::npos && note < ret && ret < next,
         "UndefinedCallExtern notes every unimplemented call (logged or not) before it returns");

  const std::string main_code = Read("src/canary_game_main.cpp");
  Expect(main_code.find("#include \"xbox360ps5/xna_title.hpp\"") != std::string::npos, "the runtime includes xna_title.hpp");
  const size_t hook = main_code.find("xe::kernel::xam::XamModule::restart_hook = [exit_cache, xam, storage] {");
  Expect(hook != std::string::npos, "the restart hook keeps its capture list (fix 93c's check finds it by it)");
  Expect(Ordered(main_code,
                 {"if (xbox360ps5::xna_title::Seen()) {", "std::ofstream(storage / \"notice.txt\", std::ios::trunc)",
                  "xbox360ps5::xna_title::NoticeLine(xam->kernel_state()->emulator()->title_name(), Tr(\"this game uses the XNA framework, which the emulator cannot run yet.\"))",
                  "XELOGW(\"Title switch: restarting the title\");", "xbox360ps5::leaving_notice::Remember(",
                  "sceSystemServiceLoadExec(\"/app0/eboot.bin\", nullptr);"},
                 hook == std::string::npos ? 0 : hook),
         "the hook writes the XNA note into notice.txt before it restarts the title");
  Expect(Ordered(main_code, {"std::ifstream notice(storage / \"notice.txt\");",
                             "launcher.SetMessage(std::string(Tr(\"Could not launch the game: \")) + reason);",
                             "std::filesystem::remove(storage / \"notice.txt\", notice_error);"}),
         "the next start shows notice.txt once in the menu");

  const std::string xam = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_info.cc");
  Expect(Ordered(xam, {"void XamLoaderLaunchTitle_entry(", "Game requested exit to dashboard via XamLoaderLaunchTitle",
                       "if (XamModule::restart_hook) {", "void XamLoaderTerminateTitle_entry() {",
                       "if (XamModule::restart_hook) {"}),
         "a return to the dashboard (launch or terminate) runs the restart hook");
}

}  // namespace

int main() {
#if HAVE_FIX_189
  CheckHeader();
  CheckTree();
#else
  std::printf("check-xna-title-notice: fix 189 left out (no xna_title.hpp); skipped\n");
  return 0;
#endif
  if (failures) {
    std::printf("check-xna-title-notice: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-xna-title-notice: ok\n");
  return 0;
}
