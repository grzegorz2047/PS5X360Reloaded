// SPDX-License-Identifier: MIT
// Fix 112 (tools/source-1.0.2/fixes/112-library-groups.patch): the player's own folders in a
// game folder are groups of games on the library's shelf.
//  (a) port/library_groups.hpp on made-up entries: games at the top of a game folder beside two
//      groups; a folder holding one game (an image in a folder of its own, an extracted game's
//      folder) is no group; nested groups and their trail; a folder whose games are all in one
//      subfolder passes through; a group that would be alone at the top is the top; a disc set (one
//      entry, fix 89) inside a group; a package's title ID and content type folders passed through;
//      the same folder in two game folders is one group; FoldersBelow's edges; Inside; StillThere;
//  (b) the same rules on real folders in a temporary directory, walked the way Launcher::Scan
//      walks them (default.xex makes a folder a game, .iso images, subfolders six deep, empty
//      folders), placed with EntryFolders;
//  (c) the assembled tree: the scan records each game's game folder and builds the groups after
//      the order, the view holds groups as -1 - n, the shelf draws Tile(n), X opens a group,
//      Circle leaves it, the game sheet and game settings are only for games, a game shown from
//      elsewhere (the home's slots, the per-game list) opens in its group;
//  (d) every language file (argument: tools/source-1.0.2/english/lang) translates "Group" and the
//      four forms of a group's number of games (fix 112b: "%d game", "%d games", "||few", "||one").
// Fix 112b: a group's tile is a folder (DrawGroupFolder: a mosaic of up to four covers, the name, a
// badge with the number), never one game's cover; CountKey picks the number's form.
// Fix 112d: copies of one game (one title ID) count once, so a folder of one game is never a group;
// a package in a title update's or DLC's content type folder is not read (GamePackageFolder); the
// console's Halo folders (one game each beside its DLC and title updates) are no groups.
// Fix 112e: ReadDiscRoot on made-up XDVDFS images (default.xbe only: an original Xbox disc, left
// out; default.xex, both or neither: kept; no XDVDFS, a cut root, a short or missing file: kept).
// Fix 112f: MergeCopies (identical copies on ext1 and usb0: one tile on ext1; another size or title
// ID: two tiles; the console's storage first; unknown title IDs and sizes apart), DriveOf, DriveRank,
// and the launcher keeping the other copies for GamePaths, the forwarder and the recent list.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/library_groups.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
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
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

namespace lg = xbox360ps5::library_groups;
using Folders = std::vector<std::vector<std::string>>;

std::string Join(const std::vector<std::string>& parts) {
  std::string text;
  for (const auto& part : parts) text += (text.empty() ? "" : "/") + part;
  return text;
}
const lg::Group* GroupOf(const lg::Layout& layout, const std::string& key) {
  const int at = layout.Find(key);
  return at < 0 ? nullptr : &layout.groups[size_t(at)];
}

