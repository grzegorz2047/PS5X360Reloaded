// SPDX-License-Identifier: MIT
// Fix 93 (tools/source-1.0.2/fixes/93-bug-report-logs.patch): logs for a bug report. "Logs for a
// bug report" (Settings, System) makes the next game start one session with more detail and turns
// itself off; when the launcher next comes up it lists that session's files with their paths on
// the console, makes one .zip of them beside them, and says how to get them to a phone or a
// computer (the settings page's link and QR code, or FTP) and attach them to a GitHub issue.
//  (a) include/xbox360ps5/bug_report.hpp: the state file's text both ways (names with line
//      breaks stay on one line, unknown lines are skipped), Write/Read through a temporary file
//      (no .new left), AtBoot (a running session has ended, unless the start is its title
//      switch; other phases unchanged), Begin;
//  (b) Files: the session's logs with their .part1-.part3, the hang watch's files written since
//      the session started (older ones left out), the first boot log that exists, each file once;
//  (c) the archive: its name (letters, digits, '-', '_'), a valid ZIP whose entries inflate back
//      to the files (the boot log's last kBootLogTail bytes from a whole line), the newest
//      kArchivesKept kept, a .part of an interrupted one removed, NewestArchive by time;
//  (d) include/xbox360ps5/log_zip.hpp: Crc32's check value, Deflated round trips (text, a run
//      longer than the window, random bytes, nothing);
//  (e) ZipLink and SizeText; Active() is off unless a session turns it on;
//  (f) the assembled tree: the System option row (live, general only, off by default, two
//      choices), the log level (information lines only in a session, every system call still
//      the "Detailed logs" option's), the setting turned off and saved as the game starts before
//      settings.Apply, the in-game guide leaves the row out, recent system calls kept and
//      written when the game ends, the report shown only when the launcher is what the start
//      shows, the settings page's /bug-report.zip behind the key, web_settings.cpp without its
//      own copies of Crc32/Deflated;
//  (g) every new English key is in every language file of the argument's folder (and translated
//      in the non-English ones).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh), with
// tools/source-1.0.2/english/lang as its argument.
#undef NDEBUG
#include "xbox360ps5/bug_report.hpp"
#include "xbox360ps5/log_zip.hpp"

