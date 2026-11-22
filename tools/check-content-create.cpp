// SPDX-License-Identifier: MIT
// Fix 101, the create itself (tools/check-content-create.sh builds this against the assembled
// tree's xenia/base and xenia/vfs). Battlefield 3's HD content on 1.0.5-work-fix101: the package
// folder was mounted (\Device\PS5X360Content4\...\00000002), but NtCreateFile of
// ...\4541095000000000 (overwrite-if, GENERIC_WRITE) got C0000022 eleven times, each after
// "ResolvePath(Device) failed - device not found": VirtualFileSystem::OpenFile created the file
// with CreatePath, which walks the path again from its first part ("Device", no device).
// Here: the console's layout (a \Device\Harddisk0 null device registered first, then the package
// folders mounted ahead of it), a file created with overwrite-if through each mount, written,
// found on the host, then overwritten.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>

#include "xenia/vfs/devices/host_path_device.h"
#include "xenia/vfs/devices/null_device.h"
#include "xenia/vfs/virtual_file_system.h"

using namespace xe;  // X_STATUS

// The console's crash report (platform/ps5/crash_report.cpp) records stages the host file code
// names; this check runs without it.
namespace xbox360ps5 {
void Stage(const char*) {}
}  // namespace xbox360ps5

namespace {

int failures = 0;
void Expect(bool condition, const std::string& what) {
  std::printf("%s: %s\n", condition ? "ok" : "FAIL", what.c_str());
  if (!condition) ++failures;
}
std::string Hex(uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", value);
  return text;
}

void Create(xe::vfs::VirtualFileSystem& vfs, const std::string& mount, const std::filesystem::path& host) {
  const std::string path = mount + "\\4541095000000000";
  for (int round = 0; round < 2; ++round) {
    xe::vfs::File* file = nullptr;
    xe::vfs::FileAction action = xe::vfs::FileAction::kDoesNotExist;
    // Battlefield 3's call: access 40110000 (GENERIC_WRITE | SYNCHRONIZE | DELETE), disposition 5.
    const X_STATUS status = vfs.OpenFile(nullptr, path, xe::vfs::FileDisposition::kOverwriteIf, 0x40110000u, false,
                                         true, &file, &action);
    const auto expected = round == 0 ? xe::vfs::FileAction::kCreated : xe::vfs::FileAction::kOverwritten;
    Expect(status == X_STATUS_SUCCESS && file && action == expected,
           std::string(round == 0 ? "created" : "overwritten") + " through " + mount + " (status " + Hex(status) +
               ", action " + std::to_string(int(action)) + ")");
    if (!file) return;
    const uint8_t data[4] = {'C', 'O', 'N', static_cast<uint8_t>(round)};
    size_t written = 0;
    Expect(file->WriteSync(std::span<const uint8_t>(data, sizeof(data)), 0, &written) == X_STATUS_SUCCESS &&
               written == sizeof(data),
           "written");
    file->Destroy();
    std::error_code error;
    Expect(std::filesystem::file_size(host / "4541095000000000", error) == sizeof(data) && !error,
           "on the host in " + host.string());
  }
}

}  // namespace

int main() {
  std::random_device random;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("check-content-create-" + std::to_string(random()));
  const std::filesystem::path drive = root / "ext1/saves/0000000000000000/45410950/00000002";
  const std::filesystem::path internal = root / "internal/saves/0000000000000000/45410950/00000002";
  std::filesystem::create_directories(drive);
  std::filesystem::create_directories(internal);

  xe::vfs::VirtualFileSystem vfs;
  auto null_device = std::make_unique<xe::vfs::NullDevice>(
      "\\Device\\Harddisk0", std::initializer_list<std::string>{"\\Partition0", "\\Cache0", "\\Cache1"});
  Expect(null_device->Initialize(), "the \\Device\\Harddisk0 null device");
  vfs.RegisterDevice(std::move(null_device));

  const std::string drive_mount = "\\Device\\PS5X360Content4\\0000000000000000\\45410950\\00000002";
  const std::string internal_mount = "\\Device\\Harddisk0\\Partition1\\Content\\0000000000000000\\45410950\\00000002";
  for (const auto& [mount, host] : {std::pair{drive_mount, drive}, std::pair{internal_mount, internal}}) {
    auto device = std::make_unique<xe::vfs::HostPathDevice>(mount, host, false);
    Expect(device->Initialize(), "mounted " + mount);
    vfs.RegisterDeviceFirst(std::move(device));
  }
  Create(vfs, drive_mount, drive);
  Create(vfs, internal_mount, internal);

  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::printf(failures ? "check-content-create: %d failure(s)\n" : "check-content-create: ok\n", failures);
  return failures ? 1 : 0;
}
