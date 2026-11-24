// SPDX-License-Identifier: MIT
// Fix 50 (tools/source-1.0.2/fixes/50-save-folder-reloaded.patch): the saves folder fix 27 makes on
// each external drive is named after the product, <drive>/PS5X360Reloaded/saves
// (XBOX360PS5_FOLDER_NAME since the rename to TrinityX360, the name until 1.0.5; app_identity.hpp),
// and a drive's old <drive>/PS5X360/saves
// is moved there by one rename() on the same filesystem before the drives are scanned.
//  (a) the decision and the steps (MigrateSaves) on a fake filesystem: old only (mkdir of the new
//      parent, rename, rmdir of the empty old folder), old folder with other files (kept), new
//      only (nothing), both (nothing moved, nothing merged, the new one used), neither (nothing),
//      a failed mkdir and a failed rename (nothing moved, the old folder still used by Scan),
//      an interrupted move (the new parent made, the rename not: finished next time), and
//      repeating it (nothing more to do);
//  (b) MigrateDrives over the console's mounts and bases (a drive's top, homebrew, xbox360),
//      placeholders and mounts without an old folder left alone, then Scan finds the new folders;
//      every base moved inside itself (<base>/PS5X360/saves -> <base>/PS5X360Reloaded/saves: the
//      mkdir, rename and rmdir in <base>, the same parent, so the same filesystem), and the
//      tester's M.2 drive as it is (/mnt/ext1 read-only at the top, the saves in
//      /mnt/ext1/xbox360/PS5X360/saves/E00098AB5214D8F8, the games beside them);
//  (c) the same on a real temporary tree with PosixFs (mkdir, rename, rmdir, stat): the files
//      inside arrive unchanged, the old empty folder is gone, a folder with other files is kept,
//      a regular file where the new folder should be makes the move fail and leaves all as it was;
//      a drive's xbox360/ and homebrew/ bases each moved inside themselves, its games left alone;
//  (d) the assembled tree: SaveRoot named after XBOX360PS5_FOLDER_NAME, the move before the scan
//      in content_manager.cc with the exact paths logged and the notice hook, the renamed text in
//      xam_ui.cc, canary_game_main.cpp and the window's list, and no lstat, remove_all or
//      weakly_canonical in the migration (the console's sandbox refuses lstat).
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/kernel/xam/storage_devices.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace storage = xe::kernel::xam::storage;
namespace stdfs = std::filesystem;
using Result = storage::Migration::Result;

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

// A fake filesystem with what MigrateSaves, MigrateDrives and Scan ask.
struct FakeFs {
  std::set<std::string> dirs, mounts, files, read_only;
  int fail_mkdir = 0, fail_rename = 0;  // errno to fail with, 0: succeed
  std::vector<std::string> calls;
  std::map<std::string, storage::FsInfo> info;

