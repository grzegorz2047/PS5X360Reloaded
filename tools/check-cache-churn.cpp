// SPDX-License-Identifier: MIT
// Fix 172 (tools/source-1.0.2/fixes/172-utility-cache-churn-memory.patch): a utility cache on a drive
// moves the game's new files to memory once the game churns it. Kinect Sports: Season Two
// (4D5309D6) made and deleted its 320 KiB \CACHE\videobufferN replay files several a second in darts
// (345 creates and 346 deletes in 30 s on 1.0.7-work-integration1, when each already took 0.8-0.96
// s on ext1), and each slow host operation was followed by a game swap of 165-939 ms; its menus made
// 7 and 4 files in 30 s. Kinect Sports (4D5308C9), in fix 150d's list, made 3580 in memory in 16 ms.
// Here, run from the assembled tree's root:
//  - xenia/vfs/devices/cache_churn.h's Group on a fake clock: the menus' pace never moves it; the
//    darts' pace moves it at the 8th small file made and deleted within 10 s (once); big files,
//    files kept longer than a minute, files it never saw made, and a group that is not armed do
//    not count; a new file goes to memory only below the budget and with the direct memory
//    reserve kept;
//  - utility_cache_drive.hpp's WatchesChurn: every game but fix 150d's titles (GTA IV and Kinect
//    Sports stay as fix 150d and 150f decide, at 1x and 2x), and not with "ram" or "drive";
//  - the sources: HostPathEntry makes the new file in memory and follows the drive's files, the
//    VFS's open keeps a file in memory under a folder on the drive, MountUtilityCache gives the watch
//    to cache1: and cache: (not cache0:), and the title arms it at the start and for the game chosen.
// tools/check-vfs-entry-lifetime.cpp (i) drives the same through a real VFS on a host folder.
#undef NDEBUG
#include "xbox360ps5/utility_cache_drive.hpp"
#include "xenia/vfs/devices/cache_churn.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
namespace churn = xe::vfs::cache_churn;
namespace drive = xbox360ps5::utility_cache_drive;

int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::string Read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
// Whitespace (and CR) out, so line breaks and indentation do not matter.
std::string Squeezed(const std::string& text) {
  std::string out;
  for (char c : text)
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') out += c;
  return out;
}

uint64_t fake_now = 1000000;
uint64_t FakeNow() { return fake_now; }

// One file made, written to `bytes` and deleted `life_ms` later; true when that delete moved the group.
bool Turn(churn::Group& group, int id, uint64_t bytes, uint64_t life_ms) {
  static int keys[1 << 16];
  const void* key = &keys[id & 0xFFFF];
  group.NoteCreate(key);
  group.NoteSize(key, bytes / 2);
  group.NoteSize(key, bytes);
  group.NoteSize(key, bytes / 4);  // a shorter write later: the largest end counts
  fake_now += life_ms;
  return group.NoteDelete(key);
}

