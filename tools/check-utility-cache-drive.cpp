// SPDX-License-Identifier: MIT
// Fix 150c (tools/source-1.0.2/fixes/150c-utility-cache-extended-storage.patch): the guest's
// utility cache (cache0:, cache1:, cache:) goes to the PS5's M.2 SSD (extended storage, /mnt/ext0
// to ext3) when one is mounted, takes a probe file and has 4 GiB free, under the folder the saves
// use there (the drive's top, else its homebrew or xbox360 folder, then PS5X360Reloaded). Without
// one, or with "data" in utility-cache-drive.txt, it stays in the internal storage. Grand Theft
// Auto IV deletes and makes again cache files of hundreds of MB while you play, and on /data each
// took the file system for up to 0.95 s. include/xbox360ps5/utility_cache_drive.hpp is driven with
// a fake filesystem laid out as the console's /mnt (fix 27's check: ext1's top not writable, its
// xbox360 folder writable; placeholders that are not mount points), then on the host's own
// filesystem; the title's start (src/canary_game_main.cpp) is checked to mount the chosen folder,
// with the internal storage as the fallback. Fix 150d: Grand Theft Auto IV's and Kinect Sports'
// cache: and cache1: in memory (CacheInMemory: its title ID; "ram"/"drive" in utility-cache-drive.txt for every game),
// cache0: and xstorage: on the drive; the title and utility_cache.hpp wired so. Run from the
// assembled tree's root.
#undef NDEBUG
#include "xbox360ps5/utility_cache_drive.hpp"

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace {
namespace stdfs = std::filesystem;
namespace drive = xbox360ps5::utility_cache_drive;
namespace storage = xe::kernel::xam::storage;

const stdfs::path kInternal = "/data/homebrew/PPSA83600/cache";
const stdfs::path kMnt = "/mnt";

// Directories exist only when listed; a probe works only in a writable one; Info answers dev 1
// (the internal storage's) with 16 GiB free unless a path has its own.
struct FakeFs {
  std::set<std::string> mounts, dirs, writable;
  std::map<std::string, storage::FsInfo> info;
  int probes = 0;

  bool IsMountPoint(const stdfs::path& path) { return mounts.count(path.string()) != 0; }
  bool IsDirectory(const stdfs::path& path) {
    return dirs.count(path.string()) != 0 || mounts.count(path.string()) != 0;
  }
  bool ProbeWritable(const stdfs::path& path) {
    ++probes;
    return IsDirectory(path) && writable.count(path.string()) != 0;
  }
  storage::FsInfo Info(const stdfs::path& path) {
    auto found = info.find(path.string());
    if (found != info.end()) return found->second;
    storage::FsInfo internal;
    internal.ok = true;
    internal.dev = 1;
    internal.space_ok = true;
    internal.capacity = uint64_t(64) << 30;
    internal.free = uint64_t(16) << 30;
    return internal;
  }
  // A drive of its own (another st_dev), free bytes known.
  void Mount(const std::string& name, uint64_t dev, uint64_t free_bytes = uint64_t(500) << 30,
             bool space_ok = true) {
    const std::string mount = (kMnt / name).string();
    mounts.insert(mount);
    storage::FsInfo fs;
    fs.ok = true;
    fs.dev = dev;
    fs.space_ok = space_ok;
    fs.capacity = uint64_t(1) << 40;
    fs.free = free_bytes;
    info[mount] = fs;
  }
};

int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}
void ExpectBase(const drive::Choice& got, const stdfs::path& want, const std::string& what) {
  Expect(got.base == want, what + " -> " + got.base.string() + " (" + got.where + ")");
}

std::string Read(const stdfs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out = text.str(), plain;
  for (char c : out)
    if (c != '\r') plain += c;
  return plain;
}

