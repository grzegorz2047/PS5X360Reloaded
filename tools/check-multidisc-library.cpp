// SPDX-License-Identifier: MIT
// Fix 89 (tools/source-1.0.2/fixes/89-multidisc-library.patch): the library shows the discs of
// one game as one entry (Battlefield 3: two tiles, Final Fantasy XIII: three), and the game's
// sheet chooses the disc to start.
//  (a) port/disc_catalog.cpp's GroupLibraryDiscs, with each disc's XEX execution info parsed from
//      a made-up XEX2 header (ParseXexExecutionInfo, as the launcher's probe does): an ISO set, an
//      extracted folder set, mixed names ("Disc 1", "(Disc 2)", "- Disk 3", and names without a
//      disc number that share a title ID), a single-disc game (never read), two different games
//      with similar names, a title ID of its own on each disc (same name before "Disc N"), two
//      regions of a game in one folder, ambiguous or disagreeing discs (left single), discs in two
//      folders, unreadable headers and the disc in the drive;
//  (b) NameWithoutDisc (the set's name) and DefaultDisc (the disc played last, else the first);
//  (c) the assembled tree: the launcher reads the headers, makes the sets after the scan with the
//      recent list, chooses a disc (ChooseDisc: path, format, title ID, hash), lists every disc in
//      GamePaths and for the forwarder, a running disc is chosen, the Play tab changes the disc
//      with up / down and says "Disc %u of %u", the extracted folder sets add each disc;
//  (d) every language file (argument: tools/source-1.0.2/english/lang) translates "Disc",
//      "Disc %u of %u" (two %u) and "Choose the disc".
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/disc_catalog.cpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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
  return text.str();
}

namespace md = xbox360ps5::multidisc;
using Info = ps5x360_disc::ExecutionInfo;

void Put(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v >> 24); b[at + 1] = uint8_t(v >> 16); b[at + 2] = uint8_t(v >> 8); b[at + 3] = uint8_t(v);
}
// A XEX2 header with one optional header: the execution info (0x00040006).
std::vector<uint8_t> Header(uint32_t title, uint32_t media, uint8_t disc, uint8_t count, uint32_t save = 0) {
  std::vector<uint8_t> b(56, 0);
  b[0] = 'X'; b[1] = 'E'; b[2] = 'X'; b[3] = '2';
  Put(b, 8, 56);           // header size
  Put(b, 20, 1);           // one optional header
  Put(b, 24, 0x00040006);  // execution info, at 32
  Put(b, 28, 32);
  Put(b, 32, media);
  Put(b, 36, 0x00000001);  // version
  Put(b, 44, title);
  Put(b, 52, save ? save : title);
  b[48] = 2;               // platform
  b[50] = disc;
  b[51] = count;
  return b;
}

struct Case {
  std::vector<md::LibraryDisc> discs;
  std::vector<std::vector<uint8_t>> headers;  // empty: unreadable
  std::set<size_t> probed;
  void Add(std::string family, std::string name, std::string id, std::vector<uint8_t> header) {
    discs.push_back({std::move(family), std::move(name), std::move(id)});
    headers.push_back(std::move(header));
  }
  std::vector<std::vector<size_t>> Group() {
    probed.clear();
    return md::GroupLibraryDiscs(discs, [this](size_t n) -> std::optional<Info> {
      probed.insert(n);
      const auto& h = headers[n];
      return ps5x360_disc::ParseXexExecutionInfo(h.data(), h.size()).info;
    });
  }
};
using Sets = std::vector<std::vector<size_t>>;

