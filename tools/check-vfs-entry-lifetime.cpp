// SPDX-License-Identifier: MIT
// Fix 139 (xenia/vfs/entry.cc, file.h, devices/host_path_file.cc, virtual_file_system.cc):
// a file system entry is never freed while a file or an operation uses it. Halo: Reach
// (4D53085B) on 1.0.5_candidate_89 crashed in Entry::Delete called from HostPathFile::Destroy
// (NtClose of a delete-on-close asq_m10_*.temp file): the entry's parent pointer read 0x643, the
// entry freed already. Built against the assembled tree's xenia/base and xenia/vfs with a real
// host directory (tools/check-vfs-entry-lifetime.sh), under AddressSanitizer when the compiler
// has it:
//  (a) two handles of one delete-on-close file, closed in both orders: the first close deletes
//      the host file, the second handle still writes and closes, the name can be created again;
//  (b) a file replaced (overwrite) while another handle of it is open, then that handle closed;
//  (c) a directory deleted while a delete-on-close file in it is open, then the file closed;
//  (d) fix 129's window (the host delete made slow, 300 ms, as the console's disk took up to
//      948 ms), with fix 139b: through a delete-on-close close and through DeletePath, the global
//      lock is free meanwhile (1.0.5-work-fix139 held it in Entry::Delete(): Kinect Sports'
//      replay files, holds of up to 949 ms), and an open of the same name goes on at once with a
//      new entry (create, open-if) or finds no file (open), never the entry being deleted; the
//      file renamed for the delete is gone afterwards.
//  (f) fix 150b (the game's re-create of a cache file waiting 0.72-0.95 s for the unlink of the old
//      one): ShrinkThenRemove's steps (sizes, the remove after them, a failed step leaving the
//      rest to the remove, nothing for a small file); a 10 MiB file deleted through the VFS is
//      shrunk in steps (2 at 4 MiB) and gone, a file made again under its name meanwhile kept.
//  (e) fix 150 (Grand Theft Auto IV's pauses at its 350-950 ms deletes): the renamed file deleted
//      in the background by default, the delete returning at once, a file made again under the
//      name meanwhile kept; inline, the delete takes the slow part as before.
//  (g) fix 150d (GTA IV's cache: and cache1: in memory, memory_device.h): a device of its own pool
//      mounted in the VFS: sparse writes read back with zeros in the hole, the bytes in use counted
//      in 64 KiB chunks, a shorter length gives chunks back and a longer one reads zeros; two
//      handles of a delete-on-close file (the name gone at the first close, the data kept for the
//      other handle, freed at its close); an overwrite while the old file is open; a directory
//      deleted with its file; the cap (STATUS_DISK_FULL, the bytes up to it written) and the
//      direct memory reserve; segments mapped through xe::memory and all unmapped once empty;
//      four threads writing, deleting and making files again; the 30 s File I/O line's part.
// tools/check-vfs-entry-lifetime.sh also runs (a)-(c) on the tree without fix 139, where the
// sanitizer must report the use of a freed entry (the console's crash).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/host_path_entry.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/entry_io.h"
#if __has_include("xenia/vfs/devices/host_delete_steps.h")
#include "xenia/vfs/devices/host_delete_steps.h"  // fix 150b
#endif
#include "xenia/vfs/virtual_file_system.h"
// Fix 150d's device needs fixes 139 and 150 (the tree without them, built by the .sh, still finds
// the assembled tree's header on its include path).
#if __has_include("xenia/vfs/devices/memory_device.h") && defined(XE_VFS_BACKGROUND_DELETE)
#include "xenia/vfs/devices/memory_device.h"  // fix 150d
#endif

using namespace xe;  // X_STATUS

// The console's crash report (platform/ps5/crash_report.cpp) records stages the host file code
// names; this check runs without it.
namespace xbox360ps5 {
void Stage(const char*) {}
}  // namespace xbox360ps5

