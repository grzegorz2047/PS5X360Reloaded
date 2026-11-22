// SPDX-License-Identifier: MIT
// Fix 93f (tools/source-1.0.2/fixes/93f-report-session.patch): what the console showed on
// 1.0.5-work-fix93e. (1) The report of a session that had hung (its state left "running") came up
// at the next start and (2) listed other games' and sessions' hang-watch files; (3) with many
// files the hint at the bottom covered the GitHub address; (4) the QR code carried the settings
// page's key; and (5) the start after a game was about 15 s black: Stage's fsync per line cost
// about 60 ms a line right after the game's caches were written, and the memory map is 160 lines.
//  (a) bug_report.hpp: "ended=1" read and written; UtcSeconds; StampOf (a hang watch's or the
//      system calls' file name: its title ID and time);
//  (b) Files(): only the session's files: a stamped file with the session's title ID named while
//      the session ran (from its start to its game logs' last change), a hang's shader dump
//      written then; other titles, earlier and later sessions left out;
//  (c) the archive's download code: 16 hex digits, new each time, compared whole, never empty;
//      TokenLink; StartText;
//  (d) the assembled tree: the report says when a session did not end normally and when it began,
//      shows five files at most, links by the code; a week-old session is not shown; the way out
//      marks the session ended; the settings page serves /bug-report.zip by the code before its key
//      check and nothing else by it; StageBlock writes once and syncs only when asked; the memory
//      report is one synced block; BootTiming does not sync; the finer start steps are timed;
//  (e) the new text is in every language file of the argument's folder, translated.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include "xbox360ps5/bug_report.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace br = xbox360ps5::bug_report;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string s = text.str(), out;
  for (char c : s) if (c != '\r') out += c;
  return out;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a + from.size());
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}
std::string Stamp(int64_t utc) {
  std::tm parts{};
  xbox360ps5::log_zip::UtcTime(std::time_t(utc), &parts);
  char text[32];
  std::snprintf(text, sizeof(text), "%04d%02d%02d-%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                parts.tm_hour, parts.tm_min, parts.tm_sec);
  return text;
}
void Touch(const fs::path& path, int64_t utc) {
  { std::ofstream(path, std::ios::binary) << "x\n"; }
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(path, now + std::chrono::seconds(utc - int64_t(std::time(nullptr))));
}

void CheckState() {
  br::State state = br::Begin("GTA IV", "545407F2", 1795291872);
  Expect(!br::Parse(br::Format(state)).ended, "(a) a session that has not ended says so");
  state.ended = true;
  Expect(Has(br::Format(state), "ended=1\n") && br::Parse(br::Format(state)).ended, "(a) ended=1 both ways");
  Expect(br::UtcSeconds(1970, 1, 1, 0, 0, 0) == 0, "(a) UtcSeconds at 1970");
  Expect(br::UtcSeconds(2026, 11, 21, 20, 11, 12) == 1795291872, "(a) UtcSeconds of the GTA IV session");
  Expect(br::UtcSeconds(2024, 2, 29, 0, 0, 0) == 1709164800, "(a) UtcSeconds of a leap day");
  std::string title;
  int64_t utc = 0;
  Expect(br::StampOf("hang-watch-545407F2-20261121-201112-UTC.txt", "hang-watch-", &title, &utc) && title == "545407F2" &&
             utc == 1795291872,
         "(a) StampOf a hang watch's file");
  Expect(br::StampOf("recent-calls-545407f2-20261121-213259-UTC.txt", "recent-calls-", &title, &utc) && title == "545407f2",
         "(a) StampOf the system calls' file");
  for (const char* bad : {"hang-watch.txt", "hang-watch-545407F2-20261121-201112-UTC.log", "hang-watch-545407G2-20261121-201112-UTC.txt",
                          "hang-watch-545407F2-2026112X-201112-UTC.txt", "hang-watch-545407F2-20261121_201112-UTC.txt"})
    Expect(!br::StampOf(bad, "hang-watch-", &title, &utc), std::string("(a) not a stamped name: ") + bad);
}

void CheckFiles(const fs::path& folder) {
  const int64_t start = int64_t(std::time(nullptr)) - 7200;  // two hours ago, ended an hour and a half ago
  const int64_t end = start + 1800;
  const fs::path log = folder / ("Game-GTA IV-5E824984-" + Stamp(start) + "-UTC-0.log");
  Touch(log, end);
  const auto make = [&](const std::string& name, int64_t written) { Touch(folder / name, written); return name; };
  const std::string own_watch = make("hang-watch-545407F2-" + Stamp(start + 2) + "-UTC.txt", end + 3);
  const std::string own_calls = make("recent-calls-545407F2-" + Stamp(end + 1) + "-UTC.txt", end + 1);
  const std::string own_dump = make("hang-0123456789abcdef-vs.ucode", start + 300);
  const std::string other_title = make("hang-watch-434D07D8-" + Stamp(start + 100) + "-UTC.txt", start + 900);
  const std::string later = make("hang-watch-545407F2-" + Stamp(end + 600) + "-UTC.txt", end + 900);
  const std::string earlier = make("hang-watch-545407F2-" + Stamp(start - 3600) + "-UTC.txt", start - 3000);
  const std::string later_dump = make("hang-fedcba9876543210-ps.txt", end + 900);
  const std::string later_calls = make("recent-calls-4D5308C9-" + Stamp(end + 700) + "-UTC.txt", end + 700);
  br::State state = br::Begin("GTA IV", "545407F2", start);
  state.logs.push_back(log.string());
  std::set<std::string> names;
  for (const auto& file : br::Files(state, {})) names.insert(file.path.filename().string());
  Expect(names.count(log.filename().string()) && names.count(own_watch) && names.count(own_calls) && names.count(own_dump),
         "(b) the session's game log, hang watch, system calls and hang dump");
  for (const auto& name : {other_title, later, earlier, later_dump, later_calls})
    Expect(!names.count(name), "(b) not another title's or session's file: " + name);
  Expect(names.size() == 4, "(b) nothing else: " + std::to_string(names.size()) + " files");
  // A session without a title ID takes stamped files of any title written while it ran.
  br::State untitled = state;
  untitled.title.clear();
  std::set<std::string> any;
  for (const auto& file : br::Files(untitled, {})) any.insert(file.path.filename().string());
  Expect(any.count(other_title) && !any.count(later), "(b) without a title ID: by time only");
}

void CheckToken() {
  Expect(!br::TokenMatches("") && !br::TokenMatches("0123456789abcdef"), "(c) no code: nothing matches");
  const std::string a = br::NewToken(), b = br::NewToken();
  Expect(a.size() == 16 && a.find_first_not_of("0123456789abcdef") == std::string::npos, "(c) 16 hex digits: " + a);
  Expect(a != b, "(c) a new code each time");
  br::SetDownloadToken(a);
  Expect(br::TokenMatches(a), "(c) the code matches");
  Expect(!br::TokenMatches(b) && !br::TokenMatches(a.substr(0, 15)) && !br::TokenMatches(a + "0") && !br::TokenMatches(""),
         "(c) another code, a part of it, a longer one or none do not");
  br::SetDownloadToken("");
  Expect(!br::TokenMatches(a), "(c) a cleared code matches nothing");
  Expect(br::TokenLink("http://192.168.1.170:8360/", "00112233aabbccdd") == "http://192.168.1.170:8360/bug-report.zip?t=00112233aabbccdd",
         "(c) TokenLink");
  Expect(br::TokenLink("http://192.168.1.170:8360", "ab") == "http://192.168.1.170:8360/bug-report.zip?t=ab" &&
             br::TokenLink("", "ab").empty() && br::TokenLink("http://x/", "").empty(),
         "(c) TokenLink without the page or the code");
  Expect(br::StartText(1795291872) == "2026-11-21 20:11 UTC", "(c) StartText: " + br::StartText(1795291872));
}

void CheckTree() {
  const std::string launcher = Read("src/launcher.cpp"), main = Read("src/canary_game_main.cpp"),
                    web = Read("platform/ps5/web_settings.cpp"), crash = Read("platform/ps5/crash_report.cpp"),
                    crash_header = Read("include/xbox360ps5/crash_report.hpp");
  // (d) the screen.
  const std::string screen = Between(launcher, "// Fix 93d: the files for a bug report", "c.Hint('o', Tr(\"Close\")");
  const std::string screen93 = screen.empty() ? Between(launcher, "g_report_shown = Smooth(", "c.Hint('o', Tr(\"Close\")") : screen;
  Expect(Has(screen93, "if (!g_report.ended) {") &&
             Has(screen93, "Tr(\"This session did not end normally: the game stopped or the emulator was closed. It began on\")") &&
             Has(screen93, "bug_report::StartText(g_report.started)"),
         "(d) the screen says when the session did not end normally, and when it began");
  Expect(Has(screen93, "std::min<size_t>(g_report.files.size(), 5);"), "(d) five files at most (the hint stays clear)");
  Expect(Has(screen93, "bug_report::TokenLink(web_plain_, g_report.token)") && !Has(screen93, "ZipLink(web_url_)"),
         "(d) the link carries the download code, not the key");
  // (d) the start and the way out.
  Expect(Has(main, "int64_t(std::time(nullptr)) - bug_report_state.started > xbox360ps5::bug_report::kStaleSeconds"),
         "(d) a week-old session is not shown");
  Expect(Has(main, "shown.ended = bug_report_state.ended;") && Has(main, "shown.token = report::NewToken();") &&
             Has(main, "report::SetDownloadToken(shown.token);"),
         "(d) the report gets the session's end and a new download code");
  const std::string way_out = Between(main, "hang_watch_thread.reset();  // fix 47", "xe::ui::controller_keyboard::Cancel();");
  Expect(Has(way_out, "bug_report_state.ended = true;") && Has(way_out, "xbox360ps5::bug_report::Write(bug_report_file, bug_report_state);"),
         "(d) the way out marks the session ended");
  // (d) the settings page.
  const size_t token_route = web.find("if (target == \"/bug-report.zip\" && method == \"GET\" && bug_report::TokenMatches(Parameter(query, \"t\"))) {");
  const size_t key_check = web.find("if (!web_key::Accept(");
  Expect(token_route != std::string::npos && key_check != std::string::npos && token_route < key_check,
         "(d) /bug-report.zip by the download code, before the key check");
  Expect(Has(Between(web, "bug_report::TokenMatches(Parameter(query, \"t\"))) {", "}"), "SendBugReport(client);") &&
             web.find("TokenMatches") == web.rfind("TokenMatches"),
         "(d) the code opens the archive and nothing else");
  // (d) the boot log's writes.
  Expect(Has(crash_header, "void StageBlock(const char* const* lines, size_t count, bool sync);"), "(d) StageBlock declared");
  const std::string block = Between(crash, "void StageBlock(const char* const* lines, size_t count, bool sync) {", "\n}\n");
  Expect(Has(block, "(void)!write(fd, block.data(), block.size());") && Has(block, "if (sync) fsync(fd);") &&
             Has(block, "if (sync) fsync(mirror);"),
         "(d) StageBlock: one write, fsync only when asked");
  const std::string memory = Between(crash, "void ReportPlatformMemory() {", "\n}\n");
  Expect(!Has(memory, "Stage(text);") && Has(memory, "StageBlock(pointers.data(), pointers.size(), true);"),
         "(d) the memory report is one synced block");
  const std::string timing = Between(main, "void BootTiming(const char* step) {", "\n}");
  Expect(Has(timing, "xbox360ps5::StageBlock(lines, 1, false);") && !Has(timing, "xbox360ps5::Stage(text);"),
         "(d) BootTiming does not sync");
  for (const char* step : {"platform memory reported", "log stream", "imgui", "fonts loaded", "launcher made", "pad",
                           "storage measured", "saves ready", "emulator made", "platform memory reported after the setup",
                           "utility cache", "immediate drawer", "immediate drawer made"})
    Expect(Has(main, std::string("BootTiming(\"") + step + "\");"), std::string("(d) timed: ") + step);
}

std::map<std::string, std::string> Language(const fs::path& file) {
  std::map<std::string, std::string> table;
  std::istringstream lines(Read(file));
  const auto unescape = [](std::string text) {
    std::string out;
    for (size_t n = 0; n < text.size(); ++n) {
      if (text[n] == '\\' && n + 1 < text.size() && text[n + 1] == 's') { out += ' '; ++n; }
      else out += text[n];
    }
    return out;
  };
  for (std::string line; std::getline(lines, line);) {
    const size_t equals = line.find(" = ");
    if (equals != std::string::npos) table[unescape(line.substr(0, equals))] = unescape(line.substr(equals + 3));
  }
  return table;
}
void CheckLanguages(const fs::path& folder) {
  const std::string key = "This session did not end normally: the game stopped or the emulator was closed. It began on";
  int languages = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++languages;
    const std::string code = entry.path().stem().string();
    const auto table = Language(entry.path());
    const auto found = table.find(key);
    Expect(found != table.end() && !found->second.empty(), "(e) " + code + " has the text");
    if (found != table.end() && code != "en") Expect(found->second != key, "(e) " + code + " translates it");
  }
  Expect(languages >= 16, "(e) every language file: " + std::to_string(languages));
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: check-report-session <tools/source-1.0.2/english/lang>\n");
    return 2;
  }
  const fs::path folder = fs::temp_directory_path() /
      ("check-report-session-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(folder);
  CheckState();
  CheckFiles(folder);
  CheckToken();
  CheckTree();
  CheckLanguages(argv[1]);
  std::error_code error;
  fs::remove_all(folder, error);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-report-session: all checks passed\n");
  return 0;
}