void CheckBuild() {
  {
    // Two games at the top, a "Racing" group of three, a "Shooters" group of two.
    const Folders folders = {{}, {"Racing"}, {"Racing"}, {"Shooters"}, {}, {"Racing"}, {"Shooters"}};
    const auto layout = lg::Build(folders);
    Expect(layout.groups.size() == 2, "the top with two groups: two groups");
    const auto* racing = GroupOf(layout, "racing");
    const auto* shooters = GroupOf(layout, "shooters");
    Expect(racing && racing->name == "Racing" && racing->parent.empty() && racing->games == std::vector<size_t>{1, 2, 5},
           "Racing holds its three games, at the top");
    Expect(shooters && shooters->games == std::vector<size_t>{3, 6}, "Shooters holds its two games");
    Expect(layout.shown_in[0].empty() && layout.shown_in[4].empty(), "the games at the top stay at the top");
    Expect(layout.shown_in[1] == "racing" && layout.shown_in[6] == "shooters", "the others are in their groups");
    Expect(racing && racing->trail == std::vector<std::string>{"Racing"}, "a top group's trail is its name");
  }
  {
    // A game in a folder of its own is no group: "Halo 3/Halo 3.iso" stays a tile at the top.
    const Folders folders = {{"Halo 3"}, {"Fable II"}, {"Racing"}, {"Racing"}};
    const auto layout = lg::Build(folders);
    Expect(layout.groups.size() == 1 && GroupOf(layout, "racing"), "a folder of one game is no group");
    Expect(layout.shown_in[0].empty() && layout.shown_in[1].empty(), "a game in its own folder is shown at the top");
  }
  {
    // Nested: Racing/Rally (two) and Racing/Forza Motorsport 4 (one game in its own folder).
    const Folders folders = {{"Racing", "Rally"}, {"Racing", "Rally"}, {"Racing", "Forza Motorsport 4"}, {"Racing"}, {}};
    const auto layout = lg::Build(folders);
    const auto* racing = GroupOf(layout, "racing");
    const auto* rally = GroupOf(layout, "racing/rally");
    Expect(layout.groups.size() == 2 && racing && rally, "a group inside a group");
    Expect(rally && rally->parent == "racing" && rally->name == "Rally", "Rally is shown inside Racing");
    Expect(rally && rally->trail == std::vector<std::string>{"Racing", "Rally"}, "Rally's trail");
    Expect(racing && racing->games == std::vector<size_t>{0, 1, 2, 3}, "Racing holds its groups' games too");
    Expect(layout.shown_in[2] == "racing" && layout.shown_in[3] == "racing", "a one-game subfolder's game is in Racing");
    Expect(layout.shown_in[0] == "racing/rally", "Rally's games are in Rally");
    Expect(layout.Inside(0, "racing") && layout.Inside(0, "racing/rally") && layout.Inside(0, ""),
           "a Rally game is inside Racing, Rally and the library");
    Expect(!layout.Inside(4, "racing") && !layout.Inside(3, "racing/rally"), "and nothing else is");
    Expect(lg::Layout{{}, {"racing2"}}.Inside(0, "racing") == false, "a longer folder name is not inside");
    // Three levels: every level that holds two games or more is a group.
    const Folders deep = {{"A", "B", "C"}, {"A", "B", "C"}, {"A", "B"}, {"A"}, {}};
    const auto three = lg::Build(deep);
    Expect(three.groups.size() == 3 && GroupOf(three, "a/b/c") && GroupOf(three, "a/b")->parent == "a",
           "groups nest three deep");
  }
  {
    // A folder whose games are all in one subfolder passes through: Collection/Shooters/(two games).
    const Folders folders = {{"Collection", "Shooters"}, {"Collection", "Shooters"}, {}};
    const auto layout = lg::Build(folders);
    Expect(layout.groups.size() == 1 && GroupOf(layout, "collection/shooters"), "only Shooters is a group");
    const auto* shooters = GroupOf(layout, "collection/shooters");
    Expect(shooters && shooters->parent.empty() && shooters->trail == std::vector<std::string>{"Shooters"},
           "it is shown at the top");
  }
  {
    // Every game in one folder ("xbox360/games/..."): that folder would be alone at the top.
    const Folders folders = {{"games"}, {"games"}, {"games", "Racing"}, {"games", "Racing"}};
    const auto layout = lg::Build(folders);
    Expect(!GroupOf(layout, "games") && GroupOf(layout, "games/racing"), "a lone top group is the top itself");
    Expect(layout.shown_in[0].empty() && layout.shown_in[2] == "games/racing", "its games and groups are at the top");
    Expect(GroupOf(layout, "games/racing")->parent.empty(), "Racing is a top group then");
    const auto single = lg::Build({{"games"}, {"games"}});
    Expect(single.groups.empty() && single.shown_in[0].empty(), "a library in one folder has no groups");
  }
  {
    // A disc set is one entry (fix 89): alone in its folder it is no group; in "Shooters" beside
    // another game it is one of the group's two games.
    const Folders folders = {{"Battlefield 3"}, {"Shooters"}, {"Shooters"}};
    const auto layout = lg::Build(folders);
    Expect(layout.shown_in[0].empty(), "a set alone in its folder is shown at the top");
    const auto* shooters = GroupOf(layout, "shooters");
    Expect(shooters && shooters->games.size() == 2, "a set in a group counts as one game");
  }
  {
    // GOD/STFS: "Racing/<game>/4D5307E6/00007000/<package>" - the hex folders are the package's.
    const Folders folders = {{"Racing", "Forza 4", "4D5307E6", "00007000"}, {"Racing", "Forza 3", "4D5307D5", "00007000"},
                             {"Halo", "4D5307E6", "00007000"}, {"Halo", "4D5307E6", "00007000"}};
    const auto layout = lg::Build(folders);
    Expect(!GroupOf(layout, "racing/forza 4") && GroupOf(layout, "racing"), "a package in a game folder is no group");
    Expect(layout.shown_in[0] == "racing" && layout.shown_in[1] == "racing", "the packages are in Racing");
    for (const auto& group : layout.groups)
      Expect(Has(group.key, "00007000") == false && Has(group.key, "4d5307e6") == false, "no hex folder is a group: " + group.key);
    Expect(GroupOf(layout, "halo") && GroupOf(layout, "halo")->games.size() == 2, "two discs' packages in Halo: a group");
    Expect(lg::PackageFolder("00007000") && lg::PackageFolder("4d5307E6") && !lg::PackageFolder("Halo 3") &&
               !lg::PackageFolder("0000700") && !lg::PackageFolder("0000700G"),
           "PackageFolder: eight hex digits");
  }
  {
    // The same folder in two game folders is one group (case aside), named as found first.
    const Folders folders = {{"Racing"}, {"racing"}, {"RACING"}, {}};
    const auto layout = lg::Build(folders);
    Expect(layout.groups.size() == 1 && GroupOf(layout, "racing") && GroupOf(layout, "racing")->name == "Racing" &&
               GroupOf(layout, "racing")->games.size() == 3,
           "one Racing for three spellings");
  }
  {
    // Nothing found: no groups; empty folder lists are the top.
    const auto none = lg::Build({});
    Expect(none.groups.empty() && none.shown_in.empty(), "an empty library has no groups");
    const auto flat = lg::Build({{}, {}, {}});
    Expect(flat.groups.empty() && flat.shown_in == std::vector<std::string>{"", "", ""}, "games at the top only");
  }
  {
    const auto layout = lg::Build({{"Racing", "Rally"}, {"Racing", "Rally"}, {"Racing"}, {}});
    Expect(lg::StillThere(layout, "racing/rally") == "racing/rally", "StillThere: a group that is there");
    Expect(lg::StillThere(layout, "racing/rally/old") == "racing/rally", "StillThere: its nearest group");
    Expect(lg::StillThere(layout, "gone/away") == "", "StillThere: else the library");
  }
  {
    // Fix 112d: copies of one game count once. "halo ce" on two drives (one group key), each with
    // the same game: one game, no group, both copies at the top; with a second game it is a group.
    const Folders folders = {{"halo ce"}, {"halo ce"}, {"Kinect"}, {"Kinect"}, {}};
    const std::vector<std::string> ids = {"4D5309B1", "4D5309B1", "4D5308D6", "4E4D07D1", "4D530919"};
    const auto layout = lg::Build(folders, ids);
    Expect(!GroupOf(layout, "halo ce") && layout.shown_in[0].empty() && layout.shown_in[1].empty(),
           "two copies of Halo in folders named halo ce: no group, both at the top");
    Expect(GroupOf(layout, "kinect") && GroupOf(layout, "kinect")->games.size() == 2, "two Kinect games: a group");
    const auto mixed = lg::Build({{"halo ce"}, {"halo ce"}, {"halo ce"}, {}}, {"4D5309B1", "4D5309B1", "4D53085B", "4D530919"});
    Expect(GroupOf(mixed, "halo ce") && GroupOf(mixed, "halo ce")->games.size() == 3, "two games (one in two copies): a group of three");
    const auto unnamed = lg::Build({{"A"}, {"A"}, {}}, {"", "", ""});
    Expect(GroupOf(unnamed, "a"), "entries without a title ID are games of their own");
    // In a subfolder: Racing holds Forza twice and nothing else, so it passes on as one game.
    const auto nested = lg::Build({{"Racing", "Forza"}, {"Racing"}, {}}, {"4D5307E6", "4D5307E6", "4D530919"});
    Expect(!GroupOf(nested, "racing") && !GroupOf(nested, "racing/forza") && nested.shown_in[0].empty() &&
               nested.shown_in[1].empty(),
           "a folder with one game in two copies, a subfolder's included: no group");
  }
  // Fix 112d: a package's content type folder that holds no game is not read.
  for (const char* type : {"00007000", "000D0000", "000d0000", "00080000", "00004000", "00005000", "02000000"})
    Expect(lg::GamePackageFolder(type), std::string("GamePackageFolder: a game's content type ") + type);
  for (const char* type : {"000B0000", "00000002", "00000001", "00009000", "00020000", "00030000", "00090000", "4D5309B1"})
    Expect(!lg::GamePackageFolder(type), std::string("GamePackageFolder: no game in ") + type);
  for (const char* folder : {"halo ce", "Racing", "Games", "4D5309B", "ABCDEFGH"})
    Expect(lg::GamePackageFolder(folder), std::string("GamePackageFolder: the player's folder ") + folder);
  // Fix 112b: the number's form.
  Expect(std::string(lg::CountKey(1)) == "%d game" && std::string(lg::CountKey(2)) == "%d games||few" &&
             std::string(lg::CountKey(4)) == "%d games||few" && std::string(lg::CountKey(5)) == "%d games" &&
             std::string(lg::CountKey(11)) == "%d games" && std::string(lg::CountKey(12)) == "%d games" &&
             std::string(lg::CountKey(14)) == "%d games" && std::string(lg::CountKey(21)) == "%d games||one" &&
             std::string(lg::CountKey(22)) == "%d games||few" && std::string(lg::CountKey(111)) == "%d games" &&
             std::string(lg::CountKey(112)) == "%d games" && std::string(lg::CountKey(101)) == "%d games||one" &&
             std::string(lg::CountKey(0)) == "%d games",
         "CountKey: 1 / 2-4, 22-24 / 21, 31 / the rest");
  // FoldersBelow and EntryFolders.
  Expect(lg::FoldersBelow("/mnt/ext1/xbox360", "/mnt/ext1/xbox360/Racing/Rally") ==
             std::vector<std::string>{"Racing", "Rally"}, "FoldersBelow: two folders");
  Expect(lg::FoldersBelow("/mnt/ext1/xbox360/", "/mnt/ext1/xbox360/Racing/") == std::vector<std::string>{"Racing"},
         "FoldersBelow: slashes at the end");
  Expect(lg::FoldersBelow("/mnt/ext1/xbox360", "/mnt/ext1/xbox360").empty(), "FoldersBelow: the root itself");
  Expect(lg::FoldersBelow("/data/xbox360", "/data/xbox3600/Racing").empty(), "FoldersBelow: a longer name is not below");
  Expect(lg::FoldersBelow("/data/xbox360", "/mnt/usb0/xbox360/Racing").empty(), "FoldersBelow: another drive");
  Expect(lg::FoldersBelow("/", "/Racing/Rally") == std::vector<std::string>{"Racing", "Rally"}, "FoldersBelow: below /");
  Expect(lg::EntryFolders("/r", "ISO", "/r/Racing/Forza.iso", "/r/Racing") == std::vector<std::string>{"Racing"},
         "EntryFolders: an image is held by its folder");
  Expect(lg::EntryFolders("/r", "XEX", "/r/Racing/Forza/default.xex", "/r/Racing/Forza") == std::vector<std::string>{"Racing"},
         "EntryFolders: an extracted game is its folder");
  Expect(lg::EntryFolders("/r", "XEX", "/r/Mass Effect 3/Disc 1/default.xex", "/r/Mass Effect 3").empty(),
         "EntryFolders: an extracted set is its set's folder");
  Expect(lg::EntryFolders("/r", "XEX", "/r/default.xex", "/r").empty(), "EntryFolders: a game at the top of the root");
  Expect(lg::EntryFolders("/r", "GOD/STFS", "/r/Halo/4D5307E6/00007000/ABCD", "/r/Halo/4D5307E6/00007000") ==
             std::vector<std::string>{"Halo", "4D5307E6", "00007000"},
         "EntryFolders: a package is held by its folder");
}