void Rule() {
  constexpr uint64_t kVideoBuffer = 320 * 1024;
  {
    // Season Two's menus: 7 files in 30 s, then 4 in 30 s: never 8 within 10 s.
    churn::Group group(FakeNow);
    group.Arm(true);
    bool moved = false;
    for (int round = 0; round < 6; ++round) {
      for (int i = 0; i < 7; ++i) {
        moved = Turn(group, i, 40 * 1024, 200) || moved;
        fake_now += 4000;
      }
    }
    Expect(!moved && !group.in_memory() && group.turns_total() == 42,
           "the menus' pace (7 small files in 30 s, 42 in all): stays on the drive");
  }
  {
    // Darts: 320 KiB files, one made and deleted every 120 ms (test107's collapsed pace was ~90 ms).
    churn::Group group(FakeNow);
    group.Arm(true);
    int moved_at = -1;
    for (int i = 0; i < 20; ++i) {
      if (Turn(group, 100 + i, kVideoBuffer, 120) && moved_at < 0) moved_at = i;
    }
    Expect(group.in_memory() && moved_at == int(churn::kTurns) - 1,
           "darts' videobuffers: moved to memory at the 8th made and deleted (at " + std::to_string(moved_at) + ")");
    Expect(group.tripped_after() == churn::kTurns, "the move logged once, after 8 turns");
  }
  {
    churn::Group group(FakeNow);
    group.Arm(true);
    for (int i = 0; i < 20; ++i) Turn(group, 200 + i, churn::kSmallBytes + 1, 100);
    Expect(!group.in_memory() && group.turns_total() == 0, "files over 8 MiB (GTA IV's archives) do not count");
    for (int i = 0; i < 20; ++i) Turn(group, 300 + i, kVideoBuffer, churn::kLifetimeMs + 1);
    Expect(!group.in_memory() && group.turns_total() == 0, "files kept longer than a minute do not count");
    static int unseen[20];
    bool any = false;
    for (int i = 0; i < 20; ++i) {
      fake_now += 50;
      any = group.NoteDelete(&unseen[i]) || any;
    }
    Expect(!any && !group.in_memory() && group.turns_total() == 0, "files made before the watch (on the drive already) do not count");
    group.NoteSize(&group, 5);  // a file it does not follow: nothing
    Expect(group.followed() == 0, "nothing followed after their deletes");
  }
  {
    churn::Group group(FakeNow);
    for (int i = 0; i < 40; ++i) Turn(group, 400 + i, kVideoBuffer, 50);
    Expect(!group.in_memory() && group.turns_total() == 40, "not armed (fix 150d's titles, \"drive\"): counted, never moved");
    group.Arm(true);
    Expect(Turn(group, 500, kVideoBuffer, 50) && group.in_memory(), "armed later: the next turn moves it (40 in the window)");
    Expect(!Turn(group, 501, kVideoBuffer, 50), "moved once only");
    group.Arm(false);
    Expect(group.in_memory(), "disarmed afterwards: the files in memory stay there");
  }
  {
    // Turns spread out: 7 within 10 s, then a gap: the window starts again.
    churn::Group group(FakeNow);
    group.Arm(true);
    for (int i = 0; i < 7; ++i) Turn(group, 600 + i, kVideoBuffer, 100);
    fake_now += churn::kWindowMs + 1;
    bool moved = false;
    for (int i = 0; i < 7; ++i) moved = Turn(group, 700 + i, kVideoBuffer, 100) || moved;
    Expect(!moved && !group.in_memory(), "7 turns, more than 10 s, 7 more: no move (the window)");
    Expect(Turn(group, 800, kVideoBuffer, 100) && group.in_memory(), "the 8th within 10 s: moved");
  }
  {
    // Many files made and never deleted: followed at most kTracked, the young ones not dropped for old.
    churn::Group group(FakeNow);
    group.Arm(true);
    static int many[churn::kTracked + 10];
    for (size_t i = 0; i < churn::kTracked; ++i) group.NoteCreate(&many[i]);
    fake_now += churn::kLifetimeMs + 1;
    group.NoteCreate(&many[churn::kTracked]);
    Expect(group.followed() == 1, "4096 files followed, all older than a minute: dropped for the next");
    for (size_t i = 0; i < churn::kTracked - 1; ++i) group.NoteCreate(&many[i]);
    group.NoteCreate(&many[churn::kTracked + 1]);
    Expect(group.followed() == 1, "4096 young files followed: the list starts again (bounded)");
  }
  {
    churn::Group group(FakeNow);
    constexpr int64_t kReserve = int64_t(1) << 30, kSegment = 16 << 20;
    Expect(!group.Admit(0, -1, kReserve, kSegment), "not moved: no file goes to memory");
    group.Arm(true);
    for (int i = 0; i < 8; ++i) Turn(group, 900 + i, 1, 10);
    Expect(group.in_memory(), "moved");
    Expect(group.Admit(0, -1, kReserve, kSegment), "moved, nothing in use, direct memory not known: memory");
    Expect(group.Admit(churn::kBudgetBytes - 1, kReserve + kSegment, kReserve, kSegment),
           "just below the budget, the reserve kept with a new segment: memory");
    Expect(!group.Admit(churn::kBudgetBytes, -1, kReserve, kSegment), "at the budget (128 MiB): the drive");
    Expect(!group.Admit(0, kReserve + kSegment - 1, kReserve, kSegment),
           "a new segment would take the direct memory under fix 150d's reserve: the drive");
    Expect(group.admitted() == 2 && group.declined() == 2, "counted: 2 in memory, 2 on the drive");
  }
}