  void AddDir(const std::string& path) {
    for (stdfs::path p = path; !p.empty() && p != p.root_path(); p = p.parent_path()) dirs.insert(p.string());
  }
  void AddFile(const std::string& path) {
    AddDir(stdfs::path(path).parent_path().string());
    files.insert(path);
  }
  void Mount(const std::string& path, uint64_t dev) {
    AddDir(path);
    mounts.insert(path);
    info[path] = {true, dev, "", "", "", false, 0, 0};
  }
  bool HasChildren(const std::string& path) const {
    const std::string prefix = path + "/";
    for (const auto& d : dirs)
      if (d.compare(0, prefix.size(), prefix) == 0) return true;
    for (const auto& f : files)
      if (f.compare(0, prefix.size(), prefix) == 0) return true;
    return false;
  }
  // MigrateSaves.
  bool IsDirectory(const stdfs::path& path) { return dirs.count(path.string()) != 0; }
  bool Exists(const stdfs::path& path) { return dirs.count(path.string()) || files.count(path.string()); }
  bool MakeDirectory(const stdfs::path& path, int* error) {
    calls.push_back("mkdir " + path.string());
    if (fail_mkdir) {
      *error = fail_mkdir;
      return false;
    }
    if (files.count(path.string())) {
      *error = EEXIST;
      return false;
    }
    dirs.insert(path.string());
    return true;
  }
  bool Rename(const stdfs::path& from, const stdfs::path& to, int* error) {
    calls.push_back("rename " + from.string() + " " + to.string());
    if (fail_rename) {
      *error = fail_rename;
      return false;
    }
    const std::string a = from.string(), b = to.string();
    std::set<std::string> moved_dirs, moved_files;
    for (const auto& d : dirs) moved_dirs.insert(d == a || d.compare(0, a.size() + 1, a + "/") == 0 ? b + d.substr(a.size()) : d);
    for (const auto& f : files)
      moved_files.insert(f == a || f.compare(0, a.size() + 1, a + "/") == 0 ? b + f.substr(a.size()) : f);
    dirs = moved_dirs;
    files = moved_files;
    return true;
  }
  bool RemoveDirectory(const stdfs::path& path) {
    calls.push_back("rmdir " + path.string());
    if (!dirs.count(path.string()) || HasChildren(path.string())) return false;
    dirs.erase(path.string());
    return true;
  }
  // Scan.
  bool IsMountPoint(const stdfs::path& path) { return mounts.count(path.string()) != 0; }
  // A read-only folder is that folder only (a drive whose top cannot be written, its xbox360
  // folder can), as the console's probe writes a file in the folder itself.
  bool ProbeWritable(const stdfs::path& path) {
    if (read_only.count(path.string())) return false;
    return dirs.count(path.string()) != 0;
  }
  storage::FsInfo Info(const stdfs::path& path) {
    const auto found = info.find(path.string());
    return found == info.end() ? storage::FsInfo{true, 1, "", "", "", false, 0, 0} : found->second;
  }
  std::vector<std::string> ListDirectories(const stdfs::path& path) {
    std::vector<std::string> out;
    const std::string prefix = path.string() + "/";
    for (const auto& d : dirs)
      if (d.compare(0, prefix.size(), prefix) == 0 && d.find('/', prefix.size()) == std::string::npos)
        out.push_back(d.substr(prefix.size()));
    return out;
  }
  bool ListNames(const stdfs::path& path, std::vector<std::string>& names) {
    names = ListDirectories(path);
    const std::string prefix = path.string() + "/";
    for (const auto& f : files)
      if (f.compare(0, prefix.size(), prefix) == 0 && f.find('/', prefix.size()) == std::string::npos)
        names.push_back(f.substr(prefix.size()));
    return true;
  }
};