// (b) Real folders, walked as Launcher::Scan walks them.
struct Found {
  std::string kind, path, folder;
};
std::string Lower(std::string text) {
  for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
  return text;
}
std::vector<Found> Walk(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  std::vector<Found> found;
  const std::function<void(const fs::path&, int)> scan = [&](const fs::path& directory, int depth) {
    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(directory)) entries.push_back(entry);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.path() < b.path(); });
    fs::path executable;
    int executables = 0;
    for (const auto& entry : entries) {
      if (Lower(entry.path().extension().string()) != ".xex" || entry.is_directory()) continue;
      ++executables;
      if (Lower(entry.path().filename().string()) == "default.xex") { executable = entry.path(); executables = 1; break; }
      executable = entry.path();
    }
    if (executables == 1) {
      found.push_back({"XEX", executable.generic_string(), directory.generic_string()});
      if (directory != root) return;
    }
    // A disc set (fix 89: "<set>/Disc 1/default.xex", "<set>/Disc 2/..."): one entry, its set's folder.
    if (fs::is_directory(directory / "Disc 1") && fs::is_directory(directory / "Disc 2")) {
      found.push_back({"XEX", (directory / "Disc 1" / "default.xex").generic_string(), directory.generic_string()});
      return;
    }
    std::vector<fs::path> images;
    for (const auto& entry : entries) {
      const std::string extension = Lower(entry.path().extension().string());
      if (entry.is_directory()) {
        if (depth < 6 && extension != ".data") scan(entry.path(), depth + 1);
      } else if (extension == ".iso") {
        // Fix 89's ISO sets: "... (Disc N).iso" of one name in one folder are one entry.
        const std::string name = entry.path().filename().string();
        if (Has(name, "(Disc ") && !Has(name, "(Disc 1)")) continue;
        found.push_back({"ISO", entry.path().generic_string(), directory.generic_string()});
      } else if (extension.empty() || extension == ".xbla" || extension == ".god") {
        // Fix 112d: a package is read only in a folder that may hold a game's (GamePackageFolder);
        // here every package read is taken for a game, as ReadPackage takes a GOD package.
        if (!lg::GamePackageFolder(directory.filename().string())) continue;
        found.push_back({"GOD/STFS", entry.path().generic_string(), directory.generic_string()});
      }
    }
  };
  scan(root, 0);
  return found;
}