void CheckGrouping() {
  const std::string root = "/mnt/ext1/xbox360";
  {  // Battlefield 3, two ISOs: one title ID (45410950), a media ID per disc; given disc 2 first.
    Case c;
    c.Add(root, "Battlefield 3 (USA, Europe) (En,Fr,De,Es,It) (Disc 2).iso", "45410950", Header(0x45410950, 0x5A6071DC, 2, 2));
    c.Add(root, "Battlefield 3 (USA, Europe) (En,Fr,De,Es,It) (Disc 1).iso", "45410950", Header(0x45410950, 0x315279EB, 1, 2));
    Expect(c.Group() == Sets{{1, 0}}, "BF3's two ISOs are one set, disc 1 first");
  }
  {  // An extracted set: "Final Fantasy XIII/Disc N/default.xex"; the folder above holds it.
    Case c;
    const std::string set = root + "/Final Fantasy XIII";
    for (uint8_t d = 1; d <= 3; ++d)
      c.Add(set, "Disc " + std::to_string(d), "535107E4", Header(0x535107E4, 0x100 + d, d, 3));
    Expect(c.Group() == Sets{{0, 1, 2}}, "an extracted folder set of three discs");
  }
  {  // Mixed names of one game; names without a disc number found by their shared title ID.
    Case c;
    c.Add(root, "Final Fantasy XIII Disc 1.iso", "535107E4", Header(0x535107E4, 1, 1, 3));
    c.Add(root, "final fantasy xiii - disk 3.iso", "535107E4", Header(0x535107E4, 3, 3, 3));
    c.Add(root, "Final Fantasy XIII (Disc 2).iso", "535107E4", Header(0x535107E4, 2, 2, 3));
    Expect(c.Group() == Sets{{0, 2, 1}}, "Disc 1, (Disc 2) and - disk 3 are one set");
    Case d;
    d.Add(root + "/lo", "LO_A.iso", "4D5307FF", Header(0x4D5307FF, 1, 1, 4));
    d.Add(root + "/lo", "LO_B.iso", "4D5307FF", Header(0x4D5307FF, 2, 2, 4));
    d.Add(root + "/lo", "LO_C.iso", "", Header(0x4D5307FF, 3, 3, 4));  // title not known yet, no number in the name
    Expect(d.Group() == Sets{{0, 1}}, "names without a disc number group by the header, the shared title ID read");
    Expect(d.probed == std::set<size_t>{0, 1}, "an image with no known title ID and no disc number is not read");
  }
  {  // A single-disc game is never read; a "Disc 1" name whose header says one disc stays single.
    Case c;
    c.Add(root, "Halo 3 (USA).iso", "4D5307E6", Header(0x4D5307E6, 1, 1, 1));
    c.Add(root, "Gears of War 2.iso", "4D53082D", Header(0x4D53082D, 1, 1, 1));
    c.Add(root, "Forza 4 (Disc 1).iso", "4D530910", Header(0x4D530910, 1, 1, 1));
    Expect(c.Group().empty(), "single discs make no set");
    Expect(c.probed == std::set<size_t>{2}, "only the image with a disc number is read");
  }
  {  // Two different games with similar names, each of two discs.
    Case c;
    c.Add(root, "Mass Effect 2 (Disc 1).iso", "4541080F", Header(0x4541080F, 1, 1, 2));
    c.Add(root, "Mass Effect 3 (Disc 1).iso", "454108E6", Header(0x454108E6, 3, 1, 2));
    c.Add(root, "Mass Effect 2 (Disc 2).iso", "4541080F", Header(0x4541080F, 2, 2, 2));
    c.Add(root, "Mass Effect 3 (Disc 2).iso", "454108E6", Header(0x454108E6, 4, 2, 2));
    c.Add(root, "Lost Odyssey (Disc 1).iso", "4D5307FF", Header(0x4D5307FF, 5, 1, 2));
    c.Add(root, "Lost Odyssey Remix (Disc 2).iso", "4D530900", Header(0x4D530900, 6, 2, 2));
    Expect(c.Group() == Sets{{0, 2}, {1, 3}}, "Mass Effect 2 and 3 are two sets; two titles of different names none");
  }
  {  // A title ID of its own on each disc: grouped by the same name before "Disc N".
    Case c;
    c.Add(root, "Some Game (Disc 1).iso", "41560817", Header(0x41560817, 1, 1, 2));
    c.Add(root, "Some Game (Disc 2).iso", "41560855", Header(0x41560855, 2, 2, 2));
    Expect(c.Group() == Sets{{0, 1}}, "different title IDs, one name: one set");
    Case d;  // the name says disc 2, the header disc 1: not trusted
    d.Add(root, "Some Game (Disc 1).iso", "41560817", Header(0x41560817, 1, 1, 2));
    d.Add(root, "Some Game (Disc 2).iso", "41560855", Header(0x41560855, 2, 1, 2));
    Expect(d.Group().empty(), "a name and a header that disagree make no set");
    Case e;  // different title IDs and different disc counts
    e.Add(root, "Some Game (Disc 1).iso", "41560817", Header(0x41560817, 1, 1, 2));
    e.Add(root, "Some Game (Disc 2).iso", "41560855", Header(0x41560855, 2, 2, 3));
    Expect(e.Group().empty(), "different disc counts make no set");
  }
  {  // Two regions of one game (one title ID) in one folder: told apart by the name.
    Case c;
    for (uint8_t d = 1; d <= 3; ++d) {
      c.Add(root, "Final Fantasy XIII (USA) (Disc " + std::to_string(d) + ").iso", "535107E4", Header(0x535107E4, d, d, 3));
      c.Add(root, "Final Fantasy XIII (Europe) (Disc " + std::to_string(d) + ").iso", "535107E4", Header(0x535107E4, 10 + d, d, 3));
    }
    Expect(c.Group() == Sets{{0, 2, 4}, {1, 3, 5}}, "two regions are two sets");
    Case d;  // two copies of disc 1 under one name: ambiguous, every disc stays single
    d.Add(root, "Battlefield 3 (Disc 1).iso", "45410950", Header(0x45410950, 1, 1, 2));
    d.Add(root, "Battlefield 3 [Disc 1].iso", "45410950", Header(0x45410950, 1, 1, 2));
    d.Add(root, "Battlefield 3 (Disc 2).iso", "45410950", Header(0x45410950, 2, 2, 2));
    Expect(d.Group().empty(), "two discs 1 of one name make no set");
  }
  {  // Discs in two folders (internal and USB) are not one set; unreadable headers make none.
    Case c;
    c.Add("/mnt/ext1/xbox360", "Battlefield 3 (Disc 1).iso", "45410950", Header(0x45410950, 1, 1, 2));
    c.Add("/mnt/usb0/xbox360", "Battlefield 3 (Disc 2).iso", "45410950", Header(0x45410950, 2, 2, 2));
    Expect(c.Group().empty(), "discs in two folders are separate");
    Case d;
    d.Add(root, "Battlefield 3 (Disc 1).iso", "45410950", {});
    d.Add(root, "Battlefield 3 (Disc 2).iso", "45410950", Header(0x45410950, 2, 2, 2));
    Expect(d.Group().empty(), "an unreadable disc 1 makes no set");
    Case e;  // the disc in the drive: no folder, never read
    e.Add("", "Battlefield 3 (Disc 1)", "45410950", Header(0x45410950, 1, 1, 2));
    e.Add(root, "Battlefield 3 (Disc 2).iso", "45410950", Header(0x45410950, 2, 2, 2));
    Expect(e.Group().empty() && e.probed == std::set<size_t>{1}, "the disc in the drive is not grouped");
    Case f;  // an incomplete set (disc 3 missing) is still one entry with the discs there
    f.Add(root, "Lost Odyssey (Disc 1).iso", "4D5307FF", Header(0x4D5307FF, 1, 1, 4));
    f.Add(root, "Lost Odyssey (Disc 2).iso", "4D5307FF", Header(0x4D5307FF, 2, 2, 4));
    f.Add(root, "Lost Odyssey (Disc 4).iso", "4D5307FF", Header(0x4D5307FF, 4, 4, 4));
    Expect(f.Group() == Sets{{0, 1, 2}}, "the discs that are there make the set");
    Case g;  // no probe: nothing is grouped
    g.Add(root, "Battlefield 3 (Disc 1).iso", "45410950", Header(0x45410950, 1, 1, 2));
    g.Add(root, "Battlefield 3 (Disc 2).iso", "45410950", Header(0x45410950, 2, 2, 2));
    Expect(md::GroupLibraryDiscs(g.discs, {}).empty(), "without a probe no set is made");
  }
}

