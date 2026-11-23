// SPDX-License-Identifier: MIT
// Fix 109 (tools/source-1.0.2/fixes/109-dlc-remove.patch): the game sheet's DLCs tab looks at every
// save device and can remove an installed package. Battlefield 3 (45410950) installs its HD
// textures itself, as a package file in <saves>/0000000000000000/45410950/00000002 of the game's
// save drive; with the save drive on the extended storage (fix 101) that is
// /mnt/ext1/xbox360/PS5X360Reloaded/saves, which the tab did not look at (only the internal folder),
// and nothing could remove them.
//  (a) dlc2::FindAll lists the installed packages of every root (each with its root, drive name and
//      size) and the dlc folders' packages installed on none of them; RootsOf takes the storage
//      scan's devices (each root once, empty ones left out);
//  (b) on a temporary folder laid out as the console (internal folder, ext1 whose top is not
//      writable but its xbox360 folder is, an empty usb0), storage::Scan's devices through RootsOf
//      find the package on ext1 (POSIX hosts: the scan's header needs them);
//  (c) dlc2::Remove refuses while a game is running and leaves the file; it deletes only the chosen
//      package (not its disabled twin, not the same file on another drive, not the dlc folder's
//      copy, not a neighbour), keeps the folders, and never removes a dlc folder's package or a
//      path outside the item's own folder; Move works on the package's own drive. It uses stat and
//      unlink only (the console's sandbox refuses lstat, and libc's remove() asks lstat first:
//      1.0.5-work-fix109 left BF3's HD textures on ext1 with "not removed"), and says which step
//      failed with its errno (a folder that refuses the unlink: EACCES, POSIX hosts not run as root);
//  (c2) 1.0.5-work-fix109b: BF3's reinstalled HD textures ended up in 00000002-off with an empty
//      00000002 beside them and no log line (Off pressed while the game ran behind the library):
//      dlc2::Switch is refused while a game runs, says its step (rename) and errno, and a drive with
//      the file both On and Off lists both rows and refuses to switch either (twin) until one is
//      removed;
//  (d) launcher.cpp: the tab and the start of a game use the scan's roots, Triangle opens the
//      "Remove DLC?" window only while no game runs, the window says the name, size and drive and
//      that it cannot be undone, with the raised buttons; X (install, on, off) is refused while a
//      game runs and every change is logged;
//  (e) the language files (argument: tools/source-1.0.2/english/lang) have every text it shows;
//  (f) the game sheet: L1 and R1 keys at the two ends of the tab bar (the "L1 / R1" text gone); its
//      lists (Patches, Settings' subjects and options, Achievements, DLCs, Updates) start on the tab
//      bar with no row highlighted, Down enters a list at its first row and Up on the first row
//      leaves it;
//  (g) fix 109d, the player's pick for the game sheet and the in-game guide (fix 115c): one tab bar
//      from xbox360ps5/tab_bar.hpp, which both fixes share (in the repository's reconstructed/
//      folder, so neither depends on the other's number): "< L1  tabs  R1 >" with high-contrast
//      pills; its pad (tab_bar::Press): L1/R1 always switch tabs (the bar focused), on the bar
//      left/right switch tabs and Down or X enter the list at its first row, in a list Up on the
//      first row goes back, Down stops at the last row, left/right only change a row with a value
//      ("< value >", tab_bar::DrawValue); seven tab names shrink to fit the 860 px sheet, the pills
//      keep their size. Checked as code here (Arrange, Press) and as its use in launcher.cpp.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "../port/dlc_manager.hpp"
#include "xbox360ps5/tab_bar.hpp"  // the tree's reconstructed/ (-Ireconstructed)

#if __has_include(<sys/statvfs.h>)
#include "xenia/kernel/xam/storage_devices.h"
#define CHECK_DLC_SCAN 1
#endif

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace dlc2 = xbox360ps5::dlc2;
namespace stdfs = std::filesystem;

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}
std::string Read(const stdfs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

constexpr uint32_t kBf3 = 0x45410950;
void Put32(std::string& data, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) data[at + size_t(i)] = char(value >> (24 - 8 * i) & 0xFF);
}
// An Xbox 360 package header (magic, content type 0x344, title 0x360, English name 0x411) padded
// to `bytes`.
void Package(const stdfs::path& path, uint32_t title, uint32_t type, const std::string& name, size_t bytes) {
  std::error_code ec;
  stdfs::create_directories(path.parent_path(), ec);
  std::string data(std::max<size_t>(bytes, 0x600), '\0');
  data.replace(0, 4, "LIVE");
  Put32(data, 0x344, type);
  Put32(data, 0x360, title);
  for (size_t i = 0; i < name.size(); ++i) data[0x412 + 2 * i] = name[i];
  std::ofstream(path, std::ios::binary).write(data.data(), std::streamsize(data.size()));
}
const dlc2::Item* Named(const dlc2::Found& found, const std::string& name, const std::string& device) {
  for (const auto& item : found.items)
    if (item.name == name && item.device == device) return &item;
  return nullptr;
}
int Count(const dlc2::Found& found, const std::string& name) {
  int n = 0;
  for (const auto& item : found.items) n += item.name == name;
  return n;
}