// Fix 112c: the mosaic of a group's folder. Each cover keeps its own proportions (within 1%), stays
// in the mosaic's area with margins, clear of the name and the badge, and no two overlap; 1 to 5
// covers (the fifth not shown), three folder shapes, real and unknown cover shapes.
void CheckMosaic() {
  const auto overlap = [](const lg::Cell& a, const lg::Cell& b) {
    return a.u0 < b.u1 && b.u0 < a.u1 && a.v0 < b.v1 && b.v0 < a.v1;
  };
  for (const float folder : {385.0f / 545.0f, 0.55f, 1.25f}) {
    for (size_t games = 1; games <= 5; ++games) {
      for (const float shape : {0.70f, 0.0f, 1.0f, 0.62f, 3.0f}) {
        std::vector<float> aspects(games, shape);
        if (games >= 2) aspects[1] = 0.75f;  // covers of different shapes in one folder
        const auto cells = lg::MosaicCells(aspects, folder);
        const std::string what = "mosaic: " + std::to_string(games) + " games, folder " + std::to_string(folder) +
                                 ", cover " + std::to_string(shape);
        Expect(cells.size() == std::min<size_t>(games, 4), what + ": up to four covers");
        for (size_t i = 0; i < cells.size(); ++i) {
          const auto& m = cells[i];
          const float wanted = aspects[i] > 0.05f && aspects[i] < 20.0f ? aspects[i] : lg::kDefaultCoverAspect;
          const float drawn = (m.u1 - m.u0) * folder / (m.v1 - m.v0);
          Expect(std::fabs(drawn / wanted - 1.0f) < 0.01f, what + ": cover " + std::to_string(i) + " keeps its proportions");
          Expect(m.u0 >= lg::kMosaicArea.u0 - 1e-4f && m.u1 <= lg::kMosaicArea.u1 + 1e-4f &&
                     m.v0 >= lg::kMosaicArea.v0 - 1e-4f && m.v1 <= lg::kMosaicArea.v1 + 1e-4f,
                 what + ": cover " + std::to_string(i) + " inside the mosaic's area");
          Expect(m.u0 > 0.05f && m.u1 < 0.95f && m.v0 > 0.14f, what + ": yellow margins around it");
          Expect(!overlap(m, lg::kFolderLabel) && !overlap(m, lg::kFolderBadge), what + ": clear of the name and the badge");
          for (size_t j = 0; j < i; ++j) Expect(!overlap(m, cells[j]), what + ": covers do not overlap");
        }
      }
    }
  }
  // The console's tile: a folder of 385x545 px, two Xbox 360 covers about 110x157 px each.
  const auto two = lg::MosaicCells({0.70f, 0.70f}, 385.0f / 545.0f);
  for (const auto& m : two) {
    const float w = (m.u1 - m.u0) * 385.0f, h = (m.v1 - m.v0) * 545.0f;
    Expect(w > 95.0f && w < 125.0f && h > 135.0f && h < 180.0f,
           "two covers about 110x157 px (" + std::to_string(int(w)) + "x" + std::to_string(int(h)) + ")");
  }
  Expect(two.size() == 2 && std::fabs((two[0].v0 + two[0].v1) / 2 - (two[1].v0 + two[1].v1) / 2) < 1e-4f &&
             std::fabs((two[0].u0 + two[1].u1) / 2 - 0.5f) < 1e-3f,
         "two covers side by side, centred");
  Expect(lg::MosaicCells({}, 0.7f).empty(), "no cover, no cell");
  Expect(lg::MosaicCells({0.7f}, 0.0f).size() == 1, "an unknown folder shape still lays out");
}

// Fix 112e: a made-up XDVDFS image: the volume descriptor at sector 32 of `base` and a root folder
// whose entries (a binary tree, 4-byte aligned) name `names`.
void WriteXdvdfs(const std::filesystem::path& path, const std::vector<std::string>& names, uint64_t base = 0,
                 bool magic = true) {
  std::vector<uint8_t> root;
  for (size_t i = 0; i < names.size(); ++i) {
    const size_t at = root.size();
    const size_t length = 14 + names[i].size();
    const size_t next = (at + length + 3) / 4 * 4;
    root.resize(next, 0xFF);
    const uint16_t right = i + 1 < names.size() ? uint16_t(next / 4) : 0;  // each entry's right: the next one
    root[at] = 0; root[at + 1] = 0;                                          // no left
    root[at + 2] = uint8_t(right); root[at + 3] = uint8_t(right >> 8);
    for (int b = 4; b < 13; ++b) root[at + b] = 0;
    root[at + 12] = 0x20;  // a file
    root[at + 13] = uint8_t(names[i].size());
    std::memcpy(&root[at + 14], names[i].data(), names[i].size());
  }
  if (root.empty()) root.assign(4, 0xFF);
  root.resize((root.size() + 2047) / 2048 * 2048, 0xFF);
  std::vector<uint8_t> image(size_t(base) + 40 * 2048 + root.size(), 0);
  uint8_t* volume = &image[size_t(base) + 32 * 2048];
  if (magic) std::memcpy(volume, "MICROSOFT*XBOX*MEDIA", 20);
  const uint32_t root_sector = 40, root_size = uint32_t(root.size());
  for (int b = 0; b < 4; ++b) { volume[20 + b] = uint8_t(root_sector >> (8 * b)); volume[24 + b] = uint8_t(root_size >> (8 * b)); }
  std::memcpy(&image[size_t(base) + 40 * 2048], root.data(), root.size());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
}