void CheckNames() {
  Expect(md::NameWithoutDisc("Battlefield 3 (Disc 1)") == "Battlefield 3", "the cached title's (Disc 1)");
  Expect(md::NameWithoutDisc("Battlefield 3 (USA, Europe) (En,Fr,De,Es,It) (Disc 2)") ==
             "Battlefield 3 (USA, Europe) (En,Fr,De,Es,It)", "a file name keeps its region");
  Expect(md::NameWithoutDisc("Final Fantasy XIII - Disk 3") == "Final Fantasy XIII", "- Disk 3");
  Expect(md::NameWithoutDisc("Lost Odyssey Disc 4.iso") == "Lost Odyssey", "an image name");
  Expect(md::NameWithoutDisc("Juego [Disco 2]") == "Juego", "[Disco 2]");
  Expect(md::NameWithoutDisc("Game_Disc1") == "Game", "Game_Disc1");
  Expect(md::NameWithoutDisc("Battlefield 3") == "Battlefield 3", "a name without a disc");
  Expect(md::NameWithoutDisc("Halo 3") == "Halo 3", "a number is no disc");
  Expect(md::NameWithoutDisc("Discovery 2") == "Discovery 2", "Discovery is no disc");
  Expect(md::NameWithoutDisc("Disc 1") == "Disc 1", "a folder named only Disc 1 keeps its name");

  const std::vector<std::string> discs = {"/g/FF13 (Disc 1).iso", "/g/FF13 (Disc 2).iso", "/g/FF13 (Disc 3).iso"};
  Expect(md::DefaultDisc(discs, {}) == 0, "never played: disc 1");
  Expect(md::DefaultDisc(discs, {"/g/Halo 3.iso", "/g/FF13 (Disc 2).iso", "/g/FF13 (Disc 1).iso"}) == 1,
         "the disc played last");
  Expect(md::DefaultDisc(discs, {"/g/FF13 (Disc 3).iso"}) == 2, "disc 3 played last");
  Expect(md::DefaultDisc(discs, {"/g/Halo 3.iso"}) == 0, "another game played: disc 1");
}