namespace {

namespace vfs = xe::vfs;
int failures = 0;
void Expect(bool condition, const std::string& what) {
  std::printf("%s: %s\n", condition ? "ok" : "FAIL", what.c_str());
  std::fflush(stdout);
  if (!condition) ++failures;
}
std::string Hex(uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", value);
  return text;
}

const std::string kMount = "\\Device\\Lifetime";
std::filesystem::path host;
// GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE, as games open their temporary files.
constexpr uint32_t kAccess = 0xC0110000u;

vfs::File* Open(vfs::VirtualFileSystem& fs, const std::string& name, vfs::FileDisposition disposition,
                X_STATUS* out_status = nullptr) {
  vfs::File* file = nullptr;
  vfs::FileAction action = vfs::FileAction::kDoesNotExist;
  const X_STATUS status =
      fs.OpenFile(nullptr, kMount + "\\" + name, disposition, kAccess, false, true, &file, &action);
  if (out_status) *out_status = status;
  return status == X_STATUS_SUCCESS ? file : nullptr;
}
bool Write(vfs::File* file, uint8_t value) {
  const uint8_t data[4] = {'L', 'I', 'F', value};
  size_t written = 0;
  return file->WriteSync(std::span<const uint8_t>(data, sizeof(data)), 0, &written) == X_STATUS_SUCCESS &&
         written == sizeof(data);
}
bool OnHost(const std::string& name) {
  std::error_code error;
  return std::filesystem::exists(host / name, error);
}

// (a) Two handles of one delete-on-close file.
void TwoHandles(vfs::VirtualFileSystem& fs) {
  for (int order = 0; order < 2; ++order) {
    const std::string name = "asq_m10_" + std::to_string(order) + ".temp";
    vfs::File* a = Open(fs, name, vfs::FileDisposition::kOverwriteIf);
    vfs::File* b = a ? Open(fs, name, vfs::FileDisposition::kOpen) : nullptr;
    Expect(a && b && a->entry() == b->entry(), name + ": two handles of one entry");
    if (!a || !b) return;
    Expect(Write(a, 1), name + ": written");
    a->entry()->SetForDeletion(true);  // NtSetInformationFile, FileDispositionInformation
    vfs::File* first = order == 0 ? a : b;
    vfs::File* second = order == 0 ? b : a;
    first->Destroy();
    Expect(!OnHost(name), name + ": the host file deleted at the first close");
#ifdef XE_VFS_ENTRY_IO_TEST_HOOK
    Expect(second->entry()->is_removed(), name + ": the entry out of its directory, kept for the other handle");
#endif
    Expect(Write(second, 2), name + ": the other handle still writes");
    second->Destroy();  // without fix 139: Entry::Delete of the freed entry (the console's crash)
    vfs::File* again = Open(fs, name, vfs::FileDisposition::kCreate);
    Expect(again != nullptr, name + ": created again after both closed");
    if (again) {
      again->entry()->SetForDeletion(true);
      again->Destroy();
    }
    Expect(!OnHost(name), name + ": deleted again");
  }
}

// (b) A file replaced while another handle of it is open.
void ReplacedWhileOpen(vfs::VirtualFileSystem& fs) {
  const std::string name = "replaced.bin";
  vfs::File* old_handle = Open(fs, name, vfs::FileDisposition::kOverwriteIf);
  Expect(old_handle && Write(old_handle, 3), name + ": created");
  if (!old_handle) return;
  vfs::File* replacing = Open(fs, name, vfs::FileDisposition::kOverwriteIf);
  Expect(replacing && replacing->entry() != old_handle->entry(), name + ": overwritten, a new entry");
  old_handle->entry()->SetForDeletion(true);
  old_handle->Destroy();  // its entry was deleted by the overwrite
  Expect(OnHost(name), name + ": the new file stays (the old handle's close deletes nothing of it)");
  if (replacing) {
    replacing->entry()->SetForDeletion(true);
    replacing->Destroy();
  }
  Expect(!OnHost(name), name + ": deleted at the new handle's close");
}

// (c) A directory deleted while a delete-on-close file in it is open.
void DirectoryWithOpenFile(vfs::VirtualFileSystem& fs) {
  Expect(fs.ResolvePath(kMount + "\\cache1") != nullptr, "cache1: the directory is in the tree");
  vfs::File* file = Open(fs, "cache1\\cache010.map", vfs::FileDisposition::kOverwriteIf);
  Expect(file && Write(file, 4), "cache1\\cache010.map: created");
  if (!file) return;
  file->entry()->SetForDeletion(true);
  Expect(fs.DeletePath(kMount + "\\cache1"), "cache1: deleted with the file open");
  Expect(!OnHost("cache1"), "cache1: gone on the host");
#ifdef XE_VFS_ENTRY_IO_TEST_HOOK
  Expect(file->entry()->is_removed(), "cache1\\cache010.map: removed with its directory, kept for its handle");
#endif
  file->Destroy();  // without fix 139: Delete through the freed directory
  Expect(Open(fs, "cache1\\cache010.map", vfs::FileDisposition::kOpen) == nullptr,
         "cache1\\cache010.map: not found afterwards");
}

#if defined(XE_VFS_ENTRY_IO_TEST_HOOK) && defined(XE_VFS_ENTRY_DETACH)
std::atomic<bool> in_delete{false};
void SlowDelete(vfs::entry_io::Kind kind) {
  if (kind != vfs::entry_io::kDelete) return;
  in_delete = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}
double MsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
bool AnyRenamedLeft() {
  std::error_code error;
  for (const auto& item : std::filesystem::directory_iterator(host, error))
    if (item.path().filename().string().rfind(vfs::HostPathEntry::kDeletedPrefix, 0) == 0) return true;
  return false;
}

// (d) Fix 129's window, with fix 139b: the host delete made slow (300 ms). The global lock is free
// meanwhile (taken at once by another thread; fix 139 held it through a delete on close), and an
// open of the same name goes on at once: a new entry and file (create, open-if), or no file
// (open). The delete comes from a delete-on-close close (Entry::Delete()) or a DeletePath.
void OpenDuringSlowDelete(vfs::VirtualFileSystem& fs, vfs::FileDisposition disposition, const char* what,
                          bool by_close) {
  const std::string name = std::string("window_") + what + (by_close ? "_close" : "_path") + ".temp";
  vfs::File* made = Open(fs, name, vfs::FileDisposition::kOverwriteIf);
  Expect(made && Write(made, 5), name + ": created");
  if (!made) return;
  vfs::Entry* dying = made->entry();
  if (!by_close) made->Destroy();
#ifdef XE_VFS_BACKGROUND_DELETE
  // Earlier deletes still queued (fix 150) would run the slow hook and set in_delete before
  // this delete starts, and the open below would race it (C0000035 in CI run 45273903022).
  vfs::Entry::FlushBackgroundDeletes();
#endif
  in_delete = false;
  vfs::entry_io::test_hook = SlowDelete;
  std::thread deleter([&] {
    if (by_close) {
      made->entry()->SetForDeletion(true);
      made->Destroy();
    } else {
      fs.DeletePath(kMount + "\\" + name);
    }
  });
  while (!in_delete) std::this_thread::yield();
  auto start = std::chrono::steady_clock::now();
  { auto lock = xe::global_critical_region::Acquire(); }
  const double lock_ms = MsSince(start);
  start = std::chrono::steady_clock::now();
  X_STATUS status = 0;
  vfs::File* file = Open(fs, name, disposition, &status);
  const double open_ms = MsSince(start);
  const bool deleting = in_delete;  // still in the slow part
  deleter.join();
#ifdef XE_VFS_BACKGROUND_DELETE
  vfs::Entry::FlushBackgroundDeletes();  // fix 150: the renamed file's delete runs in the background
#endif
  vfs::entry_io::test_hook = nullptr;
  char ms[96];
  std::snprintf(ms, sizeof(ms), "lock taken in %.1f ms, open in %.1f ms", lock_ms, open_ms);
  Expect(lock_ms < 100, name + ": the global lock is free during the slow delete (" + ms + ")");
  Expect(!AnyRenamedLeft(), name + ": the renamed file deleted afterwards");
  if (disposition == vfs::FileDisposition::kOpen) {
    Expect(!file && status == X_STATUS_OBJECT_NAME_NOT_FOUND && open_ms < 150 && deleting,
           name + ": open during the delete finds no file at once (" + ms + ", status " + Hex(status) + ")");
    if (file) file->Destroy();
    return;
  }
  Expect(file && open_ms < 150 && deleting,
         name + ": opened during the delete, at once (" + ms + ", status " + Hex(status) + ")");
  if (!file) return;
  Expect(file->entry() != dying && !file->entry()->is_deleting() && !file->entry()->is_removed(),
         name + ": a new entry, not the one being deleted");
  Expect(Write(file, 6) && OnHost(name), name + ": the new file is on the host after the delete");
  file->entry()->SetForDeletion(true);
  file->Destroy();
#ifdef XE_VFS_BACKGROUND_DELETE
  vfs::Entry::FlushBackgroundDeletes();
#endif
  Expect(!OnHost(name) && !AnyRenamedLeft(), name + ": deleted at its close");
}

#ifdef XE_VFS_BACKGROUND_DELETE
// (e) Fix 150: the renamed file deleted in the background (the default). With the delete made
// slow (300 ms), a delete returns after the rename, at once; a file made again under the name
// meanwhile is another file, still there once the background delete is done; the renamed file
// gone then. Inline (vfs-delete.txt "inline"), the delete takes the 300 ms in the caller.
#ifdef XENIA_VFS_DEVICES_HOST_DELETE_STEPS_H_
// (f) Fix 150b.
void ShrinkSteps(vfs::VirtualFileSystem& fs) {
  namespace ds = vfs::delete_steps;
  std::vector<uint64_t> sizes;
  int pauses = 0;
  bool removed = false;
  Expect(ds::ShrinkThenRemove(10, 4, [&](uint64_t n) { sizes.push_back(n); return true; },
                              [&] { removed = true; return true; }, [&] { ++pauses; }) &&
             sizes == std::vector<uint64_t>{6, 2} && pauses == 2 && removed && ds::last_steps == 2,
         "fix 150b: 10 bytes in steps of 4: 6, 2, then the remove");
  sizes.clear();
  Expect(ds::ShrinkThenRemove(10, 4, [&](uint64_t n) { sizes.push_back(n); return false; }, [] { return true; },
                              [] {}) &&
             sizes.size() == 1 && ds::last_steps == 0,
         "fix 150b: a failed step leaves the rest to the remove");
  sizes.clear();
  Expect(ds::ShrinkThenRemove(3, 4, [&](uint64_t n) { sizes.push_back(n); return true; }, [] { return false; },
                              [] {}) == false &&
             sizes.empty(),
         "fix 150b: a file of one step or less: only the remove, its result returned");
  // Through the VFS: 10 MiB, deleted in the background, made again meanwhile.
  const std::string name = "bars_1.img";
  vfs::File* made = Open(fs, name, vfs::FileDisposition::kOverwriteIf);
  Expect(made != nullptr, name + ": created");
  if (!made) return;
  made->Destroy();
  std::error_code error;
  std::filesystem::resize_file(host / name, uint64_t(10) << 20, error);
  Expect(!error, name + ": 10 MiB on the host");
  vfs::Entry::OverrideDeleteMode(1);
  Expect(fs.DeletePath(kMount + "\\" + name), name + ": deleted");
  vfs::File* again = Open(fs, name, vfs::FileDisposition::kCreate);
  Expect(again && Write(again, 9), name + ": made again at once");
  if (again) again->Destroy();
  vfs::Entry::FlushBackgroundDeletes();
  vfs::Entry::OverrideDeleteMode(-1);
  Expect(ds::last_steps == 2 && !AnyRenamedLeft(),
         name + ": the old one shrunk in " + std::to_string(ds::last_steps.load()) + " steps and gone");
  Expect(std::filesystem::file_size(host / name, error) == 4 && !error, name + ": the new one kept, 4 bytes");
  Expect(fs.DeletePath(kMount + "\\" + name), name + ": the new one deleted");
  vfs::Entry::FlushBackgroundDeletes();
}
#endif

void BackgroundDelete(vfs::VirtualFileSystem& fs, bool background) {
  const std::string name = std::string("streaming_") + (background ? "bg" : "inline") + ".dat";
  vfs::Entry::OverrideDeleteMode(background ? 1 : 0);
  vfs::File* made = Open(fs, name, vfs::FileDisposition::kOverwriteIf);
  Expect(made && Write(made, 7), name + ": created");
  if (!made) return;
  made->Destroy();
  vfs::Entry::FlushBackgroundDeletes();  // nothing earlier runs the slow hook below
  in_delete = false;
  vfs::entry_io::test_hook = SlowDelete;
  auto start = std::chrono::steady_clock::now();
  const bool deleted = fs.DeletePath(kMount + "\\" + name);
  const double delete_ms = MsSince(start);
  char ms[64];
  std::snprintf(ms, sizeof(ms), "%.1f ms", delete_ms);
  if (!background) {
    Expect(deleted && delete_ms >= 250, name + ": inline, the delete took the slow part (" + ms + ")");
    vfs::entry_io::test_hook = nullptr;
    vfs::Entry::OverrideDeleteMode(-1);
    Expect(!OnHost(name) && !AnyRenamedLeft(), name + ": inline, gone at once");
    return;
  }
  Expect(deleted && delete_ms < 100, name + ": the delete returned after the rename (" + ms + ")");
  vfs::File* again = Open(fs, name, vfs::FileDisposition::kCreate);
  Expect(again && Write(again, 8), name + ": made again while the renamed file is being deleted");
  if (again) again->Destroy();
  vfs::Entry::FlushBackgroundDeletes();
  vfs::entry_io::test_hook = nullptr;
  vfs::Entry::OverrideDeleteMode(-1);
  Expect(in_delete && OnHost(name) && !AnyRenamedLeft(),
         name + ": the background delete ran; the file made again is still there, the renamed one gone");
  std::error_code error;
  Expect(std::filesystem::file_size(host / name, error) == 4 && !error, name + ": with its own data");
  Expect(fs.DeletePath(kMount + "\\" + name), name + ": deleted");
  vfs::Entry::FlushBackgroundDeletes();
}
#endif
#endif


#ifdef XENIA_VFS_DEVICES_MEMORY_DEVICE_H_
// (g) Fix 150d.
namespace mc = vfs::memory_cache;
const std::string kMemoryMount = "\\Device\\MemoryCache";
std::atomic<int> segment_maps{0}, segment_unmaps{0};
void* CountedMap(size_t bytes, int64_t* handle) {
  void* base = mc::MapMappingSegment(bytes, handle);
  if (base) ++segment_maps;
  return base;
}
void CountedUnmap(void* base, size_t bytes, int64_t handle) {
  ++segment_unmaps;
  mc::UnmapMappingSegment(base, bytes, handle);
}
int64_t fake_direct_free = -1;
vfs::File* OpenMemory(vfs::VirtualFileSystem& fs, const std::string& name, vfs::FileDisposition disposition,
                      X_STATUS* out_status = nullptr, bool directory = false) {
  vfs::File* file = nullptr;
  vfs::FileAction action = vfs::FileAction::kDoesNotExist;
  const X_STATUS status = fs.OpenFile(nullptr, kMemoryMount + "\\" + name, disposition, kAccess, directory, !directory,
                                      &file, &action);
  if (out_status) *out_status = status;
  return status == X_STATUS_SUCCESS ? file : nullptr;
}
std::vector<uint8_t> Pattern(size_t bytes, uint8_t seed) {
  std::vector<uint8_t> data(bytes);
  for (size_t i = 0; i < bytes; ++i) data[i] = uint8_t(seed + i * 7 + (i >> 9));
  return data;
}
X_STATUS WriteAt(vfs::File* file, const std::vector<uint8_t>& data, size_t offset, size_t* written) {
  return file->WriteSync(std::span<const uint8_t>(data.data(), data.size()), offset, written);
}
std::vector<uint8_t> ReadAt(vfs::File* file, size_t bytes, size_t offset, X_STATUS* status = nullptr) {
  std::vector<uint8_t> data(bytes, 0xEE);
  size_t got = 0;
  const X_STATUS result = file->ReadSync(std::span<uint8_t>(data.data(), data.size()), offset, &got);
  if (status) *status = result;
  data.resize(got);
  return data;
}
std::vector<uint8_t> Head(const std::vector<uint8_t>& data, size_t bytes) {
  return std::vector<uint8_t>(data.begin(), data.begin() + std::min(bytes, data.size()));
}
bool AllZero(const std::vector<uint8_t>& data) {
  for (uint8_t b : data)
    if (b) return false;
  return !data.empty();
}

void MemoryCache() {
  constexpr uint64_t kMiB = 1 << 20, kChunk = mc::kChunkBytes;
  mc::Pool pool;
  pool.cap = 64 * kMiB;
  pool.reserve = 256 * kMiB;
  pool.backing = {CountedMap, CountedUnmap};
  pool.direct_free = [] { return fake_direct_free; };
  vfs::VirtualFileSystem fs;
  auto device = std::make_unique<vfs::MemoryDevice>(kMemoryMount, pool);
  Expect(device->Initialize(), "fix 150d: mounted " + kMemoryMount + " in memory");
  fs.RegisterDevice(std::move(device));

  // Sparse writes.
  const std::string name = "bars_1.img";
  vfs::File* file = OpenMemory(fs, name, vfs::FileDisposition::kOverwriteIf);
  Expect(file != nullptr, "fix 150d: " + name + " created");
  if (!file) return;
  const auto first = Pattern(kMiB, 1), second = Pattern(1000, 99);
  size_t written = 0;
  Expect(WriteAt(file, first, 0, &written) == X_STATUS_SUCCESS && written == kMiB, "fix 150d: 1 MiB written");
  Expect(WriteAt(file, second, 3 * kMiB + 100, &written) == X_STATUS_SUCCESS && written == 1000,
         "fix 150d: 1000 bytes written at 3 MiB + 100 (a hole before them)");
  file->entry()->update();
  Expect(file->entry()->size() == 3 * kMiB + 1100, "fix 150d: the entry's size is 3 MiB + 1100");
  Expect(ReadAt(file, kMiB, 0) == first, "fix 150d: the first MiB reads back");
  Expect(AllZero(ReadAt(file, 2 * kMiB, kMiB)), "fix 150d: the hole reads as zeros");
  Expect(ReadAt(file, 1000, 3 * kMiB + 100) == second, "fix 150d: the bytes after the hole read back");
  X_STATUS status = 0;
  Expect(ReadAt(file, 10, 3 * kMiB + 1100, &status).empty() && status == X_STATUS_END_OF_FILE,
         "fix 150d: a read at the end gives END_OF_FILE");
  Expect(ReadAt(file, 5000, 3 * kMiB + 1000).size() == 100, "fix 150d: a read over the end gives the rest");
  Expect(pool.used == 17 * kChunk, "fix 150d: 17 chunks in use (16 for the MiB, 1 after the hole): " +
                                       std::to_string(pool.used / kChunk));
  Expect(segment_maps == 1 && pool.mapped == mc::kSegmentBytes, "fix 150d: one segment mapped");
  // The 30 s line's files part: the files, the largest by their chunks, listed again only on a change.
  const std::string full_name = kMemoryMount + "\\" + name;
  std::string summary = pool.Summary();
  Expect(summary.find("; 1 files (0 deleted and still open, 0.0 MiB), 0.0 MiB given back in all") != std::string::npos &&
             summary.find("; largest: " + full_name + " 1.1 MiB") != std::string::npos,
         "fix 150d: the line lists 1 file, " + name + " largest with 17 chunks: " + summary);
  summary = pool.Summary();
  Expect(summary.find("; largest files as before") != std::string::npos && summary.find(full_name) == std::string::npos,
         "fix 150d: nothing changed, the largest files not listed again: " + summary);

  // Lengths.
  Expect(file->SetLength(kMiB / 2 + 10) == X_STATUS_SUCCESS && pool.used == 9 * kChunk,
         "fix 150d: a length of 512 KiB + 10 keeps 9 chunks");
  Expect(file->SetLength(kMiB) == X_STATUS_SUCCESS && AllZero(ReadAt(file, kMiB / 2 - 10, kMiB / 2 + 10)) &&
             ReadAt(file, kMiB / 2 + 10, 0) == Head(first, kMiB / 2 + 10),
         "fix 150d: lengthened again, the bytes past the cut read as zeros, those before it kept");

  // Two handles of a delete-on-close file.
  vfs::File* other = OpenMemory(fs, name, vfs::FileDisposition::kOpen);
  Expect(other && other->entry() == file->entry(), "fix 150d: a second handle of " + name);
  if (!other) return;
  file->entry()->SetForDeletion(true);
  file->Destroy();
  Expect(OpenMemory(fs, name, vfs::FileDisposition::kOpen, &status) == nullptr &&
             status == X_STATUS_OBJECT_NAME_NOT_FOUND,
         "fix 150d: the name gone at the first close");
  Expect(ReadAt(other, kMiB / 2, 0) == Head(first, kMiB / 2) && pool.used > 0,
         "fix 150d: the other handle still reads the data");
  summary = pool.Summary();
  Expect(summary.find("; 1 files (1 deleted and still open, 0.6 MiB)") != std::string::npos &&
             summary.find(full_name + " 0.6 MiB (deleted, open)") != std::string::npos,
         "fix 150d: the line counts the deleted file still open apart: " + summary);
  other->Destroy();
  Expect(pool.used == 0 && pool.mapped == 0 && segment_unmaps == 1,
         "fix 150d: its data and the segment given back at the last close");
  summary = pool.Summary();
  Expect(pool.given_back == 17 * kChunk &&
             summary.find("; 0 files (0 deleted and still open, 0.0 MiB), 1.1 MiB given back in all; largest: none") !=
                 std::string::npos,
         "fix 150d: 17 chunks given back in all (8 by the shorter length, 9 at the close): " + summary);

  // A renamed file is listed by its new name.
  if (vfs::File* renamed = OpenMemory(fs, "rename_me.img", vfs::FileDisposition::kOverwriteIf)) {
    WriteAt(renamed, second, 0, &written);
    renamed->entry()->Rename(std::filesystem::path("cache:\\renamed.img"));
    summary = pool.Summary();
    Expect(summary.find("; largest: " + kMemoryMount + "\\renamed.img 0.1 MiB") != std::string::npos,
           "fix 150d: the renamed file listed by its new name: " + summary);
    renamed->entry()->SetForDeletion(true);
    renamed->Destroy();
  }
  Expect(pool.used == 0, "fix 150d: the renamed file freed");

  // An overwrite while the old file is open.
  vfs::File* old_file = OpenMemory(fs, "anim.img", vfs::FileDisposition::kOverwriteIf);
  Expect(old_file && WriteAt(old_file, first, 0, &written) == X_STATUS_SUCCESS, "fix 150d: anim.img written");
  vfs::File* new_file = old_file ? OpenMemory(fs, "anim.img", vfs::FileDisposition::kOverwriteIf) : nullptr;
  Expect(new_file && new_file->entry() != old_file->entry() && ReadAt(new_file, 10, 0).empty() &&
             ReadAt(old_file, kMiB, 0) == first,
         "fix 150d: overwritten: a new empty file, the old handle still reads its data");
  if (old_file) old_file->Destroy();
  Expect(pool.used == 0, "fix 150d: the old data freed at its close");
  if (new_file) {
    new_file->entry()->SetForDeletion(true);
    new_file->Destroy();
  }

  // A directory deleted with its file.
  if (vfs::File* folder = OpenMemory(fs, "maps", vfs::FileDisposition::kOpenIf, nullptr, true)) folder->Destroy();
  vfs::File* in_dir = OpenMemory(fs, "maps\\cache010.map", vfs::FileDisposition::kOverwriteIf);
  Expect(in_dir && WriteAt(in_dir, first, 0, &written) == X_STATUS_SUCCESS, "fix 150d: maps\\cache010.map written");
  if (in_dir) in_dir->Destroy();
  Expect(fs.DeletePath(kMemoryMount + "\\maps") && pool.used == 0 &&
             !OpenMemory(fs, "maps\\cache010.map", vfs::FileDisposition::kOpen),
         "fix 150d: the directory deleted with its file, the memory given back");

  // The cap.
  pool.cap = 2 * kMiB;
  vfs::File* big = OpenMemory(fs, "componentpeds.img", vfs::FileDisposition::kOverwriteIf);
  const auto three = Pattern(3 * kMiB, 5);
  const uint64_t refused_before = pool.refused;
  status = big ? WriteAt(big, three, 0, &written) : X_STATUS_UNSUCCESSFUL;
  Expect(status == X_STATUS_DISK_FULL && written == 2 * kMiB && pool.used == 2 * kMiB &&
             pool.refused > refused_before,
         "fix 150d: 3 MiB over a 2 MiB cap: DISK_FULL, the 2 MiB up to it written");
  if (big) {
    big->entry()->update();
    Expect(big->entry()->size() == 2 * kMiB && ReadAt(big, 2 * kMiB, 0) == Head(three, 2 * kMiB),
           "fix 150d: the file holds the 2 MiB written");
    big->entry()->SetForDeletion(true);
    big->Destroy();
  }
  pool.cap = 64 * kMiB;

  // The direct memory reserve: no new segment while direct free memory would fall below it.
  fake_direct_free = 256 * kMiB + 8 * kMiB;
  vfs::File* reserved = OpenMemory(fs, "navmeshes.img", vfs::FileDisposition::kOverwriteIf);
  status = reserved ? WriteAt(reserved, second, 0, &written) : X_STATUS_UNSUCCESSFUL;
  Expect(status == X_STATUS_DISK_FULL && written == 0 && pool.mapped == 0,
         "fix 150d: direct free memory 8 MiB above the reserve: no 16 MiB segment mapped, DISK_FULL");
  fake_direct_free = 4096 * kMiB;
  status = reserved ? WriteAt(reserved, second, 0, &written) : X_STATUS_UNSUCCESSFUL;
  Expect(status == X_STATUS_SUCCESS && written == 1000, "fix 150d: with 4 GiB free the write goes in");
  if (reserved) {
    reserved->entry()->SetForDeletion(true);
    reserved->Destroy();
  }

  // Threads: writes, deletes and files made again.
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&fs, &errors, t] {
      const auto data = Pattern(200 * 1024, uint8_t(t));
      for (int round = 0; round < 40; ++round) {
        const std::string file_name = "thread" + std::to_string(t) + "_" + std::to_string(round % 3) + ".img";
        vfs::File* f = OpenMemory(fs, file_name, vfs::FileDisposition::kOverwriteIf);
        if (!f) {
          ++errors;
          continue;
        }
        size_t n = 0;
        const size_t at = size_t(round) * 4096;
        if (WriteAt(f, data, at, &n) != X_STATUS_SUCCESS || ReadAt(f, data.size(), at) != data) ++errors;
        if (round % 2) f->entry()->SetForDeletion(true);
        f->Destroy();
      }
    });
  }
  for (auto& thread : threads) thread.join();
  for (int t = 0; t < 4; ++t)
    for (int r = 0; r < 3; ++r)
      fs.DeletePath(kMemoryMount + "\\thread" + std::to_string(t) + "_" + std::to_string(r) + ".img");
  Expect(errors == 0 && pool.used == 0 && pool.mapped == 0 && segment_maps == segment_unmaps,
         "fix 150d: four threads writing, deleting and making files again; all given back (" +
             std::to_string(segment_maps.load()) + " segments mapped and unmapped)");
  Expect(pool.peak >= 2 * kMiB, "fix 150d: the peak kept (" + std::to_string(pool.peak / kMiB) + " MiB)");

  // The 30 s File I/O line (fix 148d) with the memory reads and writes and the pool's part.
  namespace pio = xbox360ps5::package_io;
  pio::FileIo io;
  pio::FileIoReport(io, 1'000'000'000ull);
  pio::Add(io.memory_writes, 3 * kMiB, 1000);
  pio::Add(io.memory_reads, kMiB, 1000);
  const std::string line = pio::FileIoReport(io, 32'000'000'000ull);
  const size_t part = line.find("; utility cache in memory (fix 150d)");
  Expect(line.find("memory writes 1 (3.0 MiB") != std::string::npos &&
             line.find("memory reads 1 (1.0 MiB") != std::string::npos && part != std::string::npos,
         "fix 150d: the File I/O line has the memory reads, writes and the pool" +
             (part != std::string::npos ? line.substr(part) : std::string(" (missing): ") + line));
}
#endif
}  // namespace

