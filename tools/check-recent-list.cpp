// SPDX-License-Identifier: MIT
// Fix 181 (tools/source-1.0.2/fixes/181-recent-list-restart.patch): a game that starts without the
// library (launch-once.txt: a game with start options of its own restarting the app into itself)
// keeps the other recently played games. recent_list.hpp:
//  (a) the self-restart: recent.txt holds five games, the restart's RecordLaunch has no scanned list,
//      reads the file and puts the started game first: the five stay, in their order;
//  (b) dedupe: the started game already in the list moves to the front once; a path the file names
//      twice is kept once (its first place);
//  (c) the multi-disc set (fix 89): the started disc goes first and fix 89's DefaultDisc then picks
//      it; another game's set keeps its disc (it is still in the list, in its place); fix 112's
//      group recency (the first line naming one of a group's games) follows the same order;
//  (d) a corrupt file (a NUL or another control character, a line longer than kMaxLine, a file
//      larger than kMaxBytes) is not used: the list is the started game alone, as before the fix,
//      with the reason; a missing file is an empty list; CRLF and empty lines are read as Scan does;
//  (e) the limit: 12 in all, the oldest left out, as before;
//  (f) Save writes through recent.txt.new and a rename, and leaves no temporary;
//  (g) the assembled launcher (when run from the tree's root): Scan marks the list read,
//      RecordLaunch reads the file when it is not, puts the game in front with Front, saves with Save
//      and no longer writes recent.txt itself.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/disc_catalog.cpp"
#include "xbox360ps5/recent_list.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace rl = xbox360ps5::recent_list;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
using List = std::vector<std::string>;