struct Layout {
  stdfs::path base, internal, ext1, dlc;
};
// A console's saves: the internal folder (BF3's map pack On, a pack Off) and ext1's (the HD
// textures, the game's own file name, and a copy of the map pack); a dlc folder with that map
// pack again, a new pack and another game's.
Layout MakeLayout(const stdfs::path& base) {
  Layout l{base, base / "internal" / "saves", base / "mnt" / "ext1" / "xbox360" / "PS5X360Reloaded" / "saves",
           base / "dlc"};
  std::error_code ec;
  stdfs::remove_all(base, ec);
  Package(dlc2::ActiveFolder(l.internal, kBf3) / "map_pack_1", kBf3, 0x00000002, "Back to Karkand", 300000);
  Package(dlc2::DisabledFolder(l.internal, kBf3) / "premium_pack", kBf3, 0x00000002, "Premium", 120000);
  Package(dlc2::ActiveFolder(l.ext1, kBf3) / "4541095000000000", kBf3, 0x00000002, "HD Content", 1500000);
  Package(dlc2::ActiveFolder(l.ext1, kBf3) / "map_pack_1", kBf3, 0x00000002, "Back to Karkand", 300000);
  Package(l.dlc / "map_pack_1", kBf3, 0x00000002, "Back to Karkand", 300000);
  Package(l.dlc / "close_quarters", kBf3, 0x00000002, "Close Quarters", 80000);
  Package(l.dlc / "other_game", 0x4D530A87, 0x00000002, "Not this game", 2000);
  return l;
}
std::vector<dlc2::Root> Roots(const Layout& l) {
  return {{l.internal, "Internal storage"}, {l.ext1, "M.2 SSD / extended storage (ext1)"}};
}

void CheckListing(const stdfs::path& base) {
  const Layout l = MakeLayout(base);
  const dlc2::Found found = dlc2::FindAll(kBf3, Roots(l), {l.dlc});
  const auto* hd = Named(found, "HD Content", "M.2 SSD / extended storage (ext1)");
  Expect(hd && hd->state == dlc2::State::active && hd->root == l.ext1 && hd->bytes == 1500000 &&
             hd->path == dlc2::ActiveFolder(l.ext1, kBf3) / "4541095000000000",
         "the HD textures on ext1 are listed On, with ext1's root, name and size");
  const auto* off = Named(found, "Premium", "Internal storage");
  Expect(off && off->state == dlc2::State::disabled && off->root == l.internal, "an Off package of the internal folder");
  Expect(Named(found, "Back to Karkand", "Internal storage") && Named(found, "Back to Karkand", "M.2 SSD / extended storage (ext1)") &&
             Count(found, "Back to Karkand") == 2,
         "the same file on two drives is listed once for each, the dlc folder's copy not at all");
  const auto* fresh = Named(found, "Close Quarters", "");
  Expect(fresh && fresh->state == dlc2::State::available && fresh->root.empty(), "a dlc folder's new package: Not installed, no drive");
  Expect(found.items.size() == 5 && found.other_game == 1, "five packages; another game's counted, not listed");
  Expect(found.items.front().state == dlc2::State::active && found.items.back().state == dlc2::State::available,
         "On first, Not installed last");
  // The one-root Find (the internal folder only) as before: ext1 is not looked at.
  const dlc2::Found internal_only = dlc2::Find(kBf3, l.internal, {l.dlc});
  Expect(!Named(internal_only, "HD Content", "M.2 SSD / extended storage (ext1)") && Count(internal_only, "HD Content") == 0,
         "Find with one root does not see ext1");
  // RootsOf: each root once, empty ones left out, in the scan's order.
  struct FakeDevice { stdfs::path root; std::string name; };
  const std::vector<FakeDevice> devices = {{l.internal, "a"}, {"", "b"}, {l.ext1, "c"}, {l.ext1, "d"}};
  const auto roots = dlc2::RootsOf(devices, [](const FakeDevice& d) { return d.name; });
  Expect(roots.size() == 2 && roots[0].path == l.internal && roots[0].device == "a" && roots[1].path == l.ext1 &&
             roots[1].device == "c",
         "RootsOf: each device's root once, in order, empty roots left out");
  Expect(dlc2::SizeText(1500000000) == "1.5 GB" && dlc2::SizeText(15400000000ull) == "15 GB" &&
             dlc2::SizeText(812300000) == "812 MB" && dlc2::SizeText(640000) == "640 KB" && dlc2::SizeText(10) == "1 KB",
         "sizes in decimal units");
}

