// SPDX-License-Identifier: MIT
// Fix 159 (tools/source-1.0.2/fixes/159-single-game-folder.patch): a player's folder holding one game
// of several discs is that game in the library, never a group (a folder tile). The player keeps each
// multi-disc game in a folder of its own: "Battlefield 3 (USA, Europe) (En,Fr,De)/" with "(Disc 1).iso"
// and "(Disc 2).iso", Final Fantasy XIII with Disc 1-3, Grand Theft Auto V with "(Disc 1) (Install)"
// and "(Disc 2) (Play)". Fix 89 makes such discs one entry (then the folder holds one entry and is no
// group, fix 112); when it cannot, the discs stay entries of their own and fix 112d counted them as
// different games when their title IDs differed or were unknown. Now library_groups::GameKeys counts
// them as one game: one title ID (fix 112d, anywhere), or one folder and one name without the disc
// tags (multidisc::NameWithoutDisc, fixes 89 and 148b).
//  (a) GameKeys + Build on the console's folders, with the launcher's set names (SetName below does
//      what Launcher::Scan does): BF3 Disc 1+2, FFXIII Disc 1-3 (a title ID of its own on each disc),
//      GTA V (Install)/(Play) (one title ID known, one not) -> one game, no group; a fix 89 set (one
//      entry) -> no group, as before; a folder of two different games -> a group; one ISO beside its
//      title ID content folders (DLC, title updates: not read by the scan; a GOD copy in 00007000)
//      -> one game; one single-disc ISO in its own folder -> one game, no group (today's behaviour,
//      unchanged); nested folders: a folder of two such games is a group, their own folders are not;
//      an extracted set InspectExtractedDiscGroup checked (its folder the set's) counts in the folder
//      above it; two regions' discs in one folder with different or unknown title IDs are two games
//      (one title ID: one game, fix 112d); one name in two folders is two games;
//  (b) GameKeys' own edges: 00000000 is no title ID, case is ignored, a singleton without a title ID
//      is "" (a game of its own, as Build takes it), title IDs join across folders (fix 112d); SetKey;
//  (c) the assembled launcher makes the set names as SetName does (each step pinned) and passes
//      GameKeys to Build, and logs the discs of one game fix 89 left apart (with their headers).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/disc_catalog.cpp"
#include "../port/library_groups.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace lg = xbox360ps5::library_groups;
namespace md = xbox360ps5::multidisc;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

const std::string kRoot = "/mnt/ext1/xbox360";

// One library entry as the scan leaves it: an image (or an extracted disc's default.xex), its title
// ID ("" unknown), and its folder (GameEntry::folder: for an extracted disc the folder the scan found
// it in, "" for its own Disc N folder; an extracted set InspectExtractedDiscGroup checked has the
// set's folder, e.g. "RPG/Lost Odyssey").
struct Entry {
  std::string kind, path, title, folder;
  Entry(std::string kind_, std::string path_, std::string title_, std::string folder_ = "")
      : kind(std::move(kind_)), path(std::move(path_)), title(std::move(title_)), folder(std::move(folder_)) {}
};

// What Launcher::Scan gives GameKeys for an entry (fix 159's set_names).
std::string SetName(const std::string& kind, const std::string& path) {
  if (kind != "ISO" && kind != "XEX") return "";
  const fs::path image = kind == "XEX" ? fs::path(path).parent_path() : fs::path(path);
  if (!md::DiscOfImage(image)) return "";
  const std::string file = image.filename().string();
  const std::string bare = md::NameWithoutDisc(file);
  return bare != file ? bare : image.parent_path().filename().string();
}

struct Library {
  std::vector<std::vector<std::string>> folders;
  std::vector<std::string> titles, sets;
  lg::Layout layout;
};

Library Make(const std::vector<Entry>& entries) {
  Library library;
  for (const auto& entry : entries) {
    const std::string folder = entry.kind != "XEX" ? "" : !entry.folder.empty() ? kRoot + "/" + entry.folder
                                                                                 : fs::path(entry.path).parent_path().string();
    library.folders.push_back(lg::EntryFolders(kRoot, entry.kind, entry.path, folder));
    library.titles.push_back(entry.title);
    library.sets.push_back(SetName(entry.kind, entry.path));
  }
  library.layout = lg::Build(library.folders, lg::GameKeys(library.folders, library.titles, library.sets));
  return library;
}

