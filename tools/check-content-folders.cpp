// SPDX-License-Identifier: MIT
// Fix 101 (tools/source-1.0.2/fixes/101-content-folders.patch): content a game creates itself, and
// the device types. Battlefield 3 (45410950) on 1.0.5_candidate_62, "Install HD content":
//  * with its save drive ext1 (device 4) the game said "Device Not Allowed: only the Xbox 360 Hard
//    Drive" and asked again, and the save drive setting answered ext1 ten times: fix 27 showed
//    every drive slot as a memory unit;
//  * with the internal device it created the package file itself at XamContentResolve's
//    \Device\Harddisk0\Partition1\Content\0000000000000000\45410950\00000002\4541095000000000
//    (NtCreateFile overwrite-if) and got C000003A at every press: Partition1 is mounted only for a
//    game started from an executable, and the folders did not exist.
// The assembled tree:
//  (a) xam_content_device.cc: extended storage slots are hard drives, USB slots memory units;
//  (b) the VFS can register a device ahead of the others (the first whose mount is a prefix wins);
//  (c) ContentManager::MountPackageFolder: the guest folder of a device's packages of one
//      profile, title and type (\Device\Harddisk0\Partition1\Content\... for the internal device,
//      \Device\PS5X360Content<id>\... for a present drive, nothing otherwise), the host folder the
//      packages are opened from (DeviceRoot, PackageRoot) created first, mounted writable ahead of
//      the others, each once (never replaced while a file of it may be open);
//  (d) XamContentResolve gives that folder and the file name for every device but the disc, and
//      INVALID_PARAMETER for a device that is not there;
//  (e) xam_ui.cc: after kSavedRepeats answers from the save drive setting less than 30 s apart,
//      the device window is shown instead of answering the same drive again.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
const std::string X = ".deps/xenia-canary/src/xenia/";

void CheckDeviceTypes() {
  const std::string device = Read(X + "kernel/xam/xam_content_device.cc");
  const std::string make = Body(device, "static constexpr std::array<DummyDeviceInfo, sizeof...(I)> MakeDriveInfos(");
  Expect(Has(make, "storage::kSlots[I].kind == storage::Kind::kExtended ? DeviceType::HDD\n"
                   "                                                          : DeviceType::MU,"),
         "extended storage is a hard drive, USB storage a memory unit");
  Expect(!Has(make, "storage::kSlots[I].device_id), DeviceType::MU,"), "not every slot a memory unit");
  const std::string slots = Read(X + "kernel/xam/storage_devices.h");
  Expect(Has(slots, "{\"ext1\", 4, Kind::kExtended, u\"Extended Storage (ext1)\"},") &&
             Has(slots, "{\"usb0\", 7, Kind::kUsb, u\"USB Storage (usb0)\"},"),
         "ext1 is extended storage (device 4), usb0 USB storage (device 7)");
}

void CheckVfs() {
  const std::string header = Read(X + "vfs/virtual_file_system.h");
  const std::string vfs = Read(X + "vfs/virtual_file_system.cc");
  Expect(Has(header, "  Device* RegisterDeviceFirst(std::unique_ptr<Device> device);"), "the VFS declares RegisterDeviceFirst");
  const std::string first = Body(vfs, "Device* VirtualFileSystem::RegisterDeviceFirst(");
  Expect(Has(first, "auto global_lock = global_critical_region_.Acquire();") &&
             Has(first, "return devices_.insert(devices_.begin(), std::move(device))->get();"),
         "RegisterDeviceFirst puts the device ahead of the others, under the VFS lock");
  const std::string resolve = Body(vfs, "Entry* VirtualFileSystem::ResolvePath(");
  Expect(Has(resolve, "std::ranges::find_if(std::as_const(devices_)") && Has(resolve, "starts_with(normalized_path, d->mount_path())"),
         "the first device whose mount is a prefix of the path takes it");
}