void CheckRemove(const stdfs::path& base) {
  const Layout l = MakeLayout(base);
  dlc2::Found found = dlc2::FindAll(kBf3, Roots(l), {l.dlc});
  const dlc2::Item hd = *Named(found, "HD Content", "M.2 SSD / extended storage (ext1)");
  // A running game: refused, the file stays.
  Expect(dlc2::Remove(hd, kBf3, true) == dlc2::Removal::game_running, "refused while a game is running");
  Expect(stdfs::exists(hd.path), "the package is still there after the refusal");
  // Not a dlc folder's package, never a path outside the item's folder.
  const dlc2::Item fresh = *Named(found, "Close Quarters", "");
  Expect(dlc2::Remove(fresh, kBf3, false) == dlc2::Removal::not_installed && stdfs::exists(fresh.path),
         "a dlc folder's package is never removed");
  dlc2::Item outside = hd;
  outside.path = l.dlc / "map_pack_1";
  outside.file = "map_pack_1";
  Expect(dlc2::Remove(outside, kBf3, false) == dlc2::Removal::not_installed && stdfs::exists(outside.path),
         "a path outside the item's own folder is never removed");
  dlc2::Item folder = hd;
  folder.path = dlc2::ActiveFolder(l.ext1, kBf3);
  folder.file.clear();
  Expect(dlc2::Remove(folder, kBf3, false) == dlc2::Removal::not_installed && stdfs::is_directory(folder.path),
         "the folder itself is never removed");
  Expect(dlc2::Remove(hd, 0x4D530A87, false) == dlc2::Removal::not_installed && stdfs::exists(hd.path),
         "another title's folder is not this package's");
  // The chosen package only.
  dlc2::RemoveStep refused;
  Expect(dlc2::Remove(outside, kBf3, false, &refused) == dlc2::Removal::not_installed &&
             refused.call == "check: not in its own folder" && refused.error == 0,
         "a refused check says which one");
  dlc2::RemoveStep step;
  Expect(dlc2::Remove(hd, kBf3, false, &step) == dlc2::Removal::removed && step.call == "unlink" && step.error == 0 &&
             dlc2::Describe(step) == "unlink",
         "removed when no game runs, by unlink");
  Expect(!stdfs::exists(hd.path), "the package file is gone");
  Expect(stdfs::is_directory(dlc2::ActiveFolder(l.ext1, kBf3)), "its folder stays");
  Expect(stdfs::exists(dlc2::ActiveFolder(l.ext1, kBf3) / "map_pack_1") &&
             stdfs::exists(dlc2::ActiveFolder(l.internal, kBf3) / "map_pack_1") &&
             stdfs::exists(dlc2::DisabledFolder(l.internal, kBf3) / "premium_pack") && stdfs::exists(l.dlc / "map_pack_1"),
         "every other package stays (its neighbour, the other drive's, the Off one, the dlc folder's)");
  Expect(dlc2::Remove(hd, kBf3, false) == dlc2::Removal::missing, "a second Remove finds nothing to remove");
  // Removing the internal copy of a package that ext1 also has leaves ext1's, and the dlc folder's
  // copy stays Not installed only while no drive has it.
  found = dlc2::FindAll(kBf3, Roots(l), {l.dlc});
  const dlc2::Item karkand = *Named(found, "Back to Karkand", "Internal storage");
  Expect(dlc2::Remove(karkand, kBf3, false) == dlc2::Removal::removed && !stdfs::exists(karkand.path) &&
             stdfs::exists(dlc2::ActiveFolder(l.ext1, kBf3) / "map_pack_1"),
         "the internal copy goes, ext1's stays");
  found = dlc2::FindAll(kBf3, Roots(l), {l.dlc});
  Expect(Count(found, "Back to Karkand") == 1 && Named(found, "Back to Karkand", "M.2 SSD / extended storage (ext1)"),
         "after it, the map pack is listed once, on ext1");
  // An Off package is removed from the Off folder.
  const dlc2::Item premium = *Named(found, "Premium", "Internal storage");
  Expect(dlc2::Remove(premium, kBf3, false) == dlc2::Removal::removed && !stdfs::exists(premium.path) &&
             stdfs::is_directory(dlc2::DisabledFolder(l.internal, kBf3)),
         "an Off package is removed, its folder stays");
  // Turning off a package on ext1 keeps it on ext1 (a rename on its own drive).
  const dlc2::Item ext_pack = *Named(found, "Back to Karkand", "M.2 SSD / extended storage (ext1)");
  Expect(dlc2::Move(ext_pack, l.internal, kBf3, false) && stdfs::exists(dlc2::DisabledFolder(l.ext1, kBf3) / "map_pack_1") &&
             !stdfs::exists(dlc2::DisabledFolder(l.internal, kBf3) / "map_pack_1"),
         "Off moves an ext1 package within ext1");
}