bool HasGroup(const Library& library, const std::string& key) { return library.layout.Find(key) >= 0; }

const std::string kBf3 = "Battlefield 3 (USA, Europe) (En,Fr,De,Es,It)";
const std::string kFf13 = "Final Fantasy XIII (Europe) (En,Fr,De,Es,It)";
const std::string kGta5 = "Grand Theft Auto V (World) (En,Fr,De,Es,It,Pt,Zh,Ko,Pl,Ru)";
// Something else at the top of the library, so a lone group is not taken for the top (fix 112).
const Entry kTop{"ISO", kRoot + "/Halo 3.iso", "4D5307E6"};

void SetNames() {
  Expect(SetName("ISO", kRoot + "/" + kBf3 + "/" + kBf3 + " (Disc 1).iso") == kBf3, "(a) BF3 Disc 1's set name");
  Expect(SetName("ISO", kRoot + "/" + kBf3 + "/" + kBf3 + " (Disc 2).iso") == kBf3, "(a) BF3 Disc 2's set name");
  Expect(SetName("ISO", kRoot + "/x/" + kGta5 + " (Disc 1) (Install).iso") == kGta5, "(a) GTA V (Install)'s set name");
  Expect(SetName("ISO", kRoot + "/x/" + kGta5 + " (Disc 2) (Play).iso") == kGta5, "(a) GTA V (Play)'s set name");
  Expect(SetName("ISO", kRoot + "/Halo 3/Halo 3.iso").empty(), "(a) a single-disc image has no set name");
  Expect(SetName("ISO", kRoot + "/Lost Odyssey/Disc 2.iso") == "Lost Odyssey", "(a) a bare Disc 2.iso: its folder");
  Expect(SetName("XEX", kRoot + "/Lost Odyssey/Disc 3/default.xex") == "Lost Odyssey", "(a) an extracted Disc 3: the set's folder");
  Expect(SetName("XEX", kRoot + "/Games/Mass Effect 2 (Disc 1)/default.xex") == "Mass Effect 2", "(a) an extracted (Disc 1) folder");
  Expect(SetName("GOD/STFS", kRoot + "/x/4D5307E6/00007000/ABCDEF").empty(), "(a) a package has no set name");
}