size_t Find(const std::string& text, const std::string& what, size_t from = 0) { return text.find(what, from); }
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

void CheckTree() {
  const std::string header = Read("include/xbox360ps5/launcher.hpp");
  Expect(Has(header, "std::vector<GameEntry> discs;") && Has(header, "size_t chosen_disc = 0;"),
         "GameEntry has its discs");
  Expect(Has(header, "std::vector<GameEntry> EntriesWithDiscs() const;"), "the launcher lists every disc");
  const std::string covers = Read("src/covers.cpp");
  Expect(Has(covers, "std::vector<uint8_t> ReadGameXexHeader(const fs::path& game)") &&
             Has(covers, "extension == \".iso\" ? IsoXexHeader(game) : extension == \".xex\" ? XexHeader(game)"),
         "the header of an ISO's or a folder's executable is read");
  const std::string library = Read("port/disc_library.cpp");
  Expect(Has(library, "result.launch_paths.push_back(original);"), "a checked folder set gives every disc");

  const std::string launcher = Read("src/launcher.cpp");
  const size_t folders = Find(launcher, "for (const auto& launch : group->launch_paths)");
  Expect(folders != std::string::npos && Has(launcher, "add({directory.filename().string(), launch, \"XEX\", directory.string()}, {});"),
         "a folder set adds each disc");
  Expect(!Has(launcher, "\" discs\";"), "no untranslated \"N discs\" size");
  const size_t recents = Find(launcher, "for (std::string line; std::getline(input, line);) if (!line.empty()) recents_.push_back(line);");
  const size_t group = Find(launcher, "multidisc::GroupLibraryDiscs(found, ", recents);
  const size_t probe = Find(launcher, "ReadGameXexHeader(games_[n].path)", group);
  const size_t parse = Find(launcher, "ps5x360_disc::ParseXexExecutionInfo(header.data(), header.size())", probe);
  const size_t chosen = Find(launcher, "multidisc::DefaultDisc(paths, recents_)", parse);
  const size_t choose = Find(launcher, "ChooseDisc(entry, chosen);", chosen);
  const size_t order = Find(launcher, "std::stable_sort(games_.begin(), games_.end()", choose);
  Expect(recents != std::string::npos && group != std::string::npos && probe != std::string::npos &&
             parse != std::string::npos && chosen != std::string::npos && choose != std::string::npos &&
             order != std::string::npos,
         "the sets are made after the recent list is read and before the order");
  Expect(Has(launcher, "if (game.folder == (kStorage / \"disc\").string()) continue;  // a disc in the drive"),
         "the disc in the drive is left alone");
  Expect(Has(launcher, "const std::string name = multidisc::NameWithoutDisc(named->name);"), "the set's name");
  const size_t chooser = Find(launcher, "void ChooseDisc(GameEntry& game, size_t index) {");
  const size_t chooser_end = Find(launcher, "bool HasDiscPath(", chooser);
  const std::string body = chooser == std::string::npos ? std::string() : launcher.substr(chooser, chooser_end - chooser);
  for (const char* field : {"game.path = disc.path;", "game.kind = disc.kind;", "game.title_id = disc.title_id;",
                            "game.hash = disc.hash;", "game.disc = disc.disc;", "game.chosen_disc = index;"})
    Expect(Has(body, field), std::string("ChooseDisc sets ") + field);
  Expect(!Has(body, "game.name") && !Has(body, "game.cover"), "ChooseDisc keeps the set's name and cover");
  Expect(Has(launcher, "for (const auto& disc : game.discs) paths.push_back(disc.path);  // fix 89: every disc of a set"),
         "GamePaths lists every disc");
  Expect(Has(launcher, "if (HasDiscPath(games_[size_t(view_[size_t(n)])], keep)) selected_ = n;"),
         "the selection is kept by any disc's path");
  Expect(Has(launcher, "if (HasDiscPath(game, recents_[at])) best = at;"), "a set's place: its most recent disc");
  Expect(Has(launcher, "if (SyncRunningDisc(games_)) SelectionChanged();  // fix 89"), "a running disc is chosen");
  const size_t play = Find(launcher, "if (key == Key::right) game_tab_ = 1;");
  const size_t step = Find(launcher, "StepDisc(key == Key::down ? 1 : -1);", play);
  Expect(play != std::string::npos && step != std::string::npos && step - play < 200,
         "the Play tab changes the disc with up / down");
  Expect(Has(launcher, "SelectionChanged();  // its patches and own settings, by that disc's title ID and hash"),
         "a changed disc reloads its settings and patches");
  const size_t row = Find(launcher, "c.Text(Tr(\"Disc\"), x + 64, 464");
  const size_t of = Find(launcher, "Tr(\"Disc %u of %u\"), game.disc, unsigned(game.discs.size())", row);
  Expect(row != std::string::npos && of != std::string::npos, "the Play tab's disc row");
  Expect(Has(launcher, "hint = c.Hint('v', Tr(\"Choose the disc\"), hint, 1004);"), "the disc hint");
  const std::string main = Read("src/canary_game_main.cpp");
  Expect(Has(main, "const auto entries = launcher.EntriesWithDiscs();"), "a forwarder finds any disc");
}