void CheckSwitch(const stdfs::path& base) {
  const Layout l = MakeLayout(base);
  dlc2::Found found = dlc2::FindAll(kBf3, Roots(l), {l.dlc});
  const dlc2::Item hd = *Named(found, "HD Content", "M.2 SSD / extended storage (ext1)");
  dlc2::RemoveStep step;
  Expect(dlc2::Switch(hd, l.internal, kBf3, false, true, &step) == dlc2::Switched::game_running &&
             step.call == "check: a game is running" && stdfs::exists(hd.path) &&
             !stdfs::exists(dlc2::DisabledFolder(l.ext1, kBf3) / hd.file),
         "Off is refused while a game is running; the package stays On");
  Expect(dlc2::Switch(hd, l.internal, kBf3, false, false, &step) == dlc2::Switched::switched && step.call == "rename" &&
             stdfs::exists(dlc2::DisabledFolder(l.ext1, kBf3) / hd.file) && !stdfs::exists(hd.path) &&
             stdfs::is_directory(dlc2::ActiveFolder(l.ext1, kBf3)),
         "Off with no game: renamed into ext1's 00000002-off, its 00000002 folder stays");
  // The console's state after 109b: the game made the package again in 00000002 while the Off copy
  // was there. Both rows are listed; neither switches; removing one lets the other switch.
  Package(dlc2::ActiveFolder(l.ext1, kBf3) / hd.file, kBf3, 0x00000002, "HD Content", 1500000);
  found = dlc2::FindAll(kBf3, Roots(l), {l.dlc});
  int on = 0, off = 0;
  for (const auto& item : found.items)
    if (item.name == "HD Content") (item.state == dlc2::State::active ? on : off) += 1;
  Expect(on == 1 && off == 1, "a file both On and Off on one drive: two rows");
  const dlc2::Item* active = nullptr;
  const dlc2::Item* disabled = nullptr;
  for (const auto& item : found.items)
    if (item.name == "HD Content") (item.state == dlc2::State::active ? active : disabled) = &item;
  Expect(active && disabled && dlc2::Switch(*active, l.internal, kBf3, false, false, &step) == dlc2::Switched::twin &&
             dlc2::Switch(*disabled, l.internal, kBf3, true, false) == dlc2::Switched::twin &&
             stdfs::exists(active->path) && stdfs::exists(disabled->path),
         "neither copy switches while the other folder has the file (twin), both stay");
  Expect(Has(step.call, "the other folder has this file too"), "the twin refusal says why");
  Expect(dlc2::Remove(*disabled, kBf3, false) == dlc2::Removal::removed &&
             dlc2::Switch(*active, l.internal, kBf3, false, false) == dlc2::Switched::switched,
         "after removing the Off copy, the On one switches off");
  dlc2::Item available = *Named(found, "Close Quarters", "");
  Expect(dlc2::Switch(available, l.internal, kBf3, true, false) == dlc2::Switched::failed && stdfs::exists(available.path),
         "a dlc folder's package is not switched (it is installed by copying)");
}

// A folder that refuses the unlink (no write permission on it): failed, the step and errno said,
// the file kept. POSIX hosts not run as root (root may unlink anyway).
void CheckRefusedUnlink(const stdfs::path& base) {
#ifndef _WIN32
  if (::geteuid() == 0) {
    std::printf("skip: the refused unlink (run as root)\n");
    return;
  }
  const Layout l = MakeLayout(base);
  const dlc2::Found found = dlc2::FindAll(kBf3, Roots(l), {l.dlc});
  const dlc2::Item hd = *Named(found, "HD Content", "M.2 SSD / extended storage (ext1)");
  const stdfs::path folder = dlc2::ActiveFolder(l.ext1, kBf3);
  stdfs::permissions(folder, stdfs::perms::owner_read | stdfs::perms::owner_exec);
  dlc2::RemoveStep step;
  const dlc2::Removal removal = dlc2::Remove(hd, kBf3, false, &step);
  stdfs::permissions(folder, stdfs::perms::owner_all);
  Expect(removal == dlc2::Removal::failed && step.call == "unlink" && step.error == EACCES,
         "a folder that refuses the unlink: failed at unlink with EACCES (got " + dlc2::Describe(step) + ")");
  Expect(Has(dlc2::Describe(step), "unlink failed: errno " + std::to_string(EACCES) + " ("), "the log names the call and errno");
  Expect(stdfs::exists(hd.path), "the file is kept when the unlink is refused");
#else
  (void)base;
  std::printf("skip: the refused unlink (no POSIX permissions on this host)\n");
#endif
}