void OneGameFolders() {
  // Battlefield 3: two discs fix 89 did not join (headers unread, no title ID yet).
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/" + kBf3 + "/" + kBf3 + " (Disc 1).iso", ""},
                               {"ISO", kRoot + "/" + kBf3 + "/" + kBf3 + " (Disc 2).iso", ""}});
    Expect(library.layout.groups.empty(), "(a) BF3 Disc 1+2 in a folder: no group");
    Expect(library.layout.shown_in[1].empty() && library.layout.shown_in[2].empty(), "(a) BF3's discs at the top");
  }
  // The same discs with their title ID (45410950 on both): one game, as fix 112d already said.
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/" + kBf3 + "/" + kBf3 + " (Disc 1).iso", "45410950"},
                               {"ISO", kRoot + "/" + kBf3 + "/" + kBf3 + " (Disc 2).iso", "45410950"}});
    Expect(library.layout.groups.empty(), "(a) BF3 with one title ID: no group");
  }
  // A fix 89 set: one entry (the first disc's path) in the folder.
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/" + kBf3 + "/" + kBf3 + " (Disc 1).iso", "45410950"}});
    Expect(library.layout.groups.empty() && library.layout.shown_in[1].empty(), "(a) BF3 as one fix 89 entry: no group");
  }
  // Final Fantasy XIII: three discs, a title ID of its own on each.
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/" + kFf13 + "/" + kFf13 + " (Disc 1).iso", "535107D4"},
                               {"ISO", kRoot + "/" + kFf13 + "/" + kFf13 + " (Disc 2).iso", "535107D5"},
                               {"ISO", kRoot + "/" + kFf13 + "/" + kFf13 + " (Disc 3).iso", "535107D6"}});
    Expect(library.layout.groups.empty(), "(a) FFXIII Disc 1-3 in a folder: no group");
    const auto keys = lg::GameKeys(library.folders, library.titles, library.sets);
    Expect(keys[1] == keys[2] && keys[2] == keys[3] && keys[1] == "title:535107d4", "(a) FFXIII's discs: one game");
  }
  // Grand Theft Auto V: (Install) launched once (its title ID cached), (Play) not.
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/GTA V/" + kGta5 + " (Disc 1) (Install).iso", "545408A7"},
                               {"ISO", kRoot + "/GTA V/" + kGta5 + " (Disc 2) (Play).iso", ""}});
    Expect(library.layout.groups.empty(), "(a) GTA V (Install)+(Play) in a folder: no group");
  }
  // Extracted discs of one game in its folder: "Lost Odyssey/Disc 1..4/default.xex".
  {
    std::vector<Entry> entries{kTop};
    for (int disc = 1; disc <= 4; ++disc)
      entries.push_back({"XEX", kRoot + "/RPG/Lost Odyssey/Disc " + std::to_string(disc) + "/default.xex", ""});
    entries.push_back({"ISO", kRoot + "/RPG/Blue Dragon/" + std::string("Blue Dragon (Disc 1).iso"), ""});
    entries.push_back({"ISO", kRoot + "/RPG/Blue Dragon/" + std::string("Blue Dragon (Disc 2).iso"), ""});
    const auto library = Make(entries);
    Expect(library.layout.groups.size() == 1 && HasGroup(library, "rpg"), "(a) RPG holds two games: a group");
    Expect(!HasGroup(library, "rpg/lost odyssey") && !HasGroup(library, "rpg/blue dragon"),
           "(a) an extracted set's and an image set's folder: no group");
  }
  // An extracted set InspectExtractedDiscGroup checked but fix 89 did not join: each disc's folder is
  // the set's ("RPG/Lost Odyssey"), so the folder above it (RPG) holds the discs. Beside another game
  // RPG is a group of two games; alone, RPG holds one game and is no group.
  {
    std::vector<Entry> discs;
    for (int disc = 1; disc <= 4; ++disc)
      discs.push_back({"XEX", kRoot + "/RPG/Lost Odyssey/Disc " + std::to_string(disc) + "/default.xex", "", "RPG/Lost Odyssey"});
    std::vector<Entry> entries{kTop};
    entries.insert(entries.end(), discs.begin(), discs.end());
    const auto alone = Make(entries);
    Expect(alone.layout.groups.empty(), "(a) a grouped extracted set alone in RPG: no group");
    Expect(alone.sets[1] == "Lost Odyssey" && alone.sets[4] == "Lost Odyssey", "(a) a grouped extracted set's set names");
    entries.push_back({"ISO", kRoot + "/RPG/Blue Dragon (Disc 1).iso", ""});
    const auto beside = Make(entries);
    Expect(beside.layout.groups.size() == 1 && HasGroup(beside, "rpg") && beside.layout.groups[0].games.size() == 5,
           "(a) a grouped extracted set beside another game in RPG: a group");
    const auto keys = lg::GameKeys(beside.folders, beside.titles, beside.sets);
    Expect(keys[1] == keys[4] && !keys[1].empty() && keys[5] != keys[1], "(a) the set's discs: one game; Blue Dragon another");
  }
}

