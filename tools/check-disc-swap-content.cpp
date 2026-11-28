// SPDX-License-Identifier: MIT
// Fix 182 (tools/source-1.0.2/fixes/182-disc-swap-content.patch): disc packages after a disc swap.
// Grand Theft Auto V (545408A7) opens its four install packages again under the root "extra" after
// the swap from Disc 1 (Install) to Disc 2 (Play), on the device where its first pass found them.
// After an install made in the session that is the drive (device 2), and fix 148 looked for the
// packages only on game:, which then holds Disc 2: "refuse extra for device 2 ... result 00000003"
// four times, then the game waited for hours (research/fix148g-install/gta5-install-full.log
// :4361-4402, hang/hang-watch-5.txt).
//  (a) disc_content.hpp FindPackage: every path on the disc in the drive first, then on the discs
//      in the drive before a swap; a folder is not a package; the drive wins; GTA V's four "extra"
//      opens after the swap find Disc 1's packages (and none without the earlier discs, as before);
//      a name that cannot be a package asks no device;
//  (b) the file system (virtual_file_system.h/.cc): ResolvePathOnReplacedDevices resolves the
//      symbolic links like ResolvePath, then looks on the devices ReplaceDeviceRetainingOld kept,
//      the last replaced first, under the retired devices' lock; the disc swap keeps the old disc;
//  (c) the content manager: OpenContentFromGuestEntry mounts an entry already resolved and
//      OpenContentFromGuestFile (XamContentCreateFromGuestFile) resolves on the live devices only and
//      hands over to it;
//  (d) xam_content.cc: XamContentCreate on the optical drive looks with FindPackage on the drive and
//      then on the earlier discs, opens the entry found with OpenContentFromGuestEntry and logs
//      "Content (fix 148) ... (fix 182)" when it came from an earlier disc.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/disc_content.hpp"