void CheckDecisions() {
  // The rename to TrinityX360 (fixes/trinityx360-name.patch) kept the folder: kSaveFolder is
  // XBOX360PS5_FOLDER_NAME, the old name, not the product name.
  Expect(std::string(XBOX360PS5_PRODUCT_NAME) == "TrinityX360" && std::string(XBOX360PS5_FOLDER_NAME) == "PS5X360Reloaded",
         "the product is TrinityX360, its drives' folders keep PS5X360Reloaded");
  Expect(std::string(storage::kSaveFolder) == "PS5X360Reloaded" && std::string(storage::kLegacySaveFolder) == "PS5X360",
         "the folder is the product's name, the old one fix 27's");
  Expect(storage::SaveRoot("/mnt/usb0") == "/mnt/usb0/PS5X360Reloaded/saves" &&
             storage::SaveRoot("/mnt/ext1", "xbox360") == "/mnt/ext1/xbox360/PS5X360Reloaded/saves" &&
             storage::LegacySaveRoot("/mnt/usb0") == "/mnt/usb0/PS5X360/saves",
         "SaveRoot and LegacySaveRoot");

  // Old only: mkdir, rename, rmdir; the files go with the folder.
  FakeFs fs;
  fs.AddFile("/mnt/usb0/PS5X360/saves/E000000000000001/4D5307E6/00000001/save.bin");
  auto m = storage::MigrateSaves(fs, "/mnt/usb0");
  Expect(m.result == Result::kMoved && m.old_folder_removed, "old only: moved, the empty old folder removed");
  Expect(m.from == "/mnt/usb0/PS5X360/saves" && m.to == "/mnt/usb0/PS5X360Reloaded/saves", "the exact paths");
  Expect((fs.calls == std::vector<std::string>{"mkdir /mnt/usb0/PS5X360Reloaded",
                                               "rename /mnt/usb0/PS5X360/saves /mnt/usb0/PS5X360Reloaded/saves",
                                               "rmdir /mnt/usb0/PS5X360"}),
         "mkdir, one rename, rmdir");
  Expect(fs.files.count("/mnt/usb0/PS5X360Reloaded/saves/E000000000000001/4D5307E6/00000001/save.bin") &&
             !fs.IsDirectory("/mnt/usb0/PS5X360"),
         "the save is in the new folder, the old folder is gone");
  // Repeated: nothing more.
  fs.calls.clear();
  Expect(storage::MigrateSaves(fs, "/mnt/usb0").result == Result::kNone && fs.calls.empty(), "repeated: nothing to do");

  // The old folder holds something else too: moved, the old folder kept.
  FakeFs other;
  other.AddDir("/mnt/usb1/PS5X360/saves");
  other.AddFile("/mnt/usb1/PS5X360/notes.txt");
  m = storage::MigrateSaves(other, "/mnt/usb1");
  Expect(m.result == Result::kMoved && !m.old_folder_removed && other.files.count("/mnt/usb1/PS5X360/notes.txt"),
         "an old folder with other files is kept");

  // New only, neither: nothing.
  FakeFs fresh;
  fresh.AddDir("/mnt/usb0/PS5X360Reloaded/saves");
  Expect(storage::MigrateSaves(fresh, "/mnt/usb0").result == Result::kNone && fresh.calls.empty(), "new only: nothing");
  FakeFs none;
  none.AddDir("/mnt/usb0");
  Expect(storage::MigrateSaves(none, "/mnt/usb0").result == Result::kNone && none.calls.empty(), "neither: nothing");

  // Both, as 1.0.4_candidate_14 left the tester's M.2 drive: the real Battlefield 3 save in the
  // new folder, a fresh profile save of the same title that the build without fix 50 wrote into
  // the old one, plus a title and a profile only the old folder has.
  {
    const std::string old_saves = "/mnt/ext1/xbox360/PS5X360/saves", new_saves = "/mnt/ext1/xbox360/PS5X360Reloaded/saves";
    FakeFs both;
    both.AddFile(new_saves + "/E00098AB5214D8F8/45410950/00000001/real.bin");
    both.AddFile(old_saves + "/E00098AB5214D8F8/45410950/00000001/PROF_SAVE/PROF_SAVE");
    both.AddFile(old_saves + "/E00098AB5214D8F8/45410950/Headers/00000001/PROF_SAVE");
    both.AddFile(old_saves + "/E00098AB5214D8F8/41560817/00000001/save.bin");
    both.AddFile(old_saves + "/E000000000000002/4D5307E6/00000001/save.bin");
    both.AddFile(old_saves + "/.ps5x360-write-test");
    m = storage::MigrateSaves(both, "/mnt/ext1/xbox360");
    Expect(m.result == Result::kMerged, "both: what only the old folder has is moved");
    Expect((m.merged == std::vector<std::pair<stdfs::path, stdfs::path>>{
                {old_saves + "/E000000000000002", new_saves + "/E000000000000002"},
                {old_saves + "/E00098AB5214D8F8/41560817", new_saves + "/E00098AB5214D8F8/41560817"}}),
           "both: a profile the new folder lacks, whole; a title the new profile lacks");
    Expect((m.kept == std::vector<stdfs::path>{old_saves + "/E00098AB5214D8F8/45410950"}) && m.failed.empty(),
           "both: the title the new folder has stays in the old one");
    Expect(both.files.count(new_saves + "/E00098AB5214D8F8/45410950/00000001/real.bin") &&
               !both.files.count(new_saves + "/E00098AB5214D8F8/45410950/00000001/PROF_SAVE/PROF_SAVE") &&
               both.files.count(old_saves + "/E00098AB5214D8F8/45410950/00000001/PROF_SAVE/PROF_SAVE") &&
               both.files.count(new_saves + "/E00098AB5214D8F8/41560817/00000001/save.bin") &&
               both.files.count(new_saves + "/E000000000000002/4D5307E6/00000001/save.bin") &&
               both.files.count(old_saves + "/.ps5x360-write-test"),
           "both: the real save untouched, nothing merged into it, the moved ones in the new folder");
    for (const auto& call : both.calls)
      Expect(call.find("45410950") == std::string::npos || call.rfind("rmdir", 0) == 0, "both: never renamed: " + call);
    Expect(!m.old_folder_removed && both.IsDirectory(old_saves), "both: the old folder stays while it holds something");
    // Repeated: nothing more moves.
    both.calls.clear();
    m = storage::MigrateSaves(both, "/mnt/ext1/xbox360");
    Expect(m.result == Result::kBothKept && m.merged.empty() && m.kept.size() == 1, "both, repeated: nothing more to move");
  }
  // Both, everything in the old folder missing from the new one: all moved, the old folder gone.
  {
    FakeFs both;
    both.AddFile("/mnt/usb0/PS5X360Reloaded/saves/a/new.bin");
    both.AddFile("/mnt/usb0/PS5X360/saves/a/old.bin");
    both.AddFile("/mnt/usb0/PS5X360/saves/b/other.bin");
    m = storage::MigrateSaves(both, "/mnt/usb0");
    Expect(m.result == Result::kMerged && m.merged.size() == 2 && m.kept.empty() && m.old_folder_removed &&
               both.files.count("/mnt/usb0/PS5X360Reloaded/saves/a/old.bin") &&
               both.files.count("/mnt/usb0/PS5X360Reloaded/saves/a/new.bin") &&
               both.files.count("/mnt/usb0/PS5X360Reloaded/saves/b/other.bin") && !both.IsDirectory("/mnt/usb0/PS5X360"),
           "both: a file of a shared profile and a whole profile moved, the empty old folders removed");
  }
  // Both, a rename refused: reported, nothing lost.
  {
    FakeFs both;
    both.AddDir("/mnt/usb0/PS5X360Reloaded/saves");
    both.AddFile("/mnt/usb0/PS5X360/saves/a/old.bin");
    both.fail_rename = EIO;
    m = storage::MigrateSaves(both, "/mnt/usb0");
    Expect(m.result == Result::kBothKept && m.failed.size() == 1 && m.failed[0].second == EIO &&
               both.files.count("/mnt/usb0/PS5X360/saves/a/old.bin"),
           "both: a refused rename is reported and leaves the file where it was");
  }
  // Both, the new saves path a file: untouched.
  {
    FakeFs both;
    both.AddDir("/mnt/usb0/PS5X360/saves/a");
    both.AddFile("/mnt/usb0/PS5X360Reloaded/saves");
    m = storage::MigrateSaves(both, "/mnt/usb0");
    Expect(m.result == Result::kBothKept && both.calls.empty(), "both: a file at the new path, nothing done");
  }

  // A failed mkdir, a failed rename: nothing moved.
  FakeFs no_mkdir;
  no_mkdir.AddDir("/mnt/usb0/PS5X360/saves");
  no_mkdir.fail_mkdir = EACCES;
  m = storage::MigrateSaves(no_mkdir, "/mnt/usb0");
  Expect(m.result == Result::kFailed && m.step == "mkdir" && m.error == EACCES && no_mkdir.IsDirectory("/mnt/usb0/PS5X360/saves"),
         "a failed mkdir moves nothing");
  FakeFs no_rename;
  no_rename.AddDir("/mnt/usb0/PS5X360/saves");
  no_rename.fail_rename = EXDEV;
  m = storage::MigrateSaves(no_rename, "/mnt/usb0");
  Expect(m.result == Result::kFailed && m.step == "rename" && m.error == EXDEV &&
             no_rename.IsDirectory("/mnt/usb0/PS5X360/saves") && !no_rename.IsDirectory("/mnt/usb0/PS5X360Reloaded/saves"),
         "a failed rename moves nothing");
  // Interrupted after the mkdir: the next time finishes it.
  FakeFs halfway;
  halfway.AddDir("/mnt/usb0/PS5X360/saves");
  halfway.AddDir("/mnt/usb0/PS5X360Reloaded");
  m = storage::MigrateSaves(halfway, "/mnt/usb0");
  Expect(m.result == Result::kMoved && halfway.IsDirectory("/mnt/usb0/PS5X360Reloaded/saves"), "an interrupted move is finished");
}