void OtherFolders() {
  // Two different games in one folder: a group, as before.
  {
    const auto library = Make({{"ISO", kRoot + "/top.iso", ""}, {"ISO", kRoot + "/Halo/Halo 3.iso", "4D5307E6"},
                               {"ISO", kRoot + "/Halo/Halo Reach.iso", "4D53085B"}});
    Expect(library.layout.groups.size() == 1 && HasGroup(library, "halo"), "(a) two different games: a group");
    Expect(library.layout.shown_in[1] == "halo" && library.layout.shown_in[2] == "halo", "(a) both in the group");
  }
  // Two different games without title IDs: still two games.
  {
    const auto library = Make({{"ISO", kRoot + "/top.iso", ""}, {"ISO", kRoot + "/Mixed/Fable II.iso", ""},
                               {"ISO", kRoot + "/Mixed/Gears of War.iso", ""}});
    Expect(HasGroup(library, "mixed"), "(a) two games without title IDs: a group");
  }
  // A multi-disc game beside another game in one folder: two games, a group.
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/Shooters/" + kBf3 + " (Disc 1).iso", ""},
                               {"ISO", kRoot + "/Shooters/" + kBf3 + " (Disc 2).iso", ""},
                               {"ISO", kRoot + "/Shooters/Halo 4.iso", "4D530919"}});
    Expect(HasGroup(library, "shooters") && library.layout.groups[0].games.size() == 3,
           "(a) BF3's discs beside Halo 4: a group (two games)");
  }
  // One ISO beside its title ID content folders: the DLC (00000002) and title updates (000B0000) are
  // not read by the scan; a GOD copy of the game in 00007000 is the same game.
  {
    Expect(!lg::GamePackageFolder("00000002") && !lg::GamePackageFolder("000B0000"), "(a) DLC and updates are not read");
    const auto library = Make({kTop, {"ISO", kRoot + "/halo ce/Halo CE.iso", "4D530064"},
                               {"GOD/STFS", kRoot + "/halo ce/4D530064/00007000/0123456789ABCDEF", "4D530064"}});
    Expect(library.layout.groups.empty(), "(a) one ISO and its title ID folders: no group");
    const auto alone = Make({kTop, {"ISO", kRoot + "/halo reach/Halo Reach.iso", ""}});
    Expect(alone.layout.groups.empty(), "(a) one ISO (title unknown) beside its content folders: no group");
  }
  // One single-disc ISO in a folder of its own: no group (today's behaviour too, fix 112).
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/Fable II/Fable II.iso", ""}});
    Expect(library.layout.groups.empty() && library.layout.shown_in[1].empty(), "(a) a single-disc ISO's folder: no group");
    const auto before = lg::Build(library.folders, library.titles);
    Expect(before.groups.empty(), "(a) a single-disc ISO's folder was no group before either");
  }
  // Nested: a folder of two multi-disc games is a group; each game's own folder is not.
  {
    std::vector<Entry> entries{kTop};
    for (int disc = 1; disc <= 2; ++disc)
      entries.push_back({"ISO", kRoot + "/Series/" + kBf3 + "/" + kBf3 + " (Disc " + std::to_string(disc) + ").iso", ""});
    for (int disc = 1; disc <= 3; ++disc)
      entries.push_back({"ISO", kRoot + "/Series/" + kFf13 + "/" + kFf13 + " (Disc " + std::to_string(disc) + ").iso", ""});
    const auto library = Make(entries);
    Expect(library.layout.groups.size() == 1 && HasGroup(library, "series"), "(a) nested: Series is the one group");
    bool inside = true;
    for (size_t n = 1; n < entries.size(); ++n) inside = inside && library.layout.shown_in[n] == "series";
    Expect(inside, "(a) nested: every disc shown in Series");
    Expect(library.layout.groups[0].games.size() == 5, "(a) nested: the group holds its five entries");
    // Deeper still: Collections/Series/<game>: Series passes through when it is Collections' only folder.
    std::vector<Entry> deeper{kTop, {"ISO", kRoot + "/Collections/Other.iso", ""}};
    for (size_t n = 1; n < entries.size(); ++n) {
      std::string path = entries[n].path;
      path.insert(kRoot.size(), "/Collections");
      deeper.push_back({"ISO", path, ""});
    }
    const auto nested = Make(deeper);
    Expect(HasGroup(nested, "collections") && HasGroup(nested, "collections/series") && nested.layout.groups.size() == 2,
           "(a) nested: Collections and its Series are groups, the games' folders are not");
  }
  // Two regions' discs in one folder, their title IDs unknown (or different): two names, two games, a
  // group. With one title ID they are one game, as fix 112d counts copies.
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/Regions/Blue Dragon (USA) (Disc 1).iso", ""},
                               {"ISO", kRoot + "/Regions/Blue Dragon (USA) (Disc 2).iso", ""},
                               {"ISO", kRoot + "/Regions/Blue Dragon (Europe) (Disc 1).iso", ""},
                               {"ISO", kRoot + "/Regions/Blue Dragon (Europe) (Disc 2).iso", ""}});
    Expect(HasGroup(library, "regions"), "(a) two regions' discs in one folder: a group");
    const auto shared = Make({kTop, {"ISO", kRoot + "/Regions/Blue Dragon (USA) (Disc 1).iso", "4D5307F1"},
                              {"ISO", kRoot + "/Regions/Blue Dragon (Europe) (Disc 1).iso", "4D5307F1"}});
    Expect(shared.layout.groups.empty(), "(a) two regions' discs with one title ID: one game (fix 112d)");
  }
  // One name in two folders, no title ID: not joined across folders (each folder holds one disc).
  {
    const auto library = Make({kTop, {"ISO", kRoot + "/Box/A/Odd Game (Disc 1).iso", ""},
                               {"ISO", kRoot + "/Box/B/Odd Game (Disc 2).iso", ""}});
    const auto keys = lg::GameKeys(library.folders, library.titles, library.sets);
    Expect(keys[1].empty() && keys[2].empty(), "(a) one name in two folders: two games");
    Expect(HasGroup(library, "box"), "(a) Box with the two: a group, as before");
  }
}