void CheckLanguages(const std::string& folder) {
  namespace fs = std::filesystem;
  int files = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++files;
    const std::string text = "\n" + Read(entry.path().string());
    const std::string name = entry.path().filename().string();
    for (const auto& [key, specifiers] : std::vector<std::pair<std::string, int>>{
             {"Disc", 0}, {"Disc\\s%u\\sof\\s%u", 2}, {"Choose\\sthe\\sdisc", 0}}) {
      const size_t at = text.find("\n" + key + " = ");
      Expect(at != std::string::npos, name + " translates " + key);
      if (at == std::string::npos) continue;
      const size_t start = at + key.size() + 4;
      const std::string value = text.substr(start, text.find('\n', start) - start);
      Expect(!value.empty(), name + ": " + key + " has a text");
      int count = 0, u = 0;
      for (size_t p = value.find('%'); p != std::string::npos; p = value.find('%', p + 1)) {
        ++count;
        u += value.compare(p, 2, "%u") == 0;
      }
      Expect(count == specifiers && u == specifiers, name + ": " + key + " needs " + std::to_string(specifiers) + " %u: " + value);
    }
  }
  Expect(files == 16, "16 language files (" + std::to_string(files) + ")");
}

}  // namespace

int main(int argc, char** argv) {
  CheckGrouping();
  CheckNames();
  CheckTree();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "the language folder is given");
  if (failures) {
    std::printf("check-multidisc-library: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-multidisc-library: ok\n");
  return 0;
}
