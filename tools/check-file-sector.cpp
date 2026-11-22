// SPDX-License-Identifier: MIT
// Fix 121 (tools/source-1.0.2/fixes/121-file-sector.patch): NtQueryInformationFile's
// XFileSectorInformation (class 0x1A, a file's start sector on the disc), which games use to order
// their disc reads. The Sims 3 (454108E2) asked about 7,000 times in a 140 s load (session 040236:
// the GPU waiting for the guest, frames 4508 -> 4539) while the stub answered a hash of the path,
// a random-looking number, and logged "Stub XFileSectorInformation!" every time.
//  (a) the order of disc files: a GDFX entry's data offset is the game partition's start plus
//      sector * 2048 (disc_image_device.cc), so offset / 2048 orders files as the disc does;
//  (b) the assembled tree: a file on a disc image answers that sector, big-endian as the guest
//      reads it; other files keep the stable hash; one log line for each kind, none per call.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
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
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
std::string Body(const std::string& text, const std::string& start, const std::string& end) {
  const size_t a = text.find(start);
  if (a == std::string::npos) return "";
  const size_t b = text.find(end, a);
  return text.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

void CheckOrder() {
  // A disc whose game partition starts at sector 0x20600 (an XGD2 disc's) and three files at
  // sectors 0x40, 0x1000 and 0x2A: their offsets as disc_image_device.cc gives them, and back.
  const uint64_t game_offset = 0x20600ull * 0x800;
  const uint32_t sectors[] = {0x40, 0x1000, 0x2A};
  uint32_t answers[3];
  for (int i = 0; i < 3; ++i) {
    const uint64_t data_offset = game_offset + uint64_t(sectors[i]) * 0x800;
    answers[i] = uint32_t(data_offset / 0x800);
  }
  Expect(answers[0] == 0x20640 && answers[1] == 0x21600 && answers[2] == 0x2062A, "offset / 2048 is the disc sector");
  Expect(answers[2] < answers[0] && answers[0] < answers[1], "the answers order the files as the disc does");
}

void CheckTree() {
  const std::string io = Read(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc");
  Expect(Has(io, "#include \"xenia/vfs/devices/disc_image_entry.h\"  // fix 121"), "the disc image entry is known");
  const std::string sector = Body(io, "    case XFileSectorInformation: {", "      out_length = sizeof(uint32_t);\n      break;");
  Expect(!sector.empty(), "the XFileSectorInformation case is there");
  const size_t disc = sector.find("if (auto* disc = dynamic_cast<vfs::DiscImageEntry*>(file->entry())) {");
  const size_t answer = sector.find("*info = static_cast<uint32_t>(disc->data_offset() / 0x800);", disc);
  const size_t other = sector.find("} else {", answer);
  const size_t hash = sector.find("*info = static_cast<uint32_t>(fname_hash ^ (fname_hash >> 32));", other);
  Expect(disc != std::string::npos && answer != std::string::npos && other != std::string::npos &&
             hash != std::string::npos,
         "a disc file answers its start sector, any other file the stable hash");
  Expect(Has(sector, "auto info = info_ptr.as<xe::be<uint32_t>*>();"), "big-endian, as the guest reads it");
  Expect(!Has(io, "XELOGW(\"Stub XFileSectorInformation!\");"), "no log line per call");
  Expect(Has(sector, "static std::atomic<bool> logged_disc{false}, logged_other{false};") &&
             Has(sector, "if (!logged_disc.exchange(true)) {") && Has(sector, "if (!logged_other.exchange(true)) {"),
         "one log line for each kind");
  Expect(Has(io, "#include <atomic>"), "std::atomic is included");
  const std::string device = Read(".deps/xenia-canary/src/xenia/vfs/devices/disc_image_device.cc");
  Expect(Has(device, "entry->data_offset_ = state->game_offset + (sector * kXESectorSize);"),
         "a GDFX entry's data offset is the game partition's start plus its sector");
  const std::string entry = Read(".deps/xenia-canary/src/xenia/vfs/devices/disc_image_entry.h");
  Expect(Has(entry, "size_t data_offset() const { return data_offset_; }"), "DiscImageEntry gives its data offset");
}

}  // namespace

int main() {
  CheckOrder();
  CheckTree();
  if (failures) {
    std::printf("check-file-sector: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-file-sector: ok\n");
  return 0;
}