void Watches() {
  using drive::WatchesChurn;
  Expect(WatchesChurn("4D5309D6", ""), "Kinect Sports: Season Two (4D5309D6): watched (not by a list)");
  Expect(WatchesChurn("4D53085B", "") && WatchesChurn("4D4A07F0", "\n"), "any other game (Halo: Reach, Zumba): watched");
  Expect(WatchesChurn("", ""), "no title yet (the launcher's start): watched, decided again for the game");
  Expect(!WatchesChurn("545407F2", "") && !WatchesChurn("4D5308C9", "") && !WatchesChurn("4d5308c9", ""),
         "fix 150d's titles (GTA IV, Kinect Sports): not watched");
  Expect(!WatchesChurn("4D5309D6", "ram\n"), "\"ram\": all in memory already, not watched");
  Expect(!WatchesChurn("4D5309D6", "drive") && !WatchesChurn("4D5309D6", "data") &&
             !WatchesChurn("4D5309D6", "console"),
         "\"drive\", \"data\", \"console\": on a drive, not watched");
  // Fix 150d and 150f unchanged for their titles.
  Expect(drive::CacheInMemory("545407F2", "", 1) && drive::CacheInMemory("4D5308C9", "", 1) &&
             !drive::CacheInMemory("545407F2", "", 2) && !drive::CacheInMemory("4D5308C9", "", 2),
         "fix 150d/150f: GTA IV and Kinect Sports in memory at 1x, on the drive (not watched) at 2x, as before");
  Expect(!drive::CacheInMemory("4D5309D6", "", 1), "Season Two is not in fix 150d's list (the watch decides)");
}