namespace {
namespace fs = std::filesystem;
namespace dc = xbox360ps5::disc_content;

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

// A guest entry: a file or a folder on one disc.
struct FakeEntry {
  std::string disc;
  bool folder = false;
};

// A disc: guest path -> entry. The calls each device saw, in order.
struct FakeDiscs {
  std::map<std::string, FakeEntry> drive, earlier;
  std::vector<std::string> calls;
  dc::Found Find(const std::vector<std::string>& paths, FakeEntry*& entry, std::string& path,
                 bool earlier_discs = true) {
    return dc::FindPackage(
        paths,
        [&](const std::string& p) -> FakeEntry* {
          calls.push_back("drive " + p);
          auto it = drive.find(p);
          return it == drive.end() ? nullptr : &it->second;
        },
        [&](const std::string& p) -> FakeEntry* {
          calls.push_back("earlier " + p);
          if (!earlier_discs) return nullptr;
          auto it = earlier.find(p);
          return it == earlier.end() ? nullptr : &it->second;
        },
        [](FakeEntry* e) { return !e->folder; }, entry, path);
  }
};

std::string GtaPackage(int n) {
  return "game:\\content\\0000000000000000\\545408A7\\00000002\\545408A70000000" + std::to_string(n);
}

void Find() {
  {
    FakeDiscs discs;
    discs.drive[GtaPackage(0)] = {"disc 1"};
    discs.earlier[GtaPackage(0)] = {"disc 0"};
    FakeEntry* entry = nullptr;
    std::string path;
    const auto found = discs.Find(dc::GuestPackagePaths(0, 0x545408A7, 2, "545408A700000000"), entry, path);
    Expect(found == dc::Found::kDrive && entry && entry->disc == "disc 1" && path == GtaPackage(0),
           "(a) a package on the disc in the drive is found there (fix 148 as before)");
    Expect(discs.calls.size() == 1, "(a) found in the drive: no earlier disc asked");
  }
  {
    // GTA V after an install made in the session: Disc 2 in the drive, Disc 1 kept by the swap.
    FakeDiscs discs;
    for (int n = 0; n < 4; ++n) discs.earlier[GtaPackage(n)] = {"Disc 1 (Install)"};
    for (int n = 0; n < 4; ++n) {
      const std::string name = "545408A70000000" + std::to_string(n);
      FakeEntry* entry = nullptr;
      std::string path;
      Expect(discs.Find(dc::GuestPackagePaths(0, 0x545408A7, 2, name), entry, path) ==
                     dc::Found::kEarlierDisc &&
                 entry && entry->disc == "Disc 1 (Install)" && path == GtaPackage(n),
             "(a) GTA V's \"extra\" " + name + " after the swap opens from Disc 1");
      FakeEntry* before = nullptr;
      std::string before_path = "untouched";
      Expect(discs.Find(dc::GuestPackagePaths(0, 0x545408A7, 2, name), before, before_path,
                        /*earlier_discs=*/false) == dc::Found::kNone &&
                 !before && before_path == "untouched",
             "(a) " + name + " without the earlier discs: not found (the hang before fix 182)");
    }
  }
  {
    // The player's xuid and 0: every path on the drive before any earlier disc.
    FakeDiscs discs;
    const auto paths = dc::GuestPackagePaths(0xE000012345678901ull, 0x545408A7, 2, "545408A700000003");
    Expect(paths.size() == 2, "(a) two paths for a player's xuid");
    discs.earlier[paths[0]] = {"earlier xuid"};
    discs.drive[paths[1]] = {"drive zero"};
    FakeEntry* entry = nullptr;
    std::string path;
    Expect(discs.Find(paths, entry, path) == dc::Found::kDrive && entry->disc == "drive zero" &&
               path == paths[1],
           "(a) the drive's 0 path wins over an earlier disc's xuid path");
    Expect(discs.calls == std::vector<std::string>{"drive " + paths[0], "drive " + paths[1]},
           "(a) both drive paths asked, in order, and no earlier disc");
  }
  {
    FakeDiscs discs;
    const auto paths = dc::GuestPackagePaths(0xE000012345678901ull, 0x545408A7, 2, "545408A700000001");
    discs.earlier[paths[1]] = {"earlier zero"};
    FakeEntry* entry = nullptr;
    std::string path;
    Expect(discs.Find(paths, entry, path) == dc::Found::kEarlierDisc && entry->disc == "earlier zero" &&
               path == paths[1],
           "(a) an earlier disc's 0 path when the drive has neither");
    Expect(discs.calls == std::vector<std::string>{"drive " + paths[0], "drive " + paths[1],
                                                   "earlier " + paths[0], "earlier " + paths[1]},
           "(a) the drive's paths first, then the earlier discs' in the same order");
  }
  {
    // A folder of that name is not a package, on either device.
    FakeDiscs discs;
    discs.drive[GtaPackage(2)] = {"drive", true};
    discs.earlier[GtaPackage(2)] = {"earlier"};
    FakeEntry* entry = nullptr;
    std::string path;
    Expect(discs.Find(dc::GuestPackagePaths(0, 0x545408A7, 2, "545408A700000002"), entry, path) ==
                   dc::Found::kEarlierDisc &&
               entry->disc == "earlier",
           "(a) a folder in the drive is passed over for the earlier disc's file");
    discs.earlier[GtaPackage(2)].folder = true;
    entry = nullptr;
    path.clear();
    Expect(discs.Find(dc::GuestPackagePaths(0, 0x545408A7, 2, "545408A700000002"), entry, path) ==
                   dc::Found::kNone &&
               !entry && path.empty(),
           "(a) folders on both: no package");
  }
  {
    FakeDiscs discs;
    discs.earlier["game:\\content\\0000000000000000\\545408A7\\00000002\\.."] = {"earlier"};
    FakeEntry* entry = nullptr;
    std::string path;
    Expect(discs.Find(dc::GuestPackagePaths(0, 0x545408A7, 2, ".."), entry, path) == dc::Found::kNone &&
               discs.calls.empty(),
           "(a) a name that cannot be a package asks no device");
  }
}

size_t After(const std::string& text, const std::string& what, size_t from) {
  return from == std::string::npos ? std::string::npos : text.find(what, from);
}

void Tree() {
  const std::string vfs_h = Read(".deps/xenia-canary/src/xenia/vfs/virtual_file_system.h");
  const std::string vfs = Read(".deps/xenia-canary/src/xenia/vfs/virtual_file_system.cc");
  Expect(vfs_h.find("Entry* ResolvePathOnReplacedDevices(const std::string_view path);") != std::string::npos,
         "(b) VirtualFileSystem declares ResolvePathOnReplacedDevices");
  const size_t keep = vfs.find("bool VirtualFileSystem::ReplaceDeviceRetainingOld(");
  Expect(keep != std::string::npos && After(vfs, "retained.push_back(std::move(device));", keep) !=
                                          std::string::npos,
         "(b) the replaced device is kept");
  const size_t on = vfs.find("Entry* VirtualFileSystem::ResolvePathOnReplacedDevices(");
  const size_t on_end = After(vfs, "\n}\n", on);
  Expect(on != std::string::npos && on_end != std::string::npos, "(b) ResolvePathOnReplacedDevices is there");
  if (on != std::string::npos && on_end != std::string::npos) {
    const std::string body = vfs.substr(on, on_end - on);
    const size_t links = body.find("ResolveSymbolicLink(");
    const size_t lock = body.find("std::lock_guard lock(retired_mutex);");
    Expect(body.find("global_critical_region_.Acquire()") < links,
           "(b) under the file system's lock, as ResolvePath");
    Expect(body.find("canonicalize_guest_path(path)") < links, "(b) the path canonicalized first");
    Expect(links != std::string::npos && lock != std::string::npos && links < lock,
           "(b) symbolic links (game:) resolved, then the retired devices' lock taken");
    Expect(body.find("retired_devices.find(this)") != std::string::npos,
           "(b) only this file system's kept devices");
    Expect(body.find("rbegin()") != std::string::npos && body.find("rend()") != std::string::npos,
           "(b) the last replaced disc first");
    Expect(body.find("starts_with(normalized_path, device->mount_path())") != std::string::npos &&
               body.find("device->ResolvePath(relative_path)") != std::string::npos,
           "(b) a kept device under the path's mount resolves the rest of the path");
    Expect(body.find("devices_") == std::string::npos, "(b) the live devices are not searched again");
  }
  const std::string clear = vfs.substr(vfs.find("void VirtualFileSystem::Clear()"), 300);
  Expect(clear.find("retired_devices.erase(this);") != std::string::npos,
         "(b) Clear still drops the kept discs");

  const std::string swap = Read("port/multi_disc.cpp");
  Expect(swap.find("fs->ReplaceDeviceRetainingOld(std::move(accepted))") != std::string::npos,
         "(b) the disc swap replaces the drive and keeps the old disc");

  const std::string cm_h = Read(".deps/xenia-canary/src/xenia/kernel/xam/content_manager.h");
  const std::string cm = Read(".deps/xenia-canary/src/xenia/kernel/xam/content_manager.cc");
  Expect(cm_h.find("X_RESULT OpenContentFromGuestEntry(const std::string_view root_name,") !=
             std::string::npos,
         "(c) ContentManager declares OpenContentFromGuestEntry");
  const size_t file = cm.find("X_RESULT ContentManager::OpenContentFromGuestFile(");
  const size_t entry = cm.find("X_RESULT ContentManager::OpenContentFromGuestEntry(");
  Expect(file != std::string::npos && entry != std::string::npos && file < entry,
         "(c) both opens are defined");
  if (file != std::string::npos && entry != std::string::npos) {
    const std::string by_file = cm.substr(file, entry - file);
    Expect(by_file.find("file_system()->ResolvePath(guest_path)") != std::string::npos &&
               by_file.find("return OpenContentFromGuestEntry(root_name, entry, guest_path,") !=
                   std::string::npos,
           "(c) the guest-file open resolves on the live devices and hands over");
    Expect(by_file.find("ReplacedDevices") == std::string::npos,
           "(c) XamContentCreateFromGuestFile's path never reaches an earlier disc");
    const std::string by_entry = cm.substr(entry, cm.find("X_RESULT ContentManager::CloseContent(", entry) - entry);
    const size_t in_use = by_entry.find("IsContentOpen(root_name)");
    const size_t not_file = by_entry.find("kFileAttributeDirectory");
    const size_t map = by_entry.find("MapGuestFile(entry)");
    const size_t mount = by_entry.find("MountPackage(root_name, std::move(package))");
    Expect(by_entry.find("global_critical_region_.Acquire()") < in_use,
           "(c) the entry open holds the content lock");
    Expect(in_use != std::string::npos && not_file != std::string::npos && map != std::string::npos &&
               mount != std::string::npos && in_use < not_file && not_file < map && map < mount,
           "(c) root in use, then not a file, then mapped and mounted, as the guest-file open did");
    Expect(by_entry.find("!entry ||") != std::string::npos, "(c) a null entry is refused");
  }

  const std::string xam = Read(".deps/xenia-canary/src/xenia/kernel/xam/xam_content.cc");
  const size_t create = xam.find("dword_result_t xeXamContentCreate(");
  Expect(create != std::string::npos, "(d) xeXamContentCreate is there");
  const size_t optical = After(xam, "disc_content::kOpticalDrive", create);
  const size_t find = After(xam, "disc_content::FindPackage(", create);
  Expect(optical != std::string::npos && find != std::string::npos && optical < find,
         "(d) the optical drive's package is looked for with FindPackage");
  const size_t on_drive = After(xam, "file_system->ResolvePath(path)", find);
  const size_t on_earlier = After(xam, "file_system->ResolvePathOnReplacedDevices(path)", find);
  Expect(on_drive != std::string::npos && on_earlier != std::string::npos && on_drive < on_earlier,
         "(d) the drive first, then the discs kept by a swap");
  Expect(After(xam, "kFileAttributeDirectory", on_earlier) < After(xam, "disc_entry, disc_package);", find),
         "(d) a folder is not a package");
  const size_t open = After(xam, "OpenContentFromGuestEntry(", find);
  Expect(open != std::string::npos &&
             After(xam, "root_name, disc_entry, disc_package, content_license);", find) != std::string::npos,
         "(d) the entry found is opened (not resolved again on the drive)");
  Expect(xam.find("OpenContentFromGuestFile(\n          root_name, disc_package", create) == std::string::npos,
         "(d) no open by the drive path any more");
  Expect(After(xam, "Found::kEarlierDisc", open) != std::string::npos &&
             After(xam, "(fix 182)", open) != std::string::npos &&
             After(xam, "Content (fix 148)", open) != std::string::npos,
         "(d) an open from an earlier disc is logged with fix 182");
}

}  // namespace

int main() {
  Find();
  if (fs::exists(".deps/xenia-canary")) Tree();
  else std::printf("note: no assembled tree here, tree checks skipped\n");
  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("disc swap content: all checks passed\n");
  return 0;
}