void Choices() {
  const stdfs::path ext1_product = "/mnt/ext1/PS5X360Reloaded";
  {
    FakeFs fs;
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), kInternal, "nothing mounted: internal storage");
  }
  {
    // As the console's /mnt looked: placeholders for every slot, none a mount point.
    FakeFs fs;
    for (const auto& slot : storage::kSlots) {
      fs.dirs.insert((kMnt / slot.mount).string());
      fs.writable.insert((kMnt / slot.mount).string());
    }
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), kInternal, "placeholders only: internal storage");
  }
  {
    FakeFs fs;
    fs.Mount("ext1", 4);
    fs.writable.insert("/mnt/ext1");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), ext1_product, "ext1 with a writable top");
  }
  {
    // Fix 27's console: ext1's top not writable, its xbox360 folder writable.
    FakeFs fs;
    fs.Mount("ext1", 4);
    fs.dirs.insert("/mnt/ext1/xbox360");
    fs.writable.insert("/mnt/ext1/xbox360");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), "/mnt/ext1/xbox360/PS5X360Reloaded",
               "ext1's top read-only, xbox360 writable");
  }
  {
    FakeFs fs;
    fs.Mount("ext1", 4);
    fs.dirs.insert("/mnt/ext1/homebrew");
    fs.writable.insert("/mnt/ext1/homebrew");
    fs.dirs.insert("/mnt/ext1/xbox360");
    fs.writable.insert("/mnt/ext1/xbox360");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), "/mnt/ext1/homebrew/PS5X360Reloaded",
               "homebrew before xbox360");
  }
  {
    // The product folder already there (saves on the drive): it is used when it takes a file.
    FakeFs fs;
    fs.Mount("ext1", 4);
    fs.dirs.insert("/mnt/ext1/PS5X360Reloaded");
    fs.writable.insert("/mnt/ext1/PS5X360Reloaded");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), ext1_product, "the drive's product folder");
  }
  {
    FakeFs fs;
    fs.Mount("ext1", 4);
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), kInternal, "ext1 not writable anywhere: internal");
  }
  {
    FakeFs fs;
    fs.Mount("ext0", 3);
    fs.writable.insert("/mnt/ext0");
    fs.Mount("ext1", 4);
    fs.writable.insert("/mnt/ext1");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), "/mnt/ext0/PS5X360Reloaded", "ext0 before ext1");
  }
  {
    FakeFs fs;
    fs.Mount("usb0", 7);
    fs.writable.insert("/mnt/usb0");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), kInternal, "a USB drive is not used");
  }
  {
    // A path answering the internal storage's st_dev is no drive of its own.
    FakeFs fs;
    fs.Mount("ext1", 1);
    fs.writable.insert("/mnt/ext1");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), kInternal, "ext1 on the internal file system");
  }
  {
    FakeFs fs;
    fs.Mount("ext1", 4, uint64_t(3) << 30);
    fs.writable.insert("/mnt/ext1");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, ""), kInternal, "ext1 with 3 GiB free: internal");
    FakeFs unknown;
    unknown.Mount("ext1", 4, 0, false);
    unknown.writable.insert("/mnt/ext1");
    ExpectBase(drive::Choose(unknown, kInternal, kMnt, ""), ext1_product, "ext1, free space unknown");
  }
  {
    FakeFs fs;
    fs.Mount("ext1", 4);
    fs.writable.insert("/mnt/ext1");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, "data\n"), kInternal, "utility-cache-drive.txt: data");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, "console"), kInternal, "utility-cache-drive.txt: console");
    ExpectBase(drive::Choose(fs, kInternal, kMnt, "ext\n"), ext1_product, "utility-cache-drive.txt: ext");
    Expect(drive::Choose(fs, kInternal, kMnt, "").where == "extended storage ext1", "the log names ext1");
  }
}

void HostFilesystem() {
  char pattern[] = "/tmp/check-utility-cache-drive-XXXXXX";
  const char* made = mkdtemp(pattern);
  assert(made);
  const stdfs::path root = made;
  // The real PosixFs: a plain folder named ext1 is no mount point.
  stdfs::create_directories(root / "mnt" / "ext1");
  storage::PosixFs posix;
  ExpectBase(drive::Choose(posix, root / "internal", root / "mnt", ""), root / "internal",
             "host: a folder named ext1 is not a drive");
  const double ms = drive::ProbeMs(root / "internal" / "utility-cache");
  Expect(ms >= 0 && stdfs::is_directory(root / "internal" / "utility-cache") &&
             stdfs::is_empty(root / "internal" / "utility-cache"),
         "host: the probe made, renamed and deleted its file (" + std::to_string(ms) + " ms)");
  {
    std::ofstream file(root / "a-file");
    file << "x";
  }
  Expect(drive::ProbeMs(root / "a-file" / "below") < 0, "host: a probe that cannot be made gives -1");
  stdfs::remove_all(root);
}