int main() {
  std::random_device random;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("check-vfs-entry-lifetime-" + std::to_string(random()));
  host = root / "cache";
  std::filesystem::create_directories(host / "cache1");  // (c)'s directory, found at the mount
  {
    vfs::VirtualFileSystem fs;
    auto device = std::make_unique<vfs::HostPathDevice>(kMount, host, false);
    Expect(device->Initialize(), "mounted " + kMount);
    fs.RegisterDevice(std::move(device));
    TwoHandles(fs);
    ReplacedWhileOpen(fs);
    DirectoryWithOpenFile(fs);
#if defined(XE_VFS_ENTRY_IO_TEST_HOOK) && defined(XE_VFS_ENTRY_DETACH)
    for (bool by_close : {false, true}) {
      OpenDuringSlowDelete(fs, vfs::FileDisposition::kCreate, "create", by_close);
      OpenDuringSlowDelete(fs, vfs::FileDisposition::kOpenIf, "openif", by_close);
      OpenDuringSlowDelete(fs, vfs::FileDisposition::kOpen, "open", by_close);
    }
#ifdef XE_VFS_BACKGROUND_DELETE
    BackgroundDelete(fs, true);
    BackgroundDelete(fs, false);
#ifdef XENIA_VFS_DEVICES_HOST_DELETE_STEPS_H_
    ShrinkSteps(fs);
#endif
#endif
#endif
  }
#ifdef XENIA_VFS_DEVICES_MEMORY_DEVICE_H_
  MemoryCache();  // (g) fix 150d
#endif
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::printf(failures ? "check-vfs-entry-lifetime: %d failure(s)\n" : "check-vfs-entry-lifetime: ok\n", failures);
  return failures ? 1 : 0;
}