#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace br = xbox360ps5::bug_report;
namespace zip = xbox360ps5::log_zip;

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
  return text.str();
}
void WriteFile(const fs::path& path, const std::string& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << data;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
// Text between two markers (both included); empty when either is missing.
std::string Between(const std::string& text, const std::string& from, const std::string& to) {
  const size_t a = text.find(from);
  if (a == std::string::npos) return {};
  const size_t b = text.find(to, a);
  return b == std::string::npos ? std::string() : text.substr(a, b + to.size() - a);
}

// --- A small inflater: stored and fixed-code blocks, which is all Deflated writes. -----------
struct Bits {
  const std::string& in;
  size_t at = 0;
  uint32_t held = 0;
  int count = 0;
  bool bad = false;
  uint32_t Get(int n) {  // Least significant bit first.
    uint32_t value = 0;
    for (int i = 0; i < n; ++i) {
      if (!count) {
        if (at >= in.size()) { bad = true; return 0; }
        held = uint8_t(in[at++]);
        count = 8;
      }
      value |= (held & 1u) << i;
      held >>= 1;
      --count;
    }
    return value;
  }
  uint32_t Code(int n) {  // A Huffman code: most significant bit first.
    uint32_t value = 0;
    for (int i = 0; i < n; ++i) value = value << 1 | Get(1);
    return value;
  }
};
bool Inflate(const std::string& in, std::string* out) {
  static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  Bits bits{in};
  out->clear();
  for (bool last = false; !last;) {
    last = bits.Get(1);
    const uint32_t type = bits.Get(2);
    if (type != 1 || bits.bad) return false;
    for (;;) {
      uint32_t code = bits.Code(7), symbol;
      if (code <= 0x17) symbol = 256 + code;
      else {
        code = code << 1 | bits.Get(1);
        if (code >= 0x30 && code <= 0xBF) symbol = code - 0x30;
        else if (code >= 0xC0 && code <= 0xC7) symbol = 280 + code - 0xC0;
        else {
          code = code << 1 | bits.Get(1);
          if (code < 0x190 || code > 0x1FF) return false;
          symbol = 144 + code - 0x190;
        }
      }
      if (bits.bad) return false;
      if (symbol < 256) { *out += char(symbol); continue; }
      if (symbol == 256) break;
      const uint32_t l = symbol - 257;
      if (l >= 29) return false;
      const size_t length = length_base[l] + bits.Get(length_extra[l]);
      const uint32_t d = bits.Code(5);
      if (d >= 30) return false;
      const size_t distance = distance_base[d] + bits.Get(distance_extra[d]);
      if (bits.bad || distance > out->size()) return false;
      for (size_t n = 0; n < length; ++n) *out += (*out)[out->size() - distance];
    }
  }
  return true;
}
uint32_t Le(const std::string& data, size_t at, int bytes) {
  uint32_t value = 0;
  for (int n = 0; n < bytes; ++n) value |= uint32_t(uint8_t(data[at + n])) << (8 * n);
  return value;
}
// The entries of a ZIP archive by name; false when its structure or a CRC is wrong.
bool Unzip(const std::string& data, std::map<std::string, std::string>* entries) {
  if (data.size() < 22) return false;
  const size_t end = data.size() - 22;
  if (Le(data, end, 4) != 0x06054B50) return false;
  const uint32_t count = Le(data, end + 10, 2), directory = Le(data, end + 16, 4);
  size_t at = directory;
  for (uint32_t n = 0; n < count; ++n) {
    if (at + 46 > data.size() || Le(data, at, 4) != 0x02014B50) return false;
    const uint32_t method = Le(data, at + 10, 2), crc = Le(data, at + 16, 4), packed = Le(data, at + 20, 4),
                   size = Le(data, at + 24, 4), name_size = Le(data, at + 28, 2), local = Le(data, at + 42, 4);
    const std::string name = data.substr(at + 46, name_size);
    at += 46 + name_size + Le(data, at + 30, 2) + Le(data, at + 32, 2);
    if (local + 30 > data.size() || Le(data, local, 4) != 0x04034B50) return false;
    const size_t body = local + 30 + Le(data, local + 26, 2) + Le(data, local + 28, 2);
    if (body + packed > data.size()) return false;
    std::string contents = data.substr(body, packed);
    if (method == 8) {
      std::string inflated;
      if (!Inflate(contents, &inflated)) return false;
      contents.swap(inflated);
    } else if (method != 0) return false;
    if (contents.size() != size || zip::Crc32(contents) != crc) return false;
    (*entries)[name] = contents;
  }
  return true;
}

void CheckState(const fs::path& folder) {
  br::State state;
  state.phase = br::Phase::running;
  state.game = "Virtua Tennis 3\nphase=shown";
  state.title = "53450813";
  state.started = 1795000000;
  state.logs = {"/data/homebrew/PPSA83600/logs/Game-Virtua Tennis 3-0A1B2C3D-20261121-120000-UTC-0.log",
                "/data/homebrew/PPSA83600/logs/Game-Virtua Tennis 3-0A1B2C3D-20261121-121500-UTC-0.log"};
  const br::State back = br::Parse(br::Format(state) + "junk line\nunknown=1\n");
  Expect(back.phase == br::Phase::running, "(a) the phase reads back");
  Expect(back.game == "Virtua Tennis 3 phase=shown", "(a) a line break in a name stays on its line");
  Expect(back.title == state.title && back.started == state.started && back.logs == state.logs,
         "(a) title, start time and logs read back");
  Expect(br::Parse("").phase == br::Phase::none && br::Parse("phase=other\n").phase == br::Phase::none,
         "(a) no file or an unknown phase is none");
  Expect(br::Parse("phase=ready\r\n").phase == br::Phase::ready, "(a) a CR LF line reads");
  const fs::path file = folder / "bug-report.txt";
  Expect(br::Write(file, state), "(a) Write succeeds");
  Expect(!fs::exists(file.string() + ".new"), "(a) Write leaves no .new file");
  Expect(br::Read(file).logs == state.logs, "(a) Read gives back what Write wrote");
  Expect(br::Read(folder / "missing.txt").phase == br::Phase::none, "(a) a missing file reads as none");

  br::State ended = state;
  Expect(!br::AtBoot(ended, false) && ended.phase == br::Phase::ready,
         "(a) a running session has ended at the next start");
  br::State switched = state;
  Expect(br::AtBoot(switched, true) && switched.phase == br::Phase::running,
         "(a) a title switch goes on with the session");
  for (const br::Phase phase : {br::Phase::none, br::Phase::ready, br::Phase::shown}) {
    br::State other = state;
    other.phase = phase;
    Expect(!br::AtBoot(other, false) && other.phase == phase && !br::AtBoot(other, true) && other.phase == phase,
           std::string("(a) AtBoot leaves ") + br::PhaseName(phase) + " as it is");
  }
  const br::State begun = br::Begin("Brian Lara International Cricket 2007", "4D53085C", 1795000123);
  Expect(begun.phase == br::Phase::running && begun.game == "Brian Lara International Cricket 2007" &&
             begun.title == "4D53085C" && begun.started == 1795000123 && begun.logs.empty(),
         "(a) Begin starts a running session with no logs yet");
  Expect(!br::Active(), "(e) no session with logs for a bug report unless one turns it on");
}

// "YYYYMMDD-HHMMSS" in UTC.
std::string UtcStamp(std::time_t time) {
  const std::tm* utc = std::gmtime(&time);
  char text[32];
  std::snprintf(text, sizeof(text), "%04d%02d%02d-%02d%02d%02d", utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                utc->tm_hour, utc->tm_min, utc->tm_sec);
  return text;
}
void CheckFilesAndArchive(const fs::path& folder) {
  const fs::path logs = folder / "logs";
  fs::create_directories(logs);
  const std::string log_name = "Game-Brian Lara-0A1B2C3D-20261121-120000-UTC-0";
  WriteFile(logs / (log_name + ".log"), std::string(20000, 'a') + "\nGame log\n");
  WriteFile(logs / (log_name + ".part1.log"), "part one\n");
  WriteFile(logs / (log_name + ".part3.log"), "part three\n");
  WriteFile(logs / "Game-Other-11111111-20261121-110000-UTC-0.log", "another game\n");
  // Named half an hour into the session (fix 93f takes a hang watch's file by its name's time).
  const std::string watch_name = "hang-watch-4D53085C-" + UtcStamp(std::time(nullptr) - 1800) + "-UTC.txt";
  WriteFile(logs / watch_name, "HANG beat\n");
  WriteFile(logs / "hang-0123456789abcdef-vs.ucode", std::string("\x00\x01\x02\xff", 4));
  WriteFile(logs / "hang-watch-4D53085C-20261120-090000-UTC.txt", "an older run\n");
  const auto old_time = fs::last_write_time(logs / "hang-watch-4D53085C-20261120-090000-UTC.txt") - std::chrono::hours(3);
  fs::last_write_time(logs / "hang-watch-4D53085C-20261120-090000-UTC.txt", old_time);
  // The boot log: longer than the archive keeps.
  std::string boot;
  for (int n = 0; boot.size() < br::kBootLogTail + 300000; ++n) boot += "BOOT line " + std::to_string(n) + "\n";
  boot += "BOOT the last line\n";
  WriteFile(logs / "boot.log", boot);

  br::State state = br::Begin("Brian Lara: International Cricket 2007 (PAL)", "4D53085C", int64_t(std::time(nullptr)) - 3600);
  state.logs.push_back((logs / (log_name + ".log")).string());
  const auto files = br::Files(state, {folder / "missing-boot.log", logs / "boot.log", logs / "boot.log"});
  std::vector<std::string> names;
  for (const auto& file : files) names.push_back(file.path.filename().string());
  const std::vector<std::string> expected = {log_name + ".log", log_name + ".part1.log", log_name + ".part3.log",
                                             "hang-0123456789abcdef-vs.ucode",
                                             watch_name, "boot.log"};
  Expect(names == expected, "(b) the session's logs and parts, the hang files since its start, the boot log once");
  Expect(!files.empty() && files.back().tail && !files.front().tail, "(b) only the boot log is cut to its end");
  Expect(!files.empty() && files.front().size == 20000 + 10, "(b) the sizes are the files' sizes");
  Expect(br::Files(br::State{}, {logs / "boot.log"}).size() == 1, "(b) without a session log only the boot log");

  const std::string archive_name = br::ArchiveName(state);
  // 40 characters of the name at most.
  Expect(archive_name.rfind("BugReport-Brian_Lara_International_Cricket_2007_PA-", 0) == 0 &&
             archive_name.size() > 20 && archive_name.compare(archive_name.size() - 8, 8, "-UTC.zip") == 0,
         "(c) the archive's name: " + archive_name);
  for (const unsigned char c : archive_name)
    Expect(std::isalnum(c) || c == '-' || c == '_' || c == '.', "(c) a plain character in the archive's name");
  Expect(br::IsArchive(archive_name) && !br::IsArchive("boot.log") && !br::IsArchive("BugReport-x.zip.part"),
         "(c) IsArchive");
  Expect(br::ArchiveName(br::Begin("", "", 0)) == "BugReport-Game-19700101-000000-UTC.zip", "(c) a game without a name");

  // Older archives, and a .part left by a start that ended while it wrote.
  for (int day = 10; day < 16; ++day)
    WriteFile(logs / ("BugReport-Old-202011" + std::to_string(day) + "-120000-UTC.zip"), "old");
  WriteFile(logs / "BugReport-Old-20201101-120000-UTC.zip.part", "interrupted");
  const fs::path archive = logs / archive_name;
  Expect(br::WriteArchive(archive, files), "(c) WriteArchive succeeds");
  Expect(fs::exists(archive) && !fs::exists(archive.string() + ".part"), "(c) the archive is in place, no .part");
  Expect(!fs::exists(logs / "BugReport-Old-20201101-120000-UTC.zip.part"), "(c) an interrupted .part is removed");
  int kept = 0;
  for (const auto& entry : fs::directory_iterator(logs)) kept += br::IsArchive(entry.path().filename().string());
  Expect(kept == int(br::kArchivesKept), "(c) the newest archives are kept: " + std::to_string(kept));
  Expect(!fs::exists(logs / "BugReport-Old-20201110-120000-UTC.zip") && !fs::exists(logs / "BugReport-Old-20201111-120000-UTC.zip") &&
             fs::exists(logs / "BugReport-Old-20201112-120000-UTC.zip") && fs::exists(logs / "BugReport-Old-20201115-120000-UTC.zip"),
         "(c) the oldest go first");
  Expect(br::NewestArchive(logs) == archive, "(c) NewestArchive is the one just made");
  Expect(br::NewestArchive(folder / "nowhere").empty(), "(c) no archive in a missing folder");

  std::map<std::string, std::string> entries;
  Expect(Unzip(Read(archive), &entries), "(c) the archive is a valid ZIP whose entries inflate with their CRC");
  Expect(entries.size() == expected.size(), "(c) one entry for each file");
  Expect(entries[log_name + ".log"] == Read(logs / (log_name + ".log")), "(c) the game log is whole");
  Expect(entries["hang-0123456789abcdef-vs.ucode"] == std::string("\x00\x01\x02\xff", 4), "(c) binary files are whole");
  const std::string tail = entries["boot.log"];
  const std::string marker = "[... the start of this file is left out ...]\n";
  Expect(tail.rfind(marker, 0) == 0 && tail.size() <= br::kBootLogTail + marker.size(),
         "(c) the boot log's end only, marked as such");
  Expect(tail.size() > marker.size() && tail.compare(marker.size(), 10, "BOOT line ") == 0,
         "(c) the boot log's end starts at a whole line");
  Expect(tail.size() > 20 && tail.compare(tail.size() - 19, 19, "BOOT the last line\n") == 0,
         "(c) the boot log's last line is in it");
  Expect(!br::WriteArchive(folder / "no-such-folder" / "BugReport-x-20261121-120000-UTC.zip", files),
         "(c) a folder that cannot be written fails");
}

void CheckZip() {
  Expect(zip::Crc32("123456789") == 0xCBF43926u, "(d) Crc32's check value");
  std::string text;
  for (int n = 0; n < 5000; ++n) text += "[X360] Performance: rendered output 59.9/s, line " + std::to_string(n % 37) + "\n";
  std::string run(100000, 'z');
  std::string noise(70000, '\0');
  std::mt19937 random(93);
  for (char& c : noise) c = char(random());
  for (const std::string* data : {&text, &run, &noise}) {
    std::string back;
    Expect(Inflate(zip::Deflated(*data), &back) && back == *data, "(d) Deflated round trips");
  }
  std::string back = "x";
  Expect(Inflate(zip::Deflated(""), &back) && back.empty(), "(d) nothing deflates to nothing");
  Expect(zip::Deflated(text).size() < text.size() / 4, "(d) log lines compress");
  std::string archive;
  zip::Writer writer([&archive](const std::string& piece) { archive += piece; return true; });
  writer.Add("a.txt", text, 1795000000);
  writer.Add("b.bin", "ab", 0);
  writer.Finish();
  std::map<std::string, std::string> entries;
  Expect(Unzip(archive, &entries) && entries["a.txt"] == text && entries["b.bin"] == "ab" && writer.Count() == 2,
         "(d) Writer's archive (a stored entry too)");
  bool asked = false;
  zip::Writer refused([&asked](const std::string&) { asked = true; return false; });
  Expect(!refused.Add("a.txt", text, 0) && asked && !refused.Finish(), "(d) a refused write fails the archive");
}

void CheckLinks() {
  Expect(br::ZipLink("http://192.168.1.170:8360/?k=abc123") == "http://192.168.1.170:8360/bug-report.zip?k=abc123",
         "(e) ZipLink");
  Expect(br::ZipLink("").empty() && br::ZipLink("http://192.168.1.170:8360/").empty(), "(e) ZipLink without the page");
  Expect(br::SizeText(10) == "1 KB" && br::SizeText(2048) == "2 KB" && br::SizeText(3u << 20) == "3.0 MB",
         "(e) SizeText");
  Expect(std::string(br::kIssues) == "github.com/grzegorz2047/PS5X360Reloaded/issues", "(e) the issues address");
}

void CheckTree() {
  const std::string launcher = Read("src/launcher.cpp"), main = Read("src/canary_game_main.cpp"),
                    web = Read("platform/ps5/web_settings.cpp"), header = Read("include/xbox360ps5/launcher.hpp");
  Expect(Has(header, "int bug_report = 0;"), "(f) the setting is off by default");
  // Fix 93d moves it out of System's list (category -1) and lets a game have it.
  Expect(Has(launcher, "{\"bug_report\", &S::bug_report, 4, W::live, 0, false, \"Logs for a bug report\",") ||
             Has(launcher, "{\"bug_report\", &S::bug_report, -1, W::live, 0, true, \"Logs for a bug report\","),
         "(f) the option: live, off recommended (System's row, or fix 93d's own rows)");
  Expect(Has(launcher, "{\"Off||plural\", \"Next game\"}},"), "(f) its two choices");
  Expect(Has(launcher, "cvars::log_level = detailed_logs ? 3 : bug_report::Active() ? 2 : 1;"),
         "(f) information lines in a session, every system call still the Detailed logs option's");
  Expect(Has(launcher, "void LauncherShowBugReport(bug_report::Report report, std::function<void()> closed)"),
         "(f) the launcher's report screen");
  Expect(Has(launcher, "if (g_report_open) {") && (Has(launcher, "bug_report::ZipLink(web_url_)") || Has(launcher, "bug_report::TokenLink(web_plain_, g_report.token)")) &&
             Has(launcher, "bug_report::kIssues"),
         "(f) the screen takes the pad and shows the link and the issues address");
  const std::string start = Between(main, "if (global.bug_report) {", "if (game_log->session.Begin(log_root, chosen.name");
  Expect(!start.empty() && Has(start, "global.bug_report = 0;") && Has(start, "global.Save();") &&
             Has(start, "bug_report::active.store(true);") && Has(start, "settings.bug_report = 0;") &&
             Has(start, "settings.Apply();"),
         "(f) the setting turns itself off and is saved as the game starts, before settings.Apply");
  const size_t consumed = main.find("if (global.bug_report) {");
  Expect(consumed != std::string::npos && main.rfind("restart_into_game();", consumed) != std::string::npos &&
             !Has(start, "restart_into_game();"),
         "(f) a restart into the game's own options keeps it for the start that plays");
  Expect(Has(main, "bug_report_state.logs.push_back(log_path.string());"), "(f) the session's game log is recorded");
  Expect(Has(main, "std::strcmp(option.key, \"bug_report\") != 0"), "(f) the in-game guide leaves the row out");
  Expect(Has(main, "if (xbox360ps5::bug_report::Active()) xe::logging::KeepRecentLines(true);"),
         "(f) recent system calls are kept in a session");
  // Fix 93e writes them to a file of their own (TakeRecentLines) instead of the log.
  const std::string loop_end = Between(main, "hang_watch_thread.reset();  // fix 47", "xe::ui::controller_keyboard::Cancel();");
  Expect(Has(loop_end, "xe::logging::WriteRecentLines();") || Has(loop_end, "xe::logging::TakeRecentLines()"),
         "(f) and written when the game loop ends");
  Expect(Has(main, "xbox360ps5::bug_report::AtBoot(bug_report_state, !switch_pending.host_path.empty())"),
         "(f) a title switch goes on with the session");
  Expect(Has(main, "bug_report_state.phase == xbox360ps5::bug_report::Phase::ready && game.empty()"),
         "(f) the report shows when the launcher is what the start shows");
  Expect(Has(main, "JsonText(Tr(\"Bug report files (.zip)\"))"), "(f) the settings page's label");
  // Fix 93f moves the serving into SendBugReport and adds a route by the archive's own download
  // code before the key check; the keyed route is the "} else if" one.
  const std::string keyed = Between(web, "else if (target == \"/bug-report.zip\"", "} else if");
  const std::string sender = Between(web, "void SendBugReport(int client) {", "\n}");
  const std::string serving = Has(keyed, "SendBugReport(client);") ? sender : keyed;
  Expect(!keyed.empty() && Has(serving, "bug_report::NewestArchive(log_folder)") && Has(serving, "application/zip"),
         "(f) the settings page serves the newest archive");
  const size_t key_check = web.find("Send(client, \"403 Forbidden\"");
  Expect(key_check != std::string::npos && key_check < web.find("} else if (target == \"/bug-report.zip\""),
         "(f) behind the page's key");
  Expect(Has(web, "/bug-report.zip?k='+key+'") && Has(web, "\\\"report\\\":{\\\"name\\\":"),
         "(f) the page's Logs tab links it");
  Expect(!Has(web, "uint32_t Crc32(const std::string& data) {") && !Has(web, "std::string Deflated(const std::string& in) {") &&
             Has(web, "using log_zip::Crc32;"),
         "(f) web_settings.cpp uses log_zip.hpp's Crc32 and Deflated");
}

// "key = value" lines with "\s" for spaces (tools/source-1.0.2/english/lang).
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
  const std::vector<std::string> keys = {
      "Logs for a bug report",
      "For the next game you start, the emulator writes down more of what happens. Play until the problem shows, then close the game. A screen then lists the files to attach to your bug report. It turns itself off after that game.",
      "Next game", "Logs for a bug report: on for the next game", "Files for your bug report",
      "Detailed logs of your last game session are ready.", "Files on the console:",
      "Preparing one .zip file with all of them...", "All of them in one file:",
      "Could not make the .zip file. Copy the files one by one.",
      "Scan the code with a phone to download the .zip, or open this address on a computer on the same network:",
      "To download them from a phone or computer, turn on the option Settings page for a phone (Settings, System).",
      "With an FTP program, copy them from this folder:",
      "Then open a new issue on GitHub, describe what you did and what went wrong, and attach the files:",
      "Bug report files (.zip)"};
  const std::string launcher = Read("src/launcher.cpp"), main = Read("src/canary_game_main.cpp");
  for (const auto& key : keys)
    Expect(Has(launcher, "\"" + key + "\"") || Has(main, "\"" + key + "\""), "(g) the tree uses the key: " + key);
  int languages = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++languages;
    const std::string code = entry.path().stem().string();
    const auto table = Language(entry.path());
    for (const auto& key : keys) {
      const auto found = table.find(key);
      Expect(found != table.end() && !found->second.empty(), "(g) " + code + " has: " + key);
      if (found != table.end() && code != "en" && key != "Bug report files (.zip)")
        Expect(found->second != key, "(g) " + code + " translates: " + key);
    }
  }
  Expect(languages >= 16, "(g) every language file: " + std::to_string(languages));
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: check-bug-report <tools/source-1.0.2/english/lang>\n");
    return 2;
  }
  const fs::path folder = fs::temp_directory_path() /
      ("check-bug-report-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(folder);
  CheckState(folder);
  CheckFilesAndArchive(folder);
  CheckZip();
  CheckLinks();
  CheckTree();
  CheckLanguages(argv[1]);
  std::error_code error;
  fs::remove_all(folder, error);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-bug-report: all checks passed\n");
  return 0;
}