void Edges() {
  const std::vector<std::vector<std::string>> folders{{"A"}, {"a"}, {"B"}, {"C"}, {"C"}, {"D"}, {"D"}};
  const std::vector<std::string> titles{"4d5307e6", "4D5307E6", "00000000", "", "", "4D530919", ""};
  const std::vector<std::string> sets{"", "", "", "Game ", "game", "", ""};
  const auto keys = lg::GameKeys(folders, titles, sets);
  Expect(keys[0] == keys[1] && keys[0] == "title:4d5307e6", "(b) a title ID without case, in two folders");
  Expect(keys[2].empty(), "(b) 00000000 is no title ID; alone, a game of its own");
  Expect(keys[3] == keys[4] && !keys[3].empty() && keys[3] != keys[0], "(b) one folder (without case) and one name");
  Expect(keys[5] == "title:4d530919" && keys[6].empty(), "(b) a title ID alone; nothing to join");
  Expect(lg::HolderKey({"Racing", "4D5307E6", "00007000"}) == "racing" && lg::HolderKey({}).empty(),
         "(b) HolderKey skips a package's folders");
  Expect(lg::GameKeys({}, {}, {}).empty(), "(b) no entries");
  Expect(lg::SetKey({"Racing", "4D5307E6", "00007000"}, "Game \t") == "racing\ngame" && lg::SetKey({"x"}, " ").empty() &&
             lg::SetKey({}, "Game") == "\ngame",
         "(b) SetKey: the holder and the name, lower case, trimmed; \"\" without a name");
  // Shorter title / set lists than entries are allowed.
  Expect(lg::GameKeys({{"x"}, {"x"}}, {}, {}).size() == 2, "(b) short lists");
}

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void Tree() {
  const std::string launcher = Read("src/launcher.cpp");
  Expect(launcher.find("library_groups::Build(folders, library_groups::GameKeys(folders, ids, set_names))") != std::string::npos,
         "(c) the launcher builds the groups with GameKeys");
  Expect(launcher.find("set_names[n] = bare != file ? bare : image.parent_path().filename().string();") != std::string::npos,
         "(c) the launcher's set names are SetName's");
  Expect(launcher.find("multidisc::NameWithoutDisc(file)") != std::string::npos, "(c) by fix 89's NameWithoutDisc");
  // SetName's steps, each as the launcher has it.
  const size_t names = launcher.find("std::vector<std::string> set_names(games_.size());");
  const size_t build = launcher.find("library_groups::Build(folders, library_groups::GameKeys(", names);
  const auto in_names = [&](const char* text) {
    const size_t at = launcher.find(text, names);
    return names != std::string::npos && at != std::string::npos && at < build;
  };
  Expect(in_names("const GameEntry& first = games_[n].discs.empty() ? games_[n] : games_[n].discs.front();"),
         "(c) a set by its first disc");
  Expect(in_names("if (first.kind != \"ISO\" && first.kind != \"XEX\") continue;"), "(c) only images and extracted discs");
  Expect(in_names("const fs::path image = first.kind == \"XEX\" ? first.path.parent_path() : first.path;"),
         "(c) an extracted disc by its folder");
  Expect(in_names("if (!multidisc::DiscOfImage(image)) continue;"), "(c) only names with a disc number");
  Expect(in_names("const std::string bare = multidisc::NameWithoutDisc(file);"), "(c) the name without the disc tags");
  // The discs fix 89 left apart are logged by SetKey, with their headers.
  Expect(launcher.find("library_groups::SetKey(folders[n], set_names[n])", build) != std::string::npos &&
             launcher.find("LAUNCHER disc set not made:", build) != std::string::npos,
         "(c) discs of one game fix 89 left apart are in the log");
  Expect(launcher.find("std::string& seen = disc_headers[games_[n].path.string()];") != std::string::npos,
         "(c) fix 89's header reads are kept for that log");
}
}  // namespace

int main() {
  SetNames();
  OneGameFolders();
  OtherFolders();
  Edges();
  Tree();
  if (failures) {
    std::printf("check-single-game-folder: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-single-game-folder: ok\n");
  return 0;
}