void CheckDiscRoot() {
  namespace fs = std::filesystem;
  const fs::path folder = fs::temp_directory_path() / ("check-disc-root-" + std::to_string(std::random_device{}()));
  fs::create_directories(folder);
  const auto kind = [&](const std::string& name, const std::vector<std::string>& names, uint64_t base = 0, bool magic = true) {
    const fs::path path = folder / name;
    WriteXdvdfs(path, names, base, magic);
    return lg::ReadDiscRoot(path.string());
  };
  using lg::DiscRoot;
  Expect(kind("xbe.iso", {"default.xbe", "media"}) == DiscRoot::original_xbox, "disc root: default.xbe only -> original Xbox");
  Expect(kind("xbe-upper.iso", {"$SystemUpdate", "DEFAULT.XBE"}) == DiscRoot::original_xbox, "disc root: DEFAULT.XBE in capitals");
  Expect(kind("xex.iso", {"default.xex", "media"}) == DiscRoot::xbox360, "disc root: default.xex -> a game");
  Expect(kind("both.iso", {"default.xbe", "default.xex"}) == DiscRoot::xbox360, "disc root: both -> a game");
  Expect(kind("deep.iso", {"a", "b", "c", "d", "e", "default.xbe"}) == DiscRoot::original_xbox, "disc root: found down the tree");
  Expect(kind("neither.iso", {"media", "readme.txt"}) == DiscRoot::xbox360, "disc root: neither -> kept as today");
  Expect(kind("no-magic.iso", {"default.xbe"}, 0, false) == DiscRoot::unreadable, "disc root: no XDVDFS -> unreadable, kept");
  Expect(kind("xgd3.iso", {"default.xbe"}, 0x2080000) == DiscRoot::original_xbox, "disc root: at the XGD3 offset too");
  {
    std::ofstream(folder / "short.iso", std::ios::binary) << "not an image";
    Expect(lg::ReadDiscRoot((folder / "short.iso").string()) == DiscRoot::unreadable, "disc root: a short file -> unreadable");
  }
  {
    // A root folder beyond the end of the file: unreadable, kept.
    WriteXdvdfs(folder / "cut.iso", {"default.xbe"});
    fs::resize_file(folder / "cut.iso", 40 * 2048 + 10);
    Expect(lg::ReadDiscRoot((folder / "cut.iso").string()) == DiscRoot::unreadable, "disc root: a cut root -> unreadable");
  }
  Expect(lg::ReadDiscRoot((folder / "missing.iso").string()) == DiscRoot::unreadable, "disc root: no file -> unreadable");
  fs::remove_all(folder);
}

// Fix 112f: copies of one game on two drives are one tile.
void CheckCopies() {
  Expect(lg::DriveOf("/mnt/ext1/xbox360/halo ce/Halo.iso") == "ext1" && lg::DriveOf("/mnt/usb0/xbox360/a.iso") == "usb0" &&
             lg::DriveOf("/data/xbox360/a.iso").empty() && lg::DriveOf("/app0/assets/roms/a.iso").empty() &&
             lg::DriveOf("/mnt/").empty(),
         "DriveOf: the drive under /mnt, none for the console's storage");
  Expect(lg::DriveRank("/data/xbox360/a.iso") < lg::DriveRank("/mnt/ext1/a.iso") &&
             lg::DriveRank("/mnt/ext1/a.iso") < lg::DriveRank("/mnt/usb0/a.iso") &&
             lg::DriveRank("/mnt/usb0/a.iso") < lg::DriveRank("/mnt/disc/a.iso"),
         "DriveRank: the console's storage, then ext, then usb");
  using C = lg::CopyInfo;
  const uint64_t gb = 7800000000ull;
  {
    // Halo: Reach on usb0 (found first) and ext1, same title and size: one tile, on ext1.
    const auto into = lg::MergeCopies({C{"4D53085B", gb, "/mnt/usb0/xbox360/halo reach/Halo - Reach.iso", true},
                                       C{"4D53085B", gb, "/mnt/ext1/xbox360/halo reach/Halo - Reach.iso", true}});
    Expect(into == std::vector<size_t>{1, 1}, "identical copies on ext1 and usb0: one tile, the ext1 copy");
  }
  {
    const auto into = lg::MergeCopies({C{"4D53085B", gb, "/mnt/ext1/a.iso", true}, C{"4D53085B", gb + 2048, "/mnt/usb0/a.iso", true}});
    Expect(into == std::vector<size_t>{0, 1}, "a different size: two tiles");
  }
  {
    const auto into = lg::MergeCopies({C{"4D53085B", gb, "/mnt/ext1/a.iso", true}, C{"4D5309B1", gb, "/mnt/usb0/b.iso", true}});
    Expect(into == std::vector<size_t>{0, 1}, "a different title ID: two tiles");
  }
  {
    // The console's storage first; three copies; one of them not copyable (an extracted game).
    const auto into = lg::MergeCopies({C{"4D5309B1", gb, "/mnt/usb0/h.iso", true}, C{"4D5309B1", gb, "/mnt/ext0/h.iso", true},
                                       C{"4D5309B1", gb, "/data/xbox360/h.iso", true}, C{"4D5309B1", gb, "/mnt/ext1/h/default.xex", false}});
    Expect(into == std::vector<size_t>{2, 2, 2, 3}, "three copies: the console's storage; an extracted game apart");
  }
  {
    // Of one rank, the first found; unknown title IDs and sizes never merge.
    const auto into = lg::MergeCopies({C{"4D5309B1", gb, "/mnt/ext0/h.iso", true}, C{"4d5309b1", gb, "/mnt/ext1/h.iso", true},
                                       C{"", gb, "/mnt/ext0/x.iso", true}, C{"", gb, "/mnt/usb0/x.iso", true},
                                       C{"00000000", gb, "/mnt/ext0/y.iso", true}, C{"00000000", gb, "/mnt/usb0/y.iso", true},
                                       C{"4D530919", 0, "/mnt/ext0/z.iso", true}, C{"4D530919", 0, "/mnt/usb0/z.iso", true}});
    Expect(into == std::vector<size_t>{0, 0, 2, 3, 4, 5, 6, 7}, "one rank: the first; no title ID or no size: apart");
  }
}