#ifdef CHECK_DLC_SCAN
namespace storage = xe::kernel::xam::storage;
// The real folders under a temporary /mnt; mount points and filesystems as the console's.
struct TempFs {
  std::set<std::string> mounts, read_only;
  bool IsMountPoint(const stdfs::path& path) { return mounts.count(path.string()) != 0; }
  bool IsDirectory(const stdfs::path& path) { return stdfs::is_directory(path); }
  bool ProbeWritable(const stdfs::path& path) { return !read_only.count(path.string()) && stdfs::is_directory(path); }
  storage::FsInfo Info(const stdfs::path& path) {
    storage::FsInfo info;
    info.ok = stdfs::exists(path);
    info.dev = std::hash<std::string>()(path.string());
    return info;
  }
  bool Exists(const stdfs::path& path) { return stdfs::exists(path); }
  std::vector<std::string> ListDirectories(const stdfs::path& path) {
    std::vector<std::string> names;
    std::error_code ec;
    for (stdfs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_directory(ec)) names.push_back(it->path().filename().string());
    return names;
  }
  bool ListNames(const stdfs::path& path, std::vector<std::string>& names) {
    std::error_code ec;
    stdfs::directory_iterator it(path, ec), end;
    if (ec) return false;
    for (; !ec && it != end; it.increment(ec)) names.push_back(it->path().filename().string());
    return true;
  }
};

void CheckScan(const stdfs::path& base) {
  const Layout l = MakeLayout(base);
  std::error_code ec;
  stdfs::create_directories(base / "mnt" / "usb0" / "music", ec);
  TempFs fs;
  fs.mounts = {(base / "mnt" / "ext1").string(), (base / "mnt" / "usb0").string()};
  fs.read_only = {(base / "mnt" / "ext1").string(), (base / "mnt" / "usb0").string()};
  const auto devices = storage::Scan(fs, l.internal, base / "mnt");
  const auto roots = dlc2::RootsOf(devices, [](const storage::Device& device) {
    storage::Device shown = device;
    shown.space_known = false;
    return storage::Label(shown, true, [](const char* english) { return english; });
  });
  Expect(roots.size() == 2 && roots[0].path == l.internal && roots[0].device == "Internal storage" &&
             roots[1].path == l.ext1 && roots[1].device == "M.2 SSD / extended storage (ext1)",
         "the scan's roots: the internal folder, then ext1's xbox360/PS5X360Reloaded/saves (usb0 not writable)");
  const dlc2::Found found = dlc2::FindAll(kBf3, roots, {l.dlc});
  Expect(Named(found, "HD Content", "M.2 SSD / extended storage (ext1)") != nullptr,
         "through the scan, the HD textures on ext1 are listed with their drive");
}
#endif

