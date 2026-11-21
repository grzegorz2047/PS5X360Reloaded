// SPDX-License-Identifier: MIT
// Fix 92: a read from a disc image that stops at the file's end inside a 2 KiB sector fills the
// rest of that sector in the guest's buffer (include/xbox360ps5/disc_sector_pad.hpp), as a
// console's sector read: the disc's bytes, zeros where the image ends; never past the length
// asked for; the bytes read reported stay the file's.
//
//   check-disc-sector-pad        run from an assembled tree's root
//
// Synthetic files only (no game data).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/disc_sector_pad.hpp"

namespace d = xbox360ps5::disc;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The read as disc_image_file.cc does it, on a synthetic image: `file` at a sector boundary,
// the image `image_tail` bytes longer than the file's last sector's start.
struct Image {
  std::vector<uint8_t> bytes;
  size_t data_offset = 0, data_size = 0;
  size_t Read(uint8_t* buffer, size_t length, size_t offset) const {
    const size_t copied = std::min(length, data_size - offset);
    std::memcpy(buffer, bytes.data() + data_offset + offset, copied);
    const size_t pad = d::SectorPadLength(data_size, offset, copied, length);
    const size_t at = data_offset + offset + copied;
    if (pad) {
      if (at + pad <= bytes.size()) std::memcpy(buffer + copied, bytes.data() + at, pad);
      else std::memset(buffer + copied, 0, pad);
    }
    return copied;
  }
};

int main() {
  CHECK(d::kSectorBytes == 2048);
  // Kinect Sports' 4F45161E: 2258 bytes, 0x20000 asked: the rest of its second sector.
  CHECK(d::SectorPadLength(2258, 0, 2258, 0x20000) == 4096 - 2258);
  // A shorter request: never past it.
  CHECK(d::SectorPadLength(2258, 0, 2258, 2300) == 42);
  // Not at the file's end (the request was met), or the file ends on a sector boundary: nothing.
  CHECK(d::SectorPadLength(2258, 0, 1000, 1000) == 0);
  CHECK(d::SectorPadLength(4096, 0, 4096, 0x20000) == 0);
  CHECK(d::SectorPadLength(0x20000, 0x10000, 0x10000, 0x20000) == 0);
  // From an offset: the sector is the file's (data starts on a sector boundary).
  CHECK(d::SectorPadLength(10000, 8192, 1808, 4096) == 10240 - 10000);
  // A read that stopped short of the file's end (it cannot, but must not pad): nothing.
  CHECK(d::SectorPadLength(10000, 0, 500, 4096) == 0);
  // Fix 90's 4-byte query at offset 0 of a 2258-byte file: nothing.
  CHECK(d::SectorPadLength(2258, 0, 4, 4) == 0);

  // The read itself, on a synthetic image: the guest's buffer gets the file, then the disc's
  // bytes of its last sector (marked 0x5A here), and its own bytes after that stay.
  {
    Image image;
    image.data_offset = 4 * 2048;
    image.data_size = 2258;
    image.bytes.assign(image.data_offset + 4096, 0x5A);
    for (size_t n = 0; n < image.data_size; ++n) image.bytes[image.data_offset + n] = uint8_t(n * 7);
    std::vector<uint8_t> guest(0x20000, 0xCC);
    const size_t read = image.Read(guest.data(), guest.size(), 0);
    CHECK(read == 2258);
    CHECK(guest[2257] == uint8_t(2257 * 7));
    CHECK(guest[2258] == 0x5A && guest[4095] == 0x5A);
    CHECK(guest[4096] == 0xCC && guest[0x1FFFF] == 0xCC);
    // The image ends inside the sector: zeros instead.
    image.bytes.resize(image.data_offset + 2300);
    std::vector<uint8_t> guest2(0x20000, 0xCC);
    image.Read(guest2.data(), guest2.size(), 0);
    CHECK(guest2[2258] == 0 && guest2[4095] == 0 && guest2[4096] == 0xCC);
  }

  const std::string file = ReadText(".deps/xenia-canary/src/xenia/vfs/devices/disc_image_file.cc");
  if (file.empty()) {
    std::printf("no assembled tree here; the disc read not checked\n");
  } else {
    CHECK(file.find("#include \"xbox360ps5/disc_sector_pad.hpp\"  // fix 92") != std::string::npos);
    const size_t copy = file.find("if (!entry_->mmap()->ReadAt(real_offset, buffer.data(), real_length)) {");
    const size_t pad = file.find("xbox360ps5::disc::SectorPadLength(entry_->data_size(), byte_offset,");
    const size_t from_disc = file.find("entry_->mmap()->ReadAt(real_offset + real_length, tail, pad);");
    const size_t zeros = file.find("std::memset(tail, 0, pad);");
    const size_t result = file.find("*out_bytes_read = real_length;");
    CHECK(copy != std::string::npos && copy < pad && pad < from_disc && from_disc < zeros && zeros < result &&
          result != std::string::npos);
    CHECK(file.find("real_length, buffer.size());") != std::string::npos);  // never past the request
    CHECK(file.find("(fix 92)") != std::string::npos);
    std::printf("tree: a disc read that ends inside a sector gets the rest of it\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-disc-sector-pad: all passed\n");
  return 0;
}