// Without spaces and line ends, so the checks do not depend on how a call is wrapped.
std::string Squeezed(const std::string& text) {
  std::string out;
  for (char c : text)
    if (c != ' ' && c != '\n' && c != '\t') out += c;
  return out;
}
void Wiring() {
  const std::string main = Squeezed(Read("src/canary_game_main.cpp"));
  Expect(!main.empty(), "src/canary_game_main.cpp read (run from the tree's root)");
  const auto has = [&main](const std::string& text) { return main.find(Squeezed(text)); };
  Expect(has("#include \"xbox360ps5/utility_cache_drive.hpp\"") != std::string::npos,
         "the title includes utility_cache_drive.hpp");
  const size_t choose = has("utility_cache_drive::Choose(cache_drive_fs, bulk, \"/mnt\"");
  const size_t mount = has("MountUtilityCache(*emulator.file_system(), cache_drive.base");
  const size_t fallback = has("cache_mounted = xbox360ps5::MountUtilityCache(*emulator.file_system(), bulk");
  const size_t cancel = has("Required utility cache is unavailable");
  Expect(choose != std::string::npos && mount != std::string::npos && choose < mount,
         "the title mounts the chosen folder");
  Expect(fallback != std::string::npos && mount < fallback && fallback < cancel,
         "the internal storage is the fallback before the launch is cancelled");
  Expect(has("XBOX360PS5_DATA_ROOT \"/utility-cache-drive.txt\"") != std::string::npos,
         "utility-cache-drive.txt is read from the title folder");
  Expect(has("bulk / \"utility-cache\" / \"xstorage\"") != std::string::npos,
         "xstorage: stays in the internal storage");
  Expect(has("MountUtilityCache(*emulator.file_system(), bulk)) {") == std::string::npos,
         "no unconditional mount in the internal storage left");
}

// Fix 150d.
void InMemory() {
  using drive::CacheInMemory;
  Expect(CacheInMemory("545407F2", ""), "fix 150d: GTA IV (545407F2): cache in memory");
  Expect(CacheInMemory("545407f2", "\n"), "fix 150d: its title ID in lower case too");
  Expect(CacheInMemory("4D5308C9", ""), "fix 150d: Kinect Sports (4D5308C9): cache in memory");
  Expect(!CacheInMemory("4D5309D6", ""), "fix 150d: Kinect Sports Season Two (not measured): on the drive");
  Expect(!CacheInMemory("4D53085B", ""), "fix 150d: another game (Halo: Reach): on the drive");
  Expect(!CacheInMemory("", ""), "fix 150d: no title ID: on the drive");
  Expect(CacheInMemory("4D53085B", "ram\n"), "fix 150d: \"ram\" in the file: in memory for every game");
  Expect(!CacheInMemory("545407F2", "drive"), "fix 150d: \"drive\": GTA IV on the drive");
  Expect(!CacheInMemory("545407F2", "data"), "fix 150d: \"data\": GTA IV in the internal storage (150c's switch)");
  const std::string main = Squeezed(Read("src/canary_game_main.cpp"));
  const auto has = [&main](const std::string& text) { return main.find(Squeezed(text)); };
  const size_t decide = has("utility_cache_drive::CacheInMemory(settings_title, cache_drive_switch)");
  const size_t names = has("memory_caches = {\"cache1\", \"cache\"}");
  const size_t mount = has("MountUtilityCache(*emulator.file_system(), cache_drive.base, cache_in_memory ? memory_caches : no_memory_caches)");
  const size_t fallback = has("MountUtilityCache(*emulator.file_system(), bulk, cache_in_memory ? memory_caches : no_memory_caches)");
  Expect(decide != std::string::npos && names != std::string::npos && mount != std::string::npos &&
             decide < mount && mount < fallback && fallback != std::string::npos,
         "fix 150d: the title decides by its title ID and the switch, and mounts cache1: and cache: in memory "
         "(also on the fallback)");
  Expect(has("Utility cache (fix 150d): cache: and cache1: in memory") != std::string::npos,
         "fix 150d: the start logs it with the cap, the reserve and direct free memory");
  const std::string mount_code = Squeezed(Read("include/xbox360ps5/utility_cache.hpp"));
  Expect(mount_code.find(Squeezed("std::make_unique<xe::vfs::MemoryDevice>(mount)")) != std::string::npos &&
             mount_code.find(Squeezed("for (const char* name : {\"cache0\", \"cache1\", \"cache\"})")) != std::string::npos,
         "fix 150d: utility_cache.hpp mounts a memory device for the names asked, in the order cache0, cache1, cache");
  Expect(main.find(Squeezed("memory_caches = {\"cache0\"")) == std::string::npos &&
             main.find(Squeezed("\"cache0\", \"cache1\", \"cache\"}; // memory")) == std::string::npos,
         "fix 150d: cache0: (other games' data) stays on the drive");
}
}  // namespace

int main() {
  Choices();
  HostFilesystem();
  Wiring();
  InMemory();  // fix 150d
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: the utility cache goes to the extended storage when there is one\n");
  return 0;
}