void CheckLauncher() {
  const std::string launcher = Read("src/launcher.cpp");
  const std::string roots = Body(launcher, "std::vector<dlc2::Root> DlcRoots(const fs::path& internal) {");
  Expect(Has(roots, "devices = storage::Scan(posix, internal, \"/mnt\");") && Has(roots, "dlc2::RootsOf(devices,") &&
             Has(roots, "shown.space_known = false;") && Has(roots, "storage::Label(shown, true,"),
         "DlcRoots: the storage scan's devices under /mnt, named as the where-to-save window names them");
  const std::string refresh = Body(launcher, "void Launcher::RefreshDlcs() {");
  Expect(Has(refresh, "DlcPage() = dlc2::FindAll(title, DlcRoots(save_root_), inboxes);"), "the tab lists every save device");
  const std::string take = Body(launcher, "bool Launcher::TakeLaunch(GameEntry& game) {");
  Expect(Has(take, "dlc2::FindAll(title, DlcRoots(save_root_), inboxes).items"),
         "Play does not install again what a drive already has");
  Expect(Has(launcher, "if (!busy && key == Key::triangle && rows) {") &&
             Has(launcher, "if (item.state != dlc2::State::available && !item.root.empty()) {\n"
                           "            if (!g_running_path.empty()) {\n"
                           "              dlc_result_ = \"running\";\n"
                           "            } else {\n"
                           "              g_dlc_remove_open = true;"),
         "Triangle on an installed package opens the window only while no game is running");
  const std::string press = Body(launcher, "void Launcher::Press(Key key) {");
  Expect(Has(press, "if (g_dlc_remove_open) {") &&
             Has(press, "dlc2::Remove(item, g_dlc_remove_title, !g_running_path.empty() || dlc2::copier().Busy(), &step);"),
         "X in the window removes, refused while a game runs or a copy is going on");
  Expect(Has(press, "removal == dlc2::Removal::removed ? \"removed\" : \"not removed\",\n             dlc2::Describe(step));"),
         "the log line says the step and errno");
  // stat and unlink only on the console: no lstat (symlink_status) and no libc remove() there.
  const std::string header = Read("port/dlc_manager.hpp");
  const std::string remove = Body(header, "inline Removal Remove(const Item& item, uint32_t title, bool game_running, RemoveStep* why = nullptr) {");
  Expect(!remove.empty() && !Has(remove, "symlink_status") && Has(remove, "fs::status(item.path, ec);  // stat") &&
             Has(remove, "#ifdef _WIN32\n  fs::remove(item.path, ec);  // the host checks on Windows\n") &&
             Has(remove, "#else\n  const int error = ::unlink(item.path.c_str()) == 0 ? 0 : errno;\n#endif"),
         "Remove: stat and unlink on the console, never lstat or remove()");
  Expect(press.find("if (g_dlc_remove_open) {") < press.find("if (g_confirm_open) {"), "the window takes the pad first");
  const std::string draw = Body(launcher, "void Launcher::Draw(ImGuiIO& io) {");
  Expect(Has(draw, "Tr(\"Remove DLC?\")") && Has(draw, "c.Text(item.name,") && Has(draw, "Tr(\"Size: %s\")") &&
             Has(draw, "Tr(\"Drive: %s\")") &&
             Has(draw, "Tr(\"The package file is deleted from this drive. This can't be undone.\")"),
         "the window says the name, size and drive, and that it cannot be undone");
  Expect(Has(draw, "c.Raised('x', Tr(\"Remove\"), x + 48, y + h - 96, true);") && Has(draw, "c.Raised('o', Tr(\"Cancel\"), button,"),
         "the window's raised buttons: X Remove (red), O Cancel");
  Expect(Has(launcher, "float Raised(char button, const std::string& label, float x, float y, bool danger = false) const {"),
         "the canvas draws raised buttons");
  const std::string tab = Body(launcher, "void Launcher::DrawDlcs(Canvas& c, float x) {");
  Expect(Has(tab, "dlc2::SizeText(item.bytes) + (item.device.empty() ? std::string() : \"  \\u00b7  \" + item.device)"),
         "each row shows its size and drive");
  Expect(Has(tab, "if (state != dlc2::State::available) hint = c.Hint('t', Tr(\"Remove\"), hint, 1004);"),
         "the Triangle hint for an installed package");
  Expect(Has(tab, "Tr(\"DLC removed.\")") && Has(tab, "Tr(\"Could not remove the DLC file.\")") &&
             Has(tab, "Tr(\"Close the game first: DLC can't be changed while a game is running.\")") &&
             Has(tab, "Tr(\"This drive has this DLC both On and Off: remove one of the two first.\")"),
         "the results are said");
  // (d) X: refused while a game runs, else install, on or off, each logged.
  const size_t x_key = press.find("if (!busy && key == Key::cross && rows && !save_root_.empty()) {", press.find("if (game_tab_ == 4) {"));
  const std::string x_body = x_key == std::string::npos ? "" : press.substr(x_key, 2400);
  const size_t running = x_body.find("if (!g_running_path.empty()) {");
  const size_t install = x_body.find("dlc2::copier().Start(item, save_root_, title)");
  const size_t toggle = x_body.find("dlc2::Switch(item, save_root_, title, enable, false, &step);");
  Expect(running != std::string::npos && install != std::string::npos && toggle != std::string::npos && running < install &&
             running < toggle && Has(x_body, "result = \"running\";"),
         "X installs or switches only while no game runs");
  Expect(Has(x_body, ": not changed (a game is running)\"") && Has(x_body, "result.empty() ? \"installing\" : \"not installed (the copy did not start)\"") &&
             Has(x_body, "(enable ? \"switched on\" : \"switched off\") : \"not switched\",\n                   dlc2::Describe(step));") &&
             Has(x_body, ": switched == dlc2::Switched::twin ? \"twin\" : \"move-failed\";"),
         "every DLC change and refusal is logged");
  // (f, g) The shared tab bar, its pad, and the lists' focus.
  const std::string sheet = Body(launcher, "void Launcher::DrawGameSheet(Canvas& c) {");
  Expect(Has(sheet, "c.Fill(x + 24, 256, 812, tab_bar::kBarHeight, 0x1a1f28, 0.96f, 14);") &&
             Has(sheet, "tab_bar::Draw(c, x + 24, x + 836, 256, tabs, game_tab_, g_list_focus_tab != game_tab_);") &&
             !Has(sheet, "L1 / R1") && !Has(launcher, "tab_pills"),
         "the game sheet draws the shared bar on a dark band, the open tab on green while the bar has the pad");
  Expect(Has(launcher, "#include \"xbox360ps5/tab_bar.hpp\"") &&
             Has(launcher, "void Triangle(float ax, float ay, float bx, float by, float cx, float cy, uint32_t rgb, float a = 1.0f) const {"),
         "the canvas is tab_bar's painter (Fill, Text, Width, Triangle)");
  Expect(Has(launcher, "  game_tab_ = tab;\n  g_list_focus_tab = -1;  // fix 109: the tab bar first\n"),
         "a sheet opens on its tab bar");
  Expect(Has(press, "const tab_bar::Effect effect = tab_bar::Press(nav, pad, 7, list_tab ? nav_rows : 0, has_value);") &&
             Has(press, "if (key == Key::l1 || key == Key::r1 || key == Key::left || key == Key::right ||\n"
                        "          (list_tab && (key == Key::up || key == Key::down || (key == Key::cross && on_bar)))) {") &&
             Has(press, "game_info_scroll = 0; g_list_focus_tab = -1; game_tab_ = nav.tab;") &&
             Has(press, "g_list_focus_tab = nav.on_bar ? -1 : game_tab_;\n          nav_row = nav.row;"),
         "the game sheet's pad goes through tab_bar::Press (L1/R1 and left/right everywhere, up/down and X on the bar in lists)");
  // Fix 149b: a subject's list of sections has no values (X opens a section).
  Expect((Has(press, "return game_tab_ == 2 && (game_category_open_ || (nav_identified && n == int(game_categories_.size())));") ||
          Has(press, "return game_tab_ == 2 && (game_category_open_ ? !GameSectionList() : nav_identified && n == int(game_categories_.size()));")) &&
             Has(press, "ChangeGameOption(Options()[size_t(game_rows_[size_t(game_option_row_)])], effect.value_step);"),
         "left/right change only a subject's options and the bug report row");
  const size_t nav_at = press.find("      // Fix 109d: the pad of the tab bar the in-game guide shares");
  Expect(nav_at != std::string::npos && nav_at < press.find("if (game_tab_ == 6) {") && nav_at < press.find("if (game_tab_ == 0) {"),
         "the pad rule comes before every tab's own keys");
  for (const char* gone : {"dlc_row_ = (dlc_row_ + 1) % rows;", "patch_row_ = (patch_row_ + 1) % rows;",
                           "achievement_row_ = (achievement_row_ + 1) % rows;", "sheet.row = (sheet.row + 1) % rows;",
                           "game_option_row_ = (game_option_row_ + 1) % own_rows;", "game_category_ = (game_category_ + 1) % categories;",
                           "if (key == Key::right) game_tab_ = 1;"})
    Expect(!Has(press, gone), std::string("no tab moves its rows or tabs by itself any more: ") + gone);
  for (const char* highlight : {"const bool focused = n == patch_row_ && g_list_focus_tab == 1;",
                                "const bool focused = n == game_category_ && g_list_focus_tab == 2;",
                                "const bool row_focused = game_category_ == row && g_list_focus_tab == 2;",
                                "const bool focused = n == game_option_row_ && g_list_focus_tab == 2;",
                                "n == achievement_row_ && g_list_focus_tab == 3 ? kSurfaceHigh : kSurface",
                                "const bool selected = n == dlc_row_ && g_list_focus_tab == 4;",
                                "const bool selected = n == sheet.row && g_list_focus_tab == 5;"})
    Expect(Has(launcher, highlight), std::string("a row is highlighted only once its list has the pad: ") + highlight);
  // Fix 149c: the game's option rows at 24 px (22 before).
  Expect((Has(launcher, "tab_bar::DrawValue(c, text.c_str(), x + 796, y + 10, 22, own ? kAccent : preset_here ? kBody : kFaint, 1.0f, true);") ||
          Has(launcher, "tab_bar::DrawValue(c, text.c_str(), x + 796, y + 9, 24, own ? kAccent : preset_here ? kBody : kFaint, 1.0f, true);")) &&
             Has(launcher, "tab_bar::DrawValue(c, value.c_str(), x + 796, y + 8, 24, on ? kWarning : focused ? kAccent : kMuted, 1.0f, true);"),
         "a subject's options and the bug report row show < value >");
}