void CheckDrives() {
  FakeFs fs;
  fs.AddDir("/data/homebrew/PPSA83600/saves");
  for (const char* placeholder : {"/mnt/ext0", "/mnt/usb3"}) fs.AddDir(placeholder);
  fs.AddDir("/mnt/usb3/PS5X360/saves");  // a placeholder (not a mount point): never touched
  fs.Mount("/mnt/usb0", 201);
  fs.AddFile("/mnt/usb0/PS5X360/saves/x/1.bin");
  fs.Mount("/mnt/usb1", 202);
  fs.AddDir("/mnt/usb1/roms");  // no old folder
  fs.Mount("/mnt/ext1", 301);
  fs.read_only.insert("/mnt/ext1");
  fs.AddFile("/mnt/ext1/xbox360/PS5X360/saves/y/2.bin");
  fs.Mount("/mnt/usb2", 203);  // both
  fs.AddDir("/mnt/usb2/PS5X360/saves");
  fs.AddDir("/mnt/usb2/PS5X360Reloaded/saves");
  std::vector<storage::Migration> done;
  storage::MigrateDrives(fs, "/mnt", &done);
  std::map<std::string, Result> by;
  for (const auto& m : done) by[m.from.string()] = m.result;
  Expect(done.size() == 3, "three drives with an old folder (" + std::to_string(done.size()) + ")");
  Expect(by["/mnt/usb0/PS5X360/saves"] == Result::kMoved && by["/mnt/ext1/xbox360/PS5X360/saves"] == Result::kMoved &&
             by["/mnt/usb2/PS5X360/saves"] == Result::kBothKept,
         "moved on usb0 and ext1/xbox360, both kept on usb2");
  Expect(fs.IsDirectory("/mnt/usb3/PS5X360/saves"), "a placeholder is never touched");
  std::vector<storage::Skipped> skipped;
  const auto devices = storage::Scan(fs, "/data/homebrew/PPSA83600/saves", "/mnt", &skipped);
  std::map<uint32_t, std::string> roots;
  for (const auto& d : devices) roots[d.id] = d.root.string();
  Expect(roots[7] == "/mnt/usb0/PS5X360Reloaded/saves" && roots[4] == "/mnt/ext1/xbox360/PS5X360Reloaded/saves" &&
             roots[9] == "/mnt/usb2/PS5X360Reloaded/saves" && roots[8] == "/mnt/usb1/PS5X360Reloaded/saves",
         "the scan finds the new folders (and the new one where both are)");
  // A drive whose move failed keeps using its old folder.
  FakeFs stuck;
  stuck.Mount("/mnt/usb0", 201);
  stuck.AddDir("/mnt/usb0/PS5X360/saves");
  stuck.fail_rename = EIO;
  std::vector<storage::Migration> failed;
  storage::MigrateDrives(stuck, "/mnt", &failed);
  Expect(failed.size() == 1 && failed[0].result == Result::kFailed, "a failed move is reported");
  const auto kept = storage::Scan(stuck, "/data/homebrew/PPSA83600/saves", "/mnt");
  Expect(kept.size() == 2 && kept[1].root == "/mnt/usb0/PS5X360/saves", "after a failed move the old folder is used");
}