void CheckMount() {
  const std::string header = Read(X + "kernel/xam/content_manager.h");
  Expect(Has(header, "  std::string MountPackageFolder(uint32_t device_id, uint64_t xuid,") &&
             Has(header, "  std::unordered_set<std::string> package_folders_;"),
         "content_manager.h declares MountPackageFolder and its folders");
  const std::string manager = Read(X + "kernel/xam/content_manager.cc");
  const std::string mount = Body(manager, "std::string ContentManager::MountPackageFolder(");
  Expect(Has(mount, "if (device_id == storage::kInternalDeviceId) {\n    root = \"\\\\Device\\\\Harddisk0\\\\Partition1\\\\Content\";") &&
             Has(mount, "} else if (storage::FindSlot(device_id) &&\n             storage::Find(StorageDevices(), device_id)) {\n"
                        "    root = fmt::format(\"\\\\Device\\\\PS5X360Content{}\", device_id);\n  } else {\n    return {};"),
         "the internal device under Partition1\\Content, a present drive under its own name, nothing else");
  Expect(Has(mount, "fmt::format(\"{}\\\\{:016X}\\\\{:08X}\\\\{:08X}\", root,\n                                         xuid, title_id, content_type);"),
         "the folder of one profile, title and content type");
  const size_t once = mount.find("if (package_folders_.count(folder)) {\n    return folder;");
  const size_t host = mount.find("storage::PackageRoot(DeviceRoot(device_id), xuid, title_id, content_type);");
  const size_t made = mount.find("std::filesystem::create_directories(host, error);");
  const size_t device = mount.find("std::make_unique<vfs::HostPathDevice>(folder, host, false);");
  const size_t init = mount.find("device->Initialize()");
  const size_t first = mount.find("kernel_state_->file_system()->RegisterDeviceFirst(std::move(device));");
  const size_t kept = mount.find("package_folders_.insert(folder);");
  Expect(once != std::string::npos && host != std::string::npos && made != std::string::npos && device != std::string::npos &&
             init != std::string::npos && first != std::string::npos && kept != std::string::npos && once < host &&
             host < made && made < device && device < init && init < first && first < kept,
         "each folder once: the packages' host folder created, then mounted writable ahead of the others");
  Expect(Has(mount, "std::lock_guard<std::mutex> lock(package_folders_mutex_);"), "under its own lock");
}

void CheckResolve() {
  const std::string content = Read(X + "kernel/xam/xam_content.cc");
  const size_t branch = content.find("if (content_data.device_id != static_cast<uint32_t>(DummyDeviceId::ODD)) {");
  const size_t mount = content.find("kernel_state()->content_manager()->MountPackageFolder(\n"
                                    "                content_data.device_id, xuid, kernel_state()->title_id(),\n"
                                    "                static_cast<uint32_t>(content_data.content_type.get()));",
                                    branch);
  const size_t invalid = content.find("if (folder.empty()) {\n          return X_ERROR_INVALID_PARAMETER;", mount);
  const size_t path = content.find("path_ptr, folder + \"\\\\\" + content_data.file_name(), path_size);", invalid);
  Expect(branch != std::string::npos && mount != std::string::npos && invalid != std::string::npos && path != std::string::npos,
         "XamContentResolve gives the mounted folder and the file name for every device but the disc");
  Expect(!Has(content, "root_device_path = \"\\\\Device\\\\Harddisk0\\\\Partition1\\\\Content\\\\\";"),
         "no unmounted Partition1 path any more");
}

void CheckRepeats() {
  const std::string ui = Read(X + "kernel/xam/xam_ui.cc");
  Expect(Has(ui, "#include <chrono>  // fix 101"), "xam_ui.cc includes <chrono>");
  const size_t repeats = ui.find("constexpr uint32_t kSavedRepeats = 3;");
  const size_t window = ui.find("constexpr auto kSavedRepeatWindow = std::chrono::seconds(30);", repeats);
  const size_t count = ui.find("now - saved_answered_at < kSavedRepeatWindow", window);
  const size_t refused = ui.find("if (saved_answers > kSavedRepeats) {", count);
  const size_t reset = ui.find("saved_answers = 0;\n      saved_refused = true;", refused);
  const size_t answer = ui.find("if (saved.state == storage::Saved::kUse && !saved_refused) {", reset);
  const size_t logged = ui.find("\"Storage: the game asked where to save (user {}); {} {} from the \"", answer);
  const size_t asks = ui.find("// The window is needed from here on.", logged);
  Expect(repeats != std::string::npos && window != std::string::npos && count != std::string::npos &&
             refused != std::string::npos && reset != std::string::npos && answer != std::string::npos &&
             logged != std::string::npos && asks != std::string::npos,
         "after 3 answers less than 30 s apart the window is shown instead of the same drive");
}

}  // namespace

int main() {
  CheckDeviceTypes();
  CheckVfs();
  CheckMount();
  CheckResolve();
  CheckRepeats();
  if (failures) {
    std::printf("check-content-folders: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-content-folders: ok\n");
  return 0;
}