// (g) xbox360ps5/tab_bar.hpp itself.
namespace tb = xbox360ps5::tab_bar;
struct FakeWidth {
  float operator()(const char* text, float size) const { return float(std::string(text).size()) * size * 0.5f; }
};
void CheckTabBar() {
  const std::vector<std::string> few = {"Game", "Settings"};
  const tb::Layout roomy = tb::Arrange(0, 1096, 100, few, FakeWidth());
  Expect(roomy.tab_text == tb::kTabText && roomy.tab_gap == tb::kTabGap, "tabs that fit keep 28 px and 44 px gaps");
  Expect(roomy.l1.x == tb::kEdge && roomy.r1.x + roomy.r1.w == 1096 - tb::kEdge && roomy.l1.h == tb::kPillHeight &&
             roomy.l1.y == 100 + (tb::kBarHeight - tb::kPillHeight) / 2,
         "L1 and R1 sit kEdge in from the bar's ends, centred in its band");
  const std::vector<std::string> sheet = {"Graj", "Łatki", "Ustawienia", "Osiągnięcia", "DLC", "Aktualizacje", "O programie"};
  const tb::Layout tight = tb::Arrange(0, 812, 0, sheet, FakeWidth());
  const tb::Box& last = tight.tabs.back();
  Expect(tight.tab_text < tb::kTabText && tight.tab_gap < tb::kTabGap && last.x + last.w + tight.tab_gap <= tight.r1.x + 0.01f &&
             tight.tabs.front().x >= tight.l1.x + tight.l1.w,
         "seven Polish tab names in 812 px shrink to fit between the pills");
  Expect(tight.l1.w == roomy.l1.w && tight.l1.h == tb::kPillHeight, "the pills keep their size when the tabs shrink");
  // The pad.
  const auto none = [](int) { return false; };
  const auto all = [](int) { return true; };
  tb::Nav nav;
  Expect(nav.on_bar && nav.tab == 0, "a tab opens with the bar focused");
  tb::Effect e = tb::Press(nav, tb::Key::right, 7, 5, none);
  Expect(e.tab_changed && nav.tab == 1 && nav.on_bar, "on the bar, right switches to the next tab");
  e = tb::Press(nav, tb::Key::left, 7, 5, none);
  e = tb::Press(nav, tb::Key::left, 7, 5, none);
  Expect(nav.tab == 6 && nav.on_bar, "left wraps from the first tab to the last");
  e = tb::Press(nav, tb::Key::down, 7, 3, none);
  Expect(!nav.on_bar && nav.row == 0 && !e.tab_changed, "Down enters the list at its first row");
  e = tb::Press(nav, tb::Key::right, 7, 3, none);
  Expect(!e.tab_changed && e.value_step == 0 && nav.tab == 6, "in a list, right on a row without a value does nothing");
  e = tb::Press(nav, tb::Key::right, 7, 3, all);
  Expect(!e.tab_changed && e.value_step == 1 && nav.tab == 6, "in a list, right changes a row's value");
  tb::Press(nav, tb::Key::down, 7, 3, none);
  tb::Press(nav, tb::Key::down, 7, 3, none);
  tb::Press(nav, tb::Key::down, 7, 3, none);
  Expect(nav.row == 2, "Down stops at the last row");
  e = tb::Press(nav, tb::Key::cross, 7, 3, none);
  Expect(e.activate, "X on a row activates it");
  tb::Press(nav, tb::Key::up, 7, 3, none);
  tb::Press(nav, tb::Key::up, 7, 3, none);
  tb::Press(nav, tb::Key::up, 7, 3, none);
  Expect(nav.on_bar, "Up on the first row goes back to the bar");
  e = tb::Press(nav, tb::Key::cross, 7, 3, none);
  Expect(!nav.on_bar && nav.row == 0 && !e.activate, "X on the bar enters the list (it does not activate a row)");
  e = tb::Press(nav, tb::Key::r1, 7, 3, all);
  Expect(e.tab_changed && nav.tab == 0 && nav.on_bar && e.value_step == 0, "R1 in a list switches tabs and lands on the bar");
  e = tb::Press(nav, tb::Key::down, 7, 0, none);
  Expect(nav.on_bar, "an empty list is not entered");
  e = tb::Press(nav, tb::Key::circle, 7, 3, none);
  Expect(e.back, "Circle says back");
}