// Every base fix 27's scan accepts (the drive's top, homebrew/, xbox360/) is moved in its own
// folder: <base>/PS5X360/saves -> <base>/PS5X360Reloaded/saves, mkdir and rename inside <base>,
// so the rename never leaves the filesystem. And the tester's M.2 drive as it is: the top of
// /mnt/ext1 read-only, the saves in /mnt/ext1/xbox360/PS5X360/saves/<profile>.
void CheckBases() {
  FakeFs fs;
  fs.Mount("/mnt/usb0", 201);
  for (const char* base : {"", "/homebrew", "/xbox360"})
    fs.AddFile(std::string("/mnt/usb0") + base + "/PS5X360/saves/E000000000000001/4D5307E6/00000001/save.bin");
  std::vector<storage::Migration> done;
  storage::MigrateDrives(fs, "/mnt", &done);
  Expect(done.size() == 3, "one move a base (" + std::to_string(done.size()) + ")");
  for (const char* base : {"", "/homebrew", "/xbox360"}) {
    const std::string folder = std::string("/mnt/usb0") + base;
    bool found = false;
    for (const auto& m : done) {
      if (m.from.string() != folder + "/PS5X360/saves") continue;
      found = true;
      Expect(m.result == Result::kMoved && m.to.string() == folder + "/PS5X360Reloaded/saves" && m.old_folder_removed,
             "moved in its own base: " + m.from.string() + " -> " + m.to.string());
      Expect(m.from.parent_path().parent_path() == m.to.parent_path().parent_path(),
             "the same parent, so the same filesystem: " + folder);
    }
    Expect(found, "a move for " + folder + "/PS5X360/saves");
    Expect(std::find(fs.calls.begin(), fs.calls.end(), "mkdir " + folder + "/PS5X360Reloaded") != fs.calls.end() &&
               std::find(fs.calls.begin(), fs.calls.end(),
                         "rename " + folder + "/PS5X360/saves " + folder + "/PS5X360Reloaded/saves") != fs.calls.end() &&
               std::find(fs.calls.begin(), fs.calls.end(), "rmdir " + folder + "/PS5X360") != fs.calls.end(),
           "mkdir, rename and rmdir inside " + (folder.empty() ? std::string("the top") : folder));
    Expect(fs.files.count(folder + "/PS5X360Reloaded/saves/E000000000000001/4D5307E6/00000001/save.bin") == 1 &&
               !fs.IsDirectory(folder + "/PS5X360"),
           "the save arrives, the old folder is gone: " + folder);
  }

  FakeFs m2;
  m2.AddDir("/data/homebrew/PPSA83600/saves");
  m2.Mount("/mnt/ext1", 301);
  m2.read_only.insert("/mnt/ext1");
  m2.AddFile("/mnt/ext1/xbox360/PS5X360/saves/E00098AB5214D8F8/41560817/00000001/save.bin");
  m2.AddFile("/mnt/ext1/xbox360/Battlefield 3 (USA, Europe) (Disc 2).iso");
  std::vector<storage::Migration> moved;
  storage::MigrateDrives(m2, "/mnt", &moved);
  Expect(moved.size() == 1 && moved[0].result == Result::kMoved &&
             moved[0].from == "/mnt/ext1/xbox360/PS5X360/saves" && moved[0].to == "/mnt/ext1/xbox360/PS5X360Reloaded/saves" &&
             moved[0].old_folder_removed,
         "M.2: /mnt/ext1/xbox360/PS5X360/saves -> /mnt/ext1/xbox360/PS5X360Reloaded/saves, the old folder removed");
  Expect(m2.IsDirectory("/mnt/ext1/xbox360/PS5X360Reloaded/saves/E00098AB5214D8F8") &&
             m2.files.count("/mnt/ext1/xbox360/PS5X360Reloaded/saves/E00098AB5214D8F8/41560817/00000001/save.bin") == 1 &&
             !m2.IsDirectory("/mnt/ext1/xbox360/PS5X360") &&
             m2.files.count("/mnt/ext1/xbox360/Battlefield 3 (USA, Europe) (Disc 2).iso") == 1,
         "M.2: the profile folder arrives, the old folder is gone, the games beside it stay");
  const auto devices = storage::Scan(m2, "/data/homebrew/PPSA83600/saves", "/mnt");
  Expect(devices.size() == 2 && devices[1].id == 4 && devices[1].root == "/mnt/ext1/xbox360/PS5X360Reloaded/saves",
         "M.2: the scan uses the moved folder (device 4)");
}