void CheckFolders() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("check-library-groups-" + std::to_string(std::random_device{}()));
  fs::remove_all(root);
  const auto file = [&](const std::string& rel) {
    fs::create_directories((root / rel).parent_path());
    std::ofstream(root / rel, std::ios::binary) << "x";
  };
  file("Halo 3.iso");                                  // at the top
  file("Fable II/Fable II.iso");                       // a game in a folder of its own
  file("Gears of War/default.xex");                    // an extracted game
  file("Gears of War/Content/media.bin");
  file("Racing/Forza Motorsport 4.iso");
  file("Racing/Burnout Paradise/default.xex");         // an extracted game inside a group
  file("Racing/Rally/DiRT 3.iso");
  file("Racing/Rally/WRC 3.iso");
  file("Shooters/Battlefield 3 (Disc 1).iso");         // a disc set inside a group: one entry
  file("Shooters/Battlefield 3 (Disc 2).iso");
  file("Shooters/Mass Effect 3/Disc 1/default.xex");   // an extracted disc set inside the group
  file("Shooters/Mass Effect 3/Disc 2/default.xex");
  file("Final Fantasy XIII/Final Fantasy XIII (Disc 1).iso");  // a set alone in its folder
  file("Final Fantasy XIII/Final Fantasy XIII (Disc 2).iso");
  fs::create_directories(root / "Empty");              // empty folders: nothing
  fs::create_directories(root / "Empty Too" / "Deeper");
  file("Notes/readme.txt");                            // a folder with no game
  // Fix 112d: the console's Halo folders: one game each, beside its DLC and title updates.
  file("halo ce/Halo - Combat Evolved Anniversary (axekin.com).iso");
  file("halo ce/4D5309B1/00000002/D4D738C7BB23B73457C610D52E975958164C074A4D");  // DLC (marketplace)
  file("halo ce/4D5309B1/000B0000/tu00000001_00000000");                         // a title update
  file("halo reach/Halo - Reach (axekin.com).iso");
  file("halo reach/4D53085B/00000002/2EE2ED420A705835078EE64B8562E6EF3BCABFAC4D");
  file("halo reach/566C10D3/TU_16L622R_000001C000000.000000000008B");           // a raw title update
  file("Racing/Forza 4 GOD/4D5307E6/00007000/ABCDEF0123456789");                // a GOD game: still a game
  const auto found = Walk(root);
  Folders folders;
  std::map<std::string, size_t> at;
  for (size_t n = 0; n < found.size(); ++n) {
    folders.push_back(lg::EntryFolders(root.generic_string(), found[n].kind, found[n].path, found[n].folder));
    at[fs::path(found[n].path).lexically_relative(root).generic_string()] = n;
  }
  Expect(found.size() == 13, "thirteen library entries on the folders (" + std::to_string(found.size()) + ")");
  for (const auto& entry : found)
    Expect(!Has(entry.path, "/00000002/") && !Has(entry.path, "/000B0000/") && !Has(entry.path, "TU_"),
           "folders: no DLC or title update is a library entry: " + entry.path);
  const auto layout = lg::Build(folders);
  const auto shown = [&](const std::string& rel) {
    const auto it = at.find(rel);
    Expect(it != at.end(), "found: " + rel);
    return it == at.end() ? std::string("?") : layout.shown_in[it->second];
  };
  Expect(shown("Halo 3.iso").empty(), "folders: a game at the top");
  Expect(shown("Fable II/Fable II.iso").empty(), "folders: a game in its own folder is at the top");
  Expect(shown("Gears of War/default.xex").empty(), "folders: an extracted game is at the top, not a group");
  Expect(shown("Final Fantasy XIII/Final Fantasy XIII (Disc 1).iso").empty(), "folders: a set alone in its folder is at the top");
  Expect(shown("Racing/Forza Motorsport 4.iso") == "racing", "folders: Racing holds Forza");
  Expect(shown("Racing/Burnout Paradise/default.xex") == "racing", "folders: and the extracted Burnout");
  Expect(shown("Racing/Rally/DiRT 3.iso") == "racing/rally" && shown("Racing/Rally/WRC 3.iso") == "racing/rally",
         "folders: Rally inside Racing");
  Expect(shown("Shooters/Battlefield 3 (Disc 1).iso") == "shooters", "folders: the BF3 set is in Shooters");
  Expect(shown("Shooters/Mass Effect 3/Disc 1/default.xex") == "shooters", "folders: the extracted set too");
  std::vector<std::string> keys;
  for (const auto& group : layout.groups) keys.push_back(group.key);
  Expect(keys == std::vector<std::string>{"racing", "racing/rally", "shooters"}, "folders: three groups (" + Join(keys) + ")");
  Expect(GroupOf(layout, "racing") && GroupOf(layout, "racing")->games.size() == 5, "folders: Racing has five games");
  Expect(shown("Racing/Forza 4 GOD/4D5307E6/00007000/ABCDEF0123456789") == "racing", "folders: the GOD game is in Racing");
  Expect(shown("halo ce/Halo - Combat Evolved Anniversary (axekin.com).iso").empty() &&
             shown("halo reach/Halo - Reach (axekin.com).iso").empty(),
         "folders: a Halo folder with its DLC and title updates is one game at the top, no group");
  Expect(!GroupOf(layout, "halo ce") && !GroupOf(layout, "halo reach"), "folders: no Halo group");
  Expect(GroupOf(layout, "shooters") && GroupOf(layout, "shooters")->games.size() == 2, "folders: Shooters has two");
  fs::remove_all(root);
}