void Put(const fs::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}
std::string Get(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
std::string Read(const std::string& path) {
  std::string text = Get(path), plain;
  for (char c : text) if (c != '\r') plain += c;
  return plain;
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// What RecordLaunch does after fix 181 when no scan read the list (the restart's case).
List RecordWithoutScan(const fs::path& file, const std::string& path, std::string* why = nullptr) {
  const rl::Loaded loaded = rl::Load(file);
  if (why) *why = loaded.why;
  List list = rl::Front(path, loaded.usable ? loaded.paths : List());
  Expect(rl::Save(file, list), "the list is saved");
  return list;
}

const std::string kRoot = "/mnt/ext1/xbox360/";

void Checks(const fs::path& folder) {
  fs::remove_all(folder);
  fs::create_directories(folder);
  const fs::path file = folder / "recent.txt";
  const std::string halo = kRoot + "Halo 3.iso", gta = kRoot + "GTA IV.iso", rdr = kRoot + "RDR.iso",
                    fm = kRoot + "Forza 4/Disc 1.iso", ks = kRoot + "Kinect Sports.iso",
                    zumba = kRoot + "Zumba Fitness.iso";

  // (a) The self-restart: Kinect Sports (start options of its own) restarts into itself.
  Put(file, halo + "\n" + gta + "\n" + rdr + "\n" + fm + "\n" + zumba + "\n");
  List after = RecordWithoutScan(file, ks);
  Expect(after == List({ks, halo, gta, rdr, fm, zumba}), "(a) the five other games stay, in their order");
  Expect(Get(file) == ks + "\n" + halo + "\n" + gta + "\n" + rdr + "\n" + fm + "\n" + zumba + "\n",
         "(a) recent.txt holds them");
  // Before fix 181 the restart's list was the started game alone.
  Expect(rl::Front(ks, {}) == List({ks}), "(a) without a list: the game alone (the old behaviour)");
  // The same restart again: nothing changes but the order it already has.
  after = RecordWithoutScan(file, ks);
  Expect(after == List({ks, halo, gta, rdr, fm, zumba}), "(a) a second restart into it keeps the list");

  // (b) Dedupe.
  after = RecordWithoutScan(file, rdr);
  Expect(after == List({rdr, ks, halo, gta, fm, zumba}), "(b) a listed game moves to the front once");
  Put(file, halo + "\n" + gta + "\n" + halo + "\n" + rdr + "\n");
  const rl::Loaded twice = rl::Load(file);
  Expect(twice.usable && twice.paths == List({halo, gta, rdr}), "(b) a path named twice is kept once (first place)");
  Expect(rl::Front(gta, {halo, gta, gta, rdr}) == List({gta, halo, rdr}), "(b) Front keeps each path once");

  // (c) Multi-disc sets (fix 89): the set's disc is the one found first in the list.
  const std::string ff1 = kRoot + "FFXIII/Final Fantasy XIII (Disc 1).iso",
                    ff2 = kRoot + "FFXIII/Final Fantasy XIII (Disc 2).iso",
                    ff3 = kRoot + "FFXIII/Final Fantasy XIII (Disc 3).iso",
                    bf1 = kRoot + "BF3/Battlefield 3 (Disc 1).iso", bf2 = kRoot + "BF3/Battlefield 3 (Disc 2).iso";
  const List ff{ff1, ff2, ff3}, bf{bf1, bf2};
  Put(file, ff2 + "\n" + halo + "\n" + bf2 + "\n" + ff1 + "\n");
  Expect(xbox360ps5::multidisc::DefaultDisc(ff, rl::Load(file).paths) == 1, "(c) before: FFXIII's disc 2");
  after = RecordWithoutScan(file, ff3);  // disc 3 restarts the app into itself
  Expect(after == List({ff3, ff2, halo, bf2, ff1}), "(c) the started disc first, the set's other discs kept");
  Expect(xbox360ps5::multidisc::DefaultDisc(ff, after) == 2, "(c) FFXIII remembers disc 3, the one started");
  Expect(xbox360ps5::multidisc::DefaultDisc(bf, after) == 1, "(c) Battlefield 3 keeps its disc 2");
  // Before fix 181: the list was disc 3 alone, and Battlefield 3 fell back to disc 1.
  Expect(xbox360ps5::multidisc::DefaultDisc(bf, rl::Front(ff3, {})) == 0, "(c) the old behaviour lost BF3's disc");
  // Fix 112: a group's recency is the first line naming one of its games.
  const auto first_of = [](const List& list, const List& games) {
    for (size_t at = 0; at < list.size(); ++at)
      for (const auto& game : games) if (list[at] == game) return at;
    return list.size();
  };
  Expect(first_of(after, bf) < first_of(after, {ff1}) && first_of(after, {halo}) < first_of(after, bf),
         "(c) the groups keep their order (Halo, then BF3, then the old FFXIII disc 1)");

  // (d) Corrupt and missing files.
  std::string why;
  Put(file, halo + "\n" + std::string("bad\0line", 8) + "\n" + gta + "\n");
  after = RecordWithoutScan(file, ks, &why);
  Expect(after == List({ks}) && Has(why, "control character"), "(d) a NUL: the game alone, why: " + why);
  Expect(Get(file) == ks + "\n", "(d) and the file is a list again");
  Put(file, halo + "\n" + std::string(rl::kMaxLine + 1, 'x') + "\n");
  after = RecordWithoutScan(file, ks, &why);
  Expect(after == List({ks}) && Has(why, "line 2 is"), "(d) a line too long: the game alone, why: " + why);
  Put(file, std::string(rl::kMaxBytes + 10, 'a'));
  after = RecordWithoutScan(file, ks, &why);
  Expect(after == List({ks}) && Has(why, "bytes"), "(d) a file too large: the game alone, why: " + why);
  Put(file, std::string("\x01\x02\xFF\xFE", 4));
  after = RecordWithoutScan(file, ks, &why);
  Expect(after == List({ks}), "(d) binary junk: the game alone");
  fs::remove(file);
  const rl::Loaded none = rl::Load(file);
  Expect(none.usable && none.paths.empty(), "(d) a missing file is an empty list");
  after = RecordWithoutScan(file, ks);
  Expect(after == List({ks}) && Get(file) == ks + "\n", "(d) a missing file: the game alone, written");
  Put(file, halo + "\r\n\r\n" + gta + "\r\n" + rdr);
  const rl::Loaded crlf = rl::Load(file);
  Expect(crlf.usable && crlf.paths == List({halo, gta, rdr}), "(d) CRLF, empty lines and no last new line");
  Expect(rl::Parse("").usable && rl::Parse("").paths.empty() && rl::Parse("\n\n").paths.empty(), "(d) empty texts");
  Expect(rl::Parse(kRoot + "Pok\xC3\xA9mon \xE6\x97\xA5\xE6\x9C\xAC.iso\n").usable, "(d) UTF-8 names are paths");

  // (e) The limit.
  List many;
  std::string text;
  for (int n = 0; n < 15; ++n) {
    many.push_back(kRoot + "Game " + std::to_string(n) + ".iso");
    text += many.back() + "\n";
  }
  Put(file, text);
  after = RecordWithoutScan(file, ks);
  Expect(after.size() == rl::kLimit && rl::kLimit == 12, "(e) 12 in all");
  Expect(after.front() == ks && after[1] == many[0] && after.back() == many[10], "(e) the oldest left out");
  after = RecordWithoutScan(file, many[5]);
  Expect(after.size() == 12 && after[0] == many[5] && after[1] == ks && after[11] == many[10],
         "(e) a listed game at the limit: the list keeps 12, nothing else lost");

  // (f) Save.
  Expect(!fs::exists(file.string() + ".new"), "(f) no temporary left");
  Expect(rl::Save(file, {halo}) && Get(file) == halo + "\n", "(f) the file replaced");
  Expect(!rl::Save(folder / "missing-folder" / "recent.txt", {halo}), "(f) a write that cannot happen: false");
  Expect(Get(file) == halo + "\n", "(f) the old list is left");
  fs::remove_all(folder);
}

void TreeChecks() {
  const std::string launcher = Read("src/launcher.cpp"), header = Read("include/xbox360ps5/launcher.hpp");
  if (launcher.empty() || header.empty()) {
    std::printf("no assembled tree here; the launcher not checked\n");
    return;
  }
  Expect(Has(header, "bool recents_loaded_ = false;"), "(g) the launcher remembers whether Scan read the list");
  const size_t scan_read = launcher.find("std::ifstream input(UserData() / \"recent.txt\");");
  const size_t marked = launcher.find("recents_loaded_ = true;  // fix 181", scan_read);
  Expect(scan_read != std::string::npos && marked != std::string::npos && marked - scan_read < 300,
         "(g) Scan marks the list read");
  const size_t record = launcher.find("void Launcher::RecordLaunch(");
  const size_t end = launcher.find("\nvoid Launcher::", record + 10);
  Expect(record != std::string::npos && end != std::string::npos, "(g) RecordLaunch found");
  if (record == std::string::npos || end == std::string::npos) return;
  const std::string body = launcher.substr(record, end - record);
  const size_t check = body.find("if (!recents_loaded_) {");
  const size_t load = body.find("const auto loaded = recent_list::Load(recent_file);", check);
  const size_t usable = body.find("if (loaded.usable) {", load);
  const size_t front = body.find("recents_ = recent_list::Front(path, recents_);", usable);
  const size_t save = body.find("if (!recent_list::Save(recent_file, recents_))", front);
  Expect(check != std::string::npos && load != std::string::npos && usable != std::string::npos &&
             front != std::string::npos && save != std::string::npos,
         "(g) RecordLaunch reads the file when no scan did, then Front and Save");
  Expect(Has(body, "const fs::path recent_file = UserData() / \"recent.txt\";"), "(g) recent.txt in the data folder");
  Expect(!Has(body, "std::ofstream output(UserData() / \"recent.txt\""), "(g) RecordLaunch no longer writes the file itself");
  Expect(!Has(body, "recents.size() < 12"), "(g) the limit is recent_list's");
  Expect(Has(launcher, "#include \"xbox360ps5/recent_list.hpp\"  // fix 181"), "(g) the header included");
}
}  // namespace

int main() {
  Checks(fs::temp_directory_path() / "check-recent-list");
  TreeChecks();
  if (failures) {
    std::printf("check-recent-list: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-recent-list: all passed\n");
  return 0;
}