void Sources() {
  const std::string entry = Squeezed(Read(".deps/xenia-canary/src/xenia/vfs/devices/host_path_entry.cc"));
  const std::string file = Squeezed(Read(".deps/xenia-canary/src/xenia/vfs/devices/host_path_file.cc"));
  const std::string device = Squeezed(Read(".deps/xenia-canary/src/xenia/vfs/devices/host_path_device.h"));
  const std::string vfs = Squeezed(Read(".deps/xenia-canary/src/xenia/vfs/virtual_file_system.cc"));
  const std::string mount = Squeezed(Read("include/xbox360ps5/utility_cache.hpp"));
  const std::string main = Squeezed(Read("src/canary_game_main.cpp"));
  Expect(!entry.empty() && !main.empty(), "the sources read (run from the tree's root)");
  const auto has = [](const std::string& text, const std::string& what) {
    return text.find(Squeezed(what)) != std::string::npos;
  };
  const size_t admit = entry.find(Squeezed("churn->Admit(pool.used.load()"));
  const size_t memory_entry = entry.find(Squeezed("return std::make_unique<MemoryEntry>("));
  const size_t host_file = entry.find(Squeezed("auto full_path = host_path_ / xe::to_path(name);"));
  Expect(admit != std::string::npos && memory_entry > admit && host_file > memory_entry,
         "HostPathEntry::CreateEntryInternal: moved and admitted, a MemoryEntry; else the host file as before");
  Expect(has(entry, "churn->NoteCreate(created.get());"), "a file made on the drive is followed");
  const size_t detach = entry.find(Squeezed("HostPathEntry::DetachEntryInternal(Entry* entry) {"));
  const size_t detach_memory = entry.find(Squeezed("if (InMemory(entry)) { return {};"), detach);
  const size_t detach_note = entry.find(Squeezed("NoteChurnDelete(this, entry); // fix 172"), detach);
  const size_t remove = entry.find(Squeezed("bool HostPathEntry::DeleteEntryInternal(Entry* entry) {"));
  const size_t remove_memory = entry.find(Squeezed("if (MemoryEntry* memory = InMemory(entry)) { if (auto* data = memory->data()) data->MarkDeleted(); return true; }"), remove);
  const size_t remove_note = entry.find(Squeezed("NoteChurnDelete(this, entry); auto full_path"), remove);
  Expect(detach != std::string::npos && detach_note > detach_memory && detach_memory != std::string::npos &&
             remove != std::string::npos && remove_note > remove_memory && remove_memory != std::string::npos,
         "a delete: a file in memory touches no drive; one on the drive counts for the watch (rename or inline)");
  Expect(has(entry, "xbox360ps5::package_io::memory_tail.store(&memory_cache::ReportTail);"),
         "the move adds fix 150d's memory part to the 30 s File I/O line");
  Expect(has(file, "NoteChurnSize(written ? uint64_t(byte_offset) + *out_bytes_written : 0);") &&
             has(file, "if (set) NoteChurnSize(uint64_t(length));"),
         "HostPathFile's writes and lengths give the watch the file's size");
  Expect(has(device, "std::shared_ptr<cache_churn::Group> churn_;"), "HostPathDevice holds the watch");
  Expect(has(vfs, "if (host_path && dynamic_cast<const xe::vfs::HostPathEntry*>(entry)) {"),
         "the VFS's open checks the host file only for an entry on the drive (a file in memory stays)");
  Expect(has(mount, "std::shared_ptr<xe::vfs::cache_churn::Group> churn = {}") &&
             has(mount, "if (churn && std::string(name) != \"cache0\") host_device->set_churn(churn);"),
         "MountUtilityCache gives the watch to cache1: and cache: on the drive, not cache0:");
  const size_t made = main.find(Squeezed("const auto cache_churn = std::make_shared<xe::vfs::cache_churn::Group>();"));
  const size_t armed = main.find(Squeezed("cache_churn->Arm(!cache_in_memory && xbox360ps5::utility_cache_drive::WatchesChurn(settings_title, cache_drive_switch));"));
  const size_t mounted = main.find(Squeezed("MountUtilityCache( *emulator.file_system(), cache_drive.base, cache_in_memory ? memory_caches : no_memory_caches, cache_churn);"));
  const size_t fallback = main.find(Squeezed("MountUtilityCache( *emulator.file_system(), bulk, cache_in_memory ? memory_caches : no_memory_caches, cache_churn);"));
  Expect(made != std::string::npos && armed > made && mounted > armed && fallback > mounted,
         "the start makes the watch, arms it (not for fix 150d's titles) and mounts both drives with it");
  const size_t moved = main.find(Squeezed("cache_in_memory_now = xbox360ps5::MoveUtilityCacheToMemory(*emulator.file_system());"));
  const size_t rearmed = main.find(Squeezed("const bool churn_watch = !cache_in_memory_now && xbox360ps5::utility_cache_drive::WatchesChurn(settings_title, cache_drive_switch); cache_churn->Arm(churn_watch);"));
  Expect(moved != std::string::npos && rearmed > moved,
         "the game chosen arms it again after fix 150d3's move (GTA IV and Kinect Sports at 2x: disarmed)");
  Expect(has(main, "Utility cache (fix 172): cache: and cache1: on the drive watch {} for churn"),
         "the launch logs the watch");
}
}  // namespace

int main() {
#ifndef XBOX360PS5_CACHE_CHURN
  std::printf("FAIL: cache_churn.h has no XBOX360PS5_CACHE_CHURN (fix 172 not in the tree)\n");
  return 1;
#endif
  Rule();
  Watches();
  Sources();
  std::printf(failures ? "check-cache-churn: %d failure(s)\n" : "check-cache-churn: ok\n", failures);
  return failures ? 1 : 0;
}