// (c) The assembled tree.
void CheckTree() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::string header = Read("include/xbox360ps5/launcher.hpp");
  const std::string press = Read("port/kinect_press.inc");
  Expect(Has(launcher, "#include \"library_groups.hpp\""), "the launcher has the groups");
  Expect(Has(launcher, "game_roots[game.path.string()] = scanning_root;"), "each game's game folder is kept");
  const size_t root = launcher.find("scanning_root = root.string();");
  const size_t walk = launcher.find("if (visible) scan(root, 0, root);", root);
  const size_t cleared = launcher.find("scanning_root.clear();", walk);
  const size_t drive = launcher.find("// Games on a disc in a drive", cleared);
  Expect(root != std::string::npos && walk != std::string::npos && cleared != std::string::npos && drive != std::string::npos,
         "a game folder is named while it is walked, the drive's disc has none");
  const size_t order = launcher.find("std::stable_sort(games_.begin(), games_.end()");
  const size_t build = launcher.find("g_groups = library_groups::Build(folders, ids);", order);
  const size_t filter = launcher.find("ApplyFilter();", build);
  Expect(order != std::string::npos && build != std::string::npos && filter != std::string::npos,
         "the groups are made after the shelf's order, before the view");
  Expect(Has(launcher, "folders[n] = library_groups::EntryFolders(root->second, first.kind, first.path.string(), first.folder);"),
         "an entry (a set by its first disc) is placed by EntryFolders");
  // Fix 112b: no game's cover on a group's tile; up to four covers for its folder; its number of games.
  Expect(!Has(launcher, "tile.cover = games_[n].cover"), "a group's tile has no game's cover of its own");
  Expect(Has(launcher, "if (cover >= 0 && covers.size() < 4 && std::find(covers.begin(), covers.end(), cover) == covers.end())"),
         "up to four different covers for the folder");
  Expect(Has(launcher, "Tr(library_groups::CountKey(group.games.size())), int(group.games.size())"),
         "the number of games in the number's form");
  Expect(Has(launcher, "void DrawGroupFolder(") && Has(launcher, "DrawGroupFolder(c, textures_, arts_, g_group_covers[g]") &&
             Has(launcher, "g_groups.groups[g].games.size(), entry.name, box,"),
         "both shelves draw a group as a folder");
  // Fix 112f: copies merged after the disc sets and before the order; kept as the tile's copies.
  const size_t sets = launcher.find("g_running_synced.clear();  // a set whose disc is running chooses it again");
  const size_t merge = launcher.find("const auto into = library_groups::MergeCopies(infos);", sets);
  const size_t sorted = launcher.find("std::stable_sort(games_.begin(), games_.end()", merge);
  Expect(sets != std::string::npos && merge != std::string::npos && sorted != std::string::npos,
         "copies are merged after the disc sets, before the shelf's order");
  Expect(Has(launcher, "shown.copies.push_back(games_[n].path);") &&
             Has(launcher, "XELOGW(\"LAUNCHER library entry {} also on {} (same title, same size), one tile\""),
         "a merged copy is kept and logged");
  Expect(Has(launcher, "for (const auto& copy : game.copies) paths.push_back(copy);      // fix 112f: every copy") &&
             Has(launcher, "other.path = copy;") && Has(launcher, "for (const auto& copy : game.copies) before.insert(copy.string());") &&
             Has(launcher, "for (const auto& copy : game.copies)  // fix 112f: a copy on another drive is this game too"),
         "GamePaths, the forwarder, the new-game notice and the recent list see every copy");
  Expect(Has(launcher, "game_bytes[path.string()] = entry.file_size(error);  // fix 112f"), "the sizes come from the scan");
  Expect(Has(launcher, "line += \"   ·   \" + (drive.empty() ? std::string(Tr(\"Internal storage\")) : drive);") &&
             Has(launcher, "(drive.empty() ? std::string(Tr(\"Internal storage\")) : drive);"),
         "the info line names the drive of the copy shown");
  Expect(Has(header, "std::vector<std::filesystem::path> copies;"), "GameEntry has its copies");
  // Fix 112e: an original Xbox disc is left out of the scan, with its log line, before it is listed.
  const size_t iso = launcher.find("} else if (extension == \".iso\") {");
  const size_t xbe = launcher.find("if (library_groups::ReadDiscRoot(path.string()) == library_groups::DiscRoot::original_xbox) {", iso);
  const size_t xbe_log = launcher.find("XELOGW(\"LAUNCHER skipped {}: an original Xbox game (default.xbe), not supported\", path.string());", xbe);
  const size_t listed = launcher.find("GameEntry game{path.stem().string(), path, \"ISO\", directory.string()};", xbe_log);
  Expect(iso != std::string::npos && xbe != std::string::npos && xbe_log != std::string::npos && listed != std::string::npos &&
             listed - iso < 500,
         "an original Xbox disc is skipped with its log line before an image is listed");
  // Fix 112d: copies of one game count once; non-game content type folders are not read; the log.
  Expect(Has(launcher, "g_groups = library_groups::Build(folders, ids);") &&
             Has(launcher, "if (!games_[n].title_id.empty() && games_[n].title_id != \"00000000\") ids[n] = games_[n].title_id;"),
         "the groups count games by title ID");
  const size_t skip = launcher.find("if (!library_groups::GamePackageFolder(directory.filename().string())) continue;");
  const size_t read = launcher.find("if (!ReadPackage(path, package) || !package.game) continue;", skip);
  Expect(skip != std::string::npos && read != std::string::npos && read - skip < 120,
         "a non-game content type folder is skipped before a package is read");
  Expect(Has(launcher, "XELOGW(\"LAUNCHER group {}: {} games\", group.key, group.games.size());") &&
             Has(launcher, "XELOGW(\"LAUNCHER group {}:   {} ({}, title {})\"") &&
             Has(launcher, "XELOGW(\"LAUNCHER library entry {} ({}, title {}) at the top{}\""),
         "each group with its games, and each entry at the top, in the log");
  // Fix 112c: the covers' places come from MosaicCells, with each texture's own proportions.
  Expect(Has(launcher, "aspects.push_back(size_t(shown[i]) < arts.size() ? arts[size_t(shown[i])].aspect : 0.0f);") &&
             Has(launcher, "const auto cells = library_groups::MosaicCells(aspects, folder_h > 0 ? folder_w / folder_h : 0.0f);"),
         "the mosaic keeps each cover's proportions");
  Expect(Has(launcher, "Part(q, label.u0, label.v0, label.u1, label.v1)") && Has(launcher, "Part(q, badge.u0, badge.v0, badge.u1, badge.v1)"),
         "the name and the badge where MosaicCells keeps clear of");
  Expect(Has(launcher, "        DrawCase(c, textures_, arts_, Tile(n), q,"), "a game is still drawn as its case");
  Expect(!Has(launcher, "A group is a little stack"), "no stack of game cases for a group");
  Expect(Has(launcher, "g_group = library_groups::StillThere(g_groups, g_group);"), "a rebuild stays in the group");
  Expect(Has(launcher, "return item < 0 ? nullptr : &games_[size_t(item)];"), "a group's tile is no Selected() game");
  Expect(Has(launcher, "if (g_groups.groups[g].parent == g_group) view_.push_back(-1 - int(g));"),
         "the open group's groups are in the view");
  Expect(Has(launcher, "const bool flat = filter_ != 0 || !g_search.empty();"), "a filter or a search shows games only");
  Expect(Has(launcher, "if (!g_groups.Inside(size_t(n), g_group)) continue;"), "the view keeps to the open group");
  Expect(!Has(launcher, "games_[size_t(view_[size_t(n)])]") && !Has(launcher, "games_[size_t(view_[size_t(selected_)])]"),
         "nothing reads view_ as games only");
  Expect(Has(launcher, "if (SelectedGroup() >= 0) OpenGroup(SelectedGroup());"), "X opens a group");
  const size_t circle = launcher.find("if (mode_ == Mode::shelf && key == Key::circle && !g_group.empty()) {");
  const size_t leave = launcher.find("LeaveGroup();", circle);
  const size_t library = launcher.find("if (mode_ == Mode::shelf && home_library_open && key == Key::circle) {", circle);
  Expect(circle != std::string::npos && leave < library && library != std::string::npos,
         "Circle leaves the group before it closes the library");
  Expect(Has(launcher, "if (const int kept = FindKept(view_, games_, left); kept >= 0) selected_ = kept;"),
         "back in the parent, its tile is chosen");
  Expect(Has(launcher, "RevealGame(wanted);  // fix 112"), "the per-game list opens a game in its group");
  Expect(Has(press, "if (RevealGame(slot.game) >= 0) {"), "the home's game slots open a game in its group");
  Expect(Has(launcher, "DrawCase(c, textures_, arts_, Tile(n), q,") && Has(launcher, "const GameEntry& entry = Tile(n);"),
         "both shelves draw Tile(n)");
  Expect(Has(launcher, "if (Selected()) x = c.Hint('t', Tr(\"Game settings\"), x, 990);") &&
             Has(launcher, "if (Selected()) right = NxeHint(c, 't', Tr(\"Game settings\"), right, 990);"),
         "Game settings only for a game");
  Expect(Has(launcher, "if (!g_group.empty()) c.Chip(GroupTrail(g_group), 96, 124, kAccent);") &&
             Has(launcher, "if (!g_group.empty()) NxeText(c, GroupTrail(g_group), 150, 286 + slide"),
         "both shelves name the open group");
  Expect(Has(launcher, "if (NxeUi() || !g_group.empty()) x = c.Hint('o', Tr(\"Back\"), x, 990);") &&
             Has(launcher, "if (!g_group.empty()) right = NxeHint(c, 'o', Tr(\"Back\"), right, 990);"),
         "both shelves say Circle goes back");
  Expect(Has(launcher, "std::string detail = group ? std::string(Tr(\"Group\")) : entry.kind;") &&
             Has(launcher, "if (group_chosen) line = Tr(\"Group\");"),
         "a group's tile says Group and its number of games");
  Expect(Has(header, "int SelectedGroup() const;") && Has(header, "void OpenGroup(int group);") &&
             Has(header, "void LeaveGroup();") && Has(header, "int RevealGame(int index);"),
         "the launcher's header declares the group functions");
  // The library's own lists still hold every game, not the groups.
  Expect(Has(launcher, "const std::vector<GameEntry>& Entries() const { return games_; }") ||
             Has(header, "const std::vector<GameEntry>& Entries() const { return games_; }"),
         "Entries() is still every game");
}