void Write(const stdfs::path& path, const std::string& text) {
  stdfs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
std::string ReadText(const stdfs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void CheckRealTree() {
  const stdfs::path root = stdfs::temp_directory_path() / ("check-save-folder-" + std::to_string(::getpid()));
  stdfs::remove_all(root);
  storage::PosixFs fs;
  // Old only.
  const auto a = root / "a";
  Write(a / "PS5X360/saves/E000000000000001/4D5307E6/00000001/save.bin", "save data");
  auto m = storage::MigrateSaves(fs, a);
  Expect(m.result == Result::kMoved && m.old_folder_removed, "real: moved, old folder removed");
  Expect(ReadText(a / "PS5X360Reloaded/saves/E000000000000001/4D5307E6/00000001/save.bin") == "save data" &&
             !stdfs::exists(a / "PS5X360"),
         "real: the save arrives unchanged, the old folder is gone");
  Expect(storage::MigrateSaves(fs, a).result == Result::kNone, "real: repeated, nothing");
  // Old folder with other files: kept.
  const auto b = root / "b";
  Write(b / "PS5X360/saves/x.bin", "x");
  Write(b / "PS5X360/readme.txt", "r");
  m = storage::MigrateSaves(fs, b);
  Expect(m.result == Result::kMoved && !m.old_folder_removed && stdfs::exists(b / "PS5X360/readme.txt") &&
             ReadText(b / "PS5X360Reloaded/saves/x.bin") == "x",
         "real: an old folder with other files stays");
  // Both: what only the old folder has moves, what the new one has stays.
  const auto c = root / "c";
  Write(c / "PS5X360/saves/E00098AB5214D8F8/45410950/00000001/PROF_SAVE", "fresh");
  Write(c / "PS5X360/saves/E00098AB5214D8F8/41560817/00000001/save.bin", "mw2");
  Write(c / "PS5X360/saves/E000000000000002/4D5307E6/save.bin", "other profile");
  Write(c / "PS5X360Reloaded/saves/E00098AB5214D8F8/45410950/00000001/PROF_SAVE", "real");
  m = storage::MigrateSaves(fs, c);
  Expect(m.result == Result::kMerged && m.merged.size() == 2 && m.kept.size() == 1 && m.failed.empty() &&
             ReadText(c / "PS5X360Reloaded/saves/E00098AB5214D8F8/45410950/00000001/PROF_SAVE") == "real" &&
             ReadText(c / "PS5X360/saves/E00098AB5214D8F8/45410950/00000001/PROF_SAVE") == "fresh" &&
             ReadText(c / "PS5X360Reloaded/saves/E00098AB5214D8F8/41560817/00000001/save.bin") == "mw2" &&
             ReadText(c / "PS5X360Reloaded/saves/E000000000000002/4D5307E6/save.bin") == "other profile" &&
             !stdfs::exists(c / "PS5X360/saves/E000000000000002") && !m.old_folder_removed,
         "real: both: the title only the old folder has and its other profile moved, the real save untouched");
  Expect(storage::MigrateSaves(fs, c).result == Result::kBothKept, "real: both, repeated: nothing more");
  stdfs::remove_all(c / "PS5X360/saves/E00098AB5214D8F8/45410950");  // set aside by hand
  m = storage::MigrateSaves(fs, c);
  Expect(m.result == Result::kBothKept && m.old_folder_removed && !stdfs::exists(c / "PS5X360"),
         "real: both, once the old folder is empty: removed");
  // Neither.
  const auto d = root / "d";
  stdfs::create_directories(d);
  Expect(storage::MigrateSaves(fs, d).result == Result::kNone && !stdfs::exists(d / "PS5X360Reloaded"), "real: neither");
  // A regular file where the new folder should be: mkdir fails, nothing moves.
  const auto e = root / "e";
  Write(e / "PS5X360/saves/s.bin", "s");
  Write(e / "PS5X360Reloaded", "a file");
  m = storage::MigrateSaves(fs, e);
  Expect(m.result == Result::kFailed && m.step == "mkdir" && ReadText(e / "PS5X360/saves/s.bin") == "s",
         "real: a failed mkdir leaves everything as it was");
  // The new saves folder name taken by a file: both counted as present, nothing moved.
  const auto f = root / "f";
  Write(f / "PS5X360/saves/s.bin", "s");
  Write(f / "PS5X360Reloaded/saves", "a file");
  Expect(storage::MigrateSaves(fs, f).result == Result::kBothKept && ReadText(f / "PS5X360/saves/s.bin") == "s",
         "real: something already at the new path is never replaced");
  // The xbox360/ and homebrew/ bases of a drive, as MigrateDrives passes them: each moved inside
  // itself; the drive's other files stay.
  const auto g = root / "g";
  Write(g / "xbox360/PS5X360/saves/E00098AB5214D8F8/41560817/00000001/save.bin", "m2 save");
  Write(g / "xbox360/game.iso", "iso");
  Write(g / "homebrew/PS5X360/saves/E000000000000002/4D5307E6/00000001/save.bin", "homebrew save");
  for (const char* base : {"xbox360", "homebrew"}) {
    m = storage::MigrateSaves(fs, g / base);
    Expect(m.result == Result::kMoved && m.old_folder_removed && m.from == g / base / "PS5X360/saves" &&
               m.to == g / base / "PS5X360Reloaded/saves" && !stdfs::exists(g / base / "PS5X360"),
           std::string("real: ") + base + "/PS5X360/saves moved to " + base + "/PS5X360Reloaded/saves");
  }
  Expect(ReadText(g / "xbox360/PS5X360Reloaded/saves/E00098AB5214D8F8/41560817/00000001/save.bin") == "m2 save" &&
             ReadText(g / "homebrew/PS5X360Reloaded/saves/E000000000000002/4D5307E6/00000001/save.bin") == "homebrew save" &&
             ReadText(g / "xbox360/game.iso") == "iso" && !stdfs::exists(g / "PS5X360Reloaded"),
         "real: the saves arrive in their own base, the games stay, nothing at the drive's top");
  stdfs::remove_all(root);
}

std::string Read(const char* path) { return ReadText(path); }
void ExpectIn(const char* path, const std::vector<std::string>& parts) {
  const std::string text = Read(path);
  Expect(!text.empty(), std::string("read ") + path);
  for (const auto& part : parts) Expect(text.find(part) != std::string::npos, std::string(path) + " has: " + part);
}
void ExpectNotIn(const std::string& text, const std::string& what, const std::vector<std::string>& parts) {
  for (const auto& part : parts) Expect(text.find(part) == std::string::npos, what + " has no " + part);
}

void CheckTree() {
  const char* x = ".deps/xenia-canary/src/xenia/kernel/xam/";
  const std::string storage_h = Read((std::string(x) + "storage_devices.h").c_str());
  // Fix 50 named the folder after the product; the rename to TrinityX360 keeps it on the old name
  // (XBOX360PS5_FOLDER_NAME), so the saves already on each drive stay where they are.
  Expect(storage_h.find("inline constexpr const char kSaveFolder[] = XBOX360PS5_FOLDER_NAME;") != std::string::npos &&
             storage_h.find("kSaveFolder[] = XBOX360PS5_PRODUCT_NAME") == std::string::npos &&
             storage_h.find("return folder / kSaveFolder / \"saves\";") != std::string::npos,
         "SaveRoot keeps the drives' folder name (XBOX360PS5_FOLDER_NAME, PS5X360Reloaded)");
  const size_t begin = storage_h.find("void MergeSaves(Fs& fs, Migration& migration) {");
  const size_t end = storage_h.find("// Every mounted drive's bases", begin);
  Expect(begin != std::string::npos && end != std::string::npos && storage_h.find("Migration MigrateSaves(", begin) < end,
         "MergeSaves and MigrateSaves");
  const std::string migrate = storage_h.substr(begin, end - begin);
  ExpectNotIn(migrate, "MigrateSaves", {"lstat", "remove_all", "weakly_canonical", "copy", "symlink_status"});
  const size_t posix = storage_h.find("struct PosixFs");
  const std::string posix_fs = storage_h.substr(posix);
  ExpectNotIn(posix_fs, "PosixFs", {"lstat", "remove_all", "weakly_canonical"});
  for (const char* part : {"::mkdir(path.c_str(), 0777)", "::rename(from.c_str(), to.c_str())", "::rmdir(path.c_str())"})
    Expect(posix_fs.find(part) != std::string::npos, std::string("PosixFs has ") + part);
  Expect(storage_h.find("\"Each drive keeps its saves in its own PS5X360Reloaded/saves folder.\"") != std::string::npos,
         "the window's text names the new folder");
  const std::string content = Read((std::string(x) + "content_manager.cc").c_str());
  const size_t migrate_call = content.find("storage::MigrateDrives(fs, external_storage_root, &migrations);");
  const size_t scan_call = content.find("devices = storage::Scan(");
  Expect(migrate_call != std::string::npos && scan_call != std::string::npos && migrate_call < scan_call,
         "the move before the scan");
  for (const char* part : {"\"Storage: saves folder moved: {} -> {}{}\"",
                           "\"Storage: moved {} -> {} (only in the old saves folder)\"",
                           "\"Storage: kept {} in the old saves folder: {} has the same \"",
                           "(!migration.kept.empty() || !migration.failed.empty())) {",
                           "storage_notice(migration.to.parent_path().parent_path().string());",
                           "\"Storage: could not move {} to {} ({} failed: {}); the old \""})
    Expect(content.find(part) != std::string::npos, std::string("content_manager.cc has ") + part);
  ExpectIn((std::string(x) + "xam_ui.cc").c_str(),
           {"UiText(\"Each drive keeps its saves in its own PS5X360Reloaded/saves folder.\")"});
  const std::string main_text = Read("src/canary_game_main.cpp");
  Expect(main_text.find("\"Each drive keeps its saves in its own PS5X360/saves folder.\"") == std::string::npos &&
             main_text.find("\"Each drive keeps its saves in its own PS5X360Reloaded/saves folder.\"") != std::string::npos,
         "the title's text table has the renamed key only");
  Expect(main_text.find("xe::kernel::xam::storage_notice = &StorageNotice;") != std::string::npos &&
             main_text.find("Tr(\"This drive has both PS5X360/saves and PS5X360Reloaded/saves. PS5X360Reloaded/saves is used; "
                            "the old folder was left as it is:\")") != std::string::npos,
         "the notice, translated");
}

}  // namespace

int main() {
  CheckDecisions();
  CheckDrives();
  CheckBases();
  CheckRealTree();
  CheckTree();
  if (failures) {
    std::printf("check-save-folder: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-save-folder: ok\n");
  return 0;
}
