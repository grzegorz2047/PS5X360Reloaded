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
//      948 ms): an open of the same name meanwhile waits for the delete and gets a new entry
//      (create, open-if), or finds no file (open), never the entry being deleted.
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

#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/entry.h"
#include "xenia/vfs/entry_io.h"
#include "xenia/vfs/virtual_file_system.h"

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

#ifdef XE_VFS_ENTRY_IO_TEST_HOOK
std::atomic<bool> in_delete{false};
void SlowDelete(vfs::entry_io::Kind kind) {
  if (kind != vfs::entry_io::kDelete) return;
  in_delete = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

// (d) Fix 129's window: an open of the name being deleted.
void OpenDuringSlowDelete(vfs::VirtualFileSystem& fs, vfs::FileDisposition disposition, const char* what) {
  const std::string name = std::string("window_") + what + ".temp";
  vfs::File* made = Open(fs, name, vfs::FileDisposition::kOverwriteIf);
  Expect(made && Write(made, 5), name + ": created");
  if (!made) return;
  vfs::Entry* dying = made->entry();
  made->Destroy();
  in_delete = false;
  vfs::entry_io::test_hook = SlowDelete;
  std::thread deleter([&] { fs.DeletePath(kMount + "\\" + name); });
  while (!in_delete) std::this_thread::yield();
  const auto start = std::chrono::steady_clock::now();
  X_STATUS status = 0;
  vfs::File* file = Open(fs, name, disposition, &status);
  const double waited_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  deleter.join();
  vfs::entry_io::test_hook = nullptr;
  char ms[32];
  std::snprintf(ms, sizeof(ms), "%.0f ms", waited_ms);
  if (disposition == vfs::FileDisposition::kOpen) {
    Expect(!file && status == X_STATUS_OBJECT_NAME_NOT_FOUND && waited_ms >= 150,
           name + ": open waited for the delete (" + ms + ") and found no file (status " + Hex(status) + ")");
    if (file) file->Destroy();
    return;
  }
  Expect(file && waited_ms >= 150,
         name + ": waited for the delete (" + ms + ") and opened (status " + Hex(status) + ")");
  if (!file) return;
  Expect(file->entry() != dying && !file->entry()->is_deleting() && !file->entry()->is_removed(),
         name + ": a new entry, not the one being deleted");
  Expect(Write(file, 6) && OnHost(name), name + ": the new file is on the host after the delete");
  file->entry()->SetForDeletion(true);
  file->Destroy();
  Expect(!OnHost(name), name + ": deleted at its close");
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
#ifdef XE_VFS_ENTRY_IO_TEST_HOOK
    OpenDuringSlowDelete(fs, vfs::FileDisposition::kCreate, "create");
    OpenDuringSlowDelete(fs, vfs::FileDisposition::kOpenIf, "openif");
    OpenDuringSlowDelete(fs, vfs::FileDisposition::kOpen, "open");
#endif
  }
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::printf(failures ? "check-vfs-entry-lifetime: %d failure(s)\n" : "check-vfs-entry-lifetime: ok\n", failures);
  return failures ? 1 : 0;
}