void CheckLanguages(const std::string& folder) {
  namespace fs = std::filesystem;
  int files = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.path().extension() != ".txt") continue;
    ++files;
    const std::string text = "\n" + Read(entry.path().string());
    const std::string name = entry.path().filename().string();
    const size_t at = text.find("\nGroup = ");
    Expect(at != std::string::npos, name + " translates Group");
    if (at == std::string::npos) continue;
    const size_t start = at + 9;
    const std::string value = text.substr(start, text.find('\n', start) - start);
    Expect(!value.empty() && value.find('%') == std::string::npos, name + ": Group has a text: " + value);
    // Fix 112b: the four forms, each with one %d.
    for (const char* key : {R"(%d\sgame)", R"(%d\sgames)", R"(%d\sgames||few)", R"(%d\sgames||one)"}) {
      const size_t found = text.find("\n" + std::string(key) + " = ");
      Expect(found != std::string::npos, name + " translates " + key);
      if (found == std::string::npos) continue;
      const size_t from = found + 1 + std::string(key).size() + 3;
      const std::string form = text.substr(from, text.find('\n', from) - from);
      const size_t d = form.find("%d");
      Expect(d != std::string::npos && form.find('%', d + 1) == std::string::npos, name + ": " + key + " has one %d: " + form);
    }
    if (name == "pl.txt")
      Expect(Has(text, "\n" R"(%d\sgames||few = %d\sgry)" "\n") && Has(text, "\n" R"(%d\sgames = %d\sgier)" "\n") &&
                 Has(text, "\n" R"(%d\sgame = %d\sgra)" "\n"),
             "pl.txt: 1 gra, 2 gry, 5 gier");
  }
  Expect(files == 16, "16 language files (" + std::to_string(files) + ")");
}

}  // namespace

int main(int argc, char** argv) {
  CheckBuild();
  CheckMosaic();
  CheckDiscRoot();
  CheckCopies();
  CheckFolders();
  CheckTree();
  if (argc > 1) CheckLanguages(argv[1]);
  else Expect(false, "the language folder is given");
  if (failures) {
    std::printf("check-library-groups: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-library-groups: ok\n");
  return 0;
}