std::string Escaped(const std::string& text) {
  std::string out;
  for (char c : text) out += c == ' ' ? std::string("\\s") : std::string(1, c);
  return out;
}
void CheckLanguages(const stdfs::path& folder) {
  const char* const codes[] = {"en", "pt-BR", "pt", "es", "es-419", "de", "fr", "it",
                               "ja", "ko", "nl", "pl", "ru", "tr", "zh-Hans", "zh-Hant"};
  const char* const keys[] = {"Remove DLC?", "Size: %s", "Drive: %s",
                              "The package file is deleted from this drive. This can't be undone.",
                              "DLC removed.", "Could not remove the DLC file.",
                              "Close the game first: DLC can't be changed while a game is running.",
                              "This drive has this DLC both On and Off: remove one of the two first.", "Navigate",
                              "Remove", "Cancel", "Internal storage", "M.2 SSD / extended storage", "USB %s"};
  for (const char* code : codes) {
    const std::string text = "\n" + Read(folder / (std::string(code) + ".txt"));
    Expect(text.size() > 1000, std::string(code) + ".txt is there");
    for (const char* key : keys) {
      const std::string line = "\n" + Escaped(key) + " = ";
      const size_t at = text.find(line);
      Expect(at != std::string::npos, std::string(code) + ".txt translates \"" + key + "\"");
      if (at == std::string::npos) continue;
      const std::string value = text.substr(at + line.size(), text.find('\n', at + line.size()) - at - line.size());
      if (Has(key, "%s")) Expect(Has(value, "%s"), std::string(code) + ".txt keeps %s in \"" + key + "\"");
      if (std::string(code) != "en" && std::string(key) == "Remove DLC?")
        Expect(value != Escaped(key), std::string(code) + ".txt: \"Remove DLC?\" is translated");
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  const stdfs::path base = stdfs::temp_directory_path() / "check-dlc-remove";
  CheckListing(base);
  CheckRemove(base);
  CheckRefusedUnlink(base);
  CheckSwitch(base);
#ifdef CHECK_DLC_SCAN
  CheckScan(base);
#else
  std::printf("skip: the storage scan part (no POSIX headers on this host)\n");
#endif
  CheckTabBar();
  if (stdfs::exists("src/launcher.cpp")) CheckLauncher();
  else std::printf("skip: launcher.cpp (not run from an assembled tree)\n");
  if (argc > 1) CheckLanguages(argv[1]);
  std::error_code ec;
  stdfs::remove_all(base, ec);
  if (failures) {
    std::printf("check-dlc-remove: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-dlc-remove: ok\n");
  return 0;
}
