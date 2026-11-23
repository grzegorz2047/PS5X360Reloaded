// SPDX-License-Identifier: MIT
// Fix 144 (xenia/kernel/xam/low_half_size.h, xam_content_device.cc): the storage sizes a game
// is told are safe for a title that reads only their low 32 bits. 1.0.2 gave XamContentGetDeviceData
// and XamContentGetLocalizedDeviceData that rule (Just Cause 2, upstream issue 36: "not enough
// space" with 16 GiB free of 64 GiB, whose low halves are zero), but XamContentCreateDeviceEnumerator
// still gave the raw sizes.
//  (a) LowHalfSafeSize: never above the size, less than 4 GiB below it, a low half that is never
//      small (the size itself up to 4 GiB less 1 MiB, else 4 GiB less 1 MiB), the console's round
//      figures;
//  (b) the assembled tree: one rule (no copy of the old lambda), the device's data, its localized
//      data and the enumerator all through ReportedTotalBytes / ReportedFreeBytes, the disc's
//      fixed figures kept.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include("xenia/kernel/xam/low_half_size.h")
#include "xenia/kernel/xam/low_half_size.h"
#define HAVE_FIX_144 1
#else
#define HAVE_FIX_144 0
#endif

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
  return text.str();
}
size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}
// The text of one function: from its signature to the DECLARE line after it.
std::string Function(const std::string& text, const std::string& signature) {
  size_t begin = text.find(signature);
  if (begin == std::string::npos) return {};
  size_t end = text.find("DECLARE_XAM_EXPORT", begin);
  return text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

#if HAVE_FIX_144
using xe::kernel::xam::kLowHalfSafeLow;
using xe::kernel::xam::LowHalfSafeSize;
constexpr uint64_t kGiB = 1024ull * 1024 * 1024;

void CheckValue(uint64_t bytes) {
  const uint64_t told = LowHalfSafeSize(bytes);
  const uint32_t low = static_cast<uint32_t>(told);
  char what[160];
  std::snprintf(what, sizeof(what), "size %llu told as %llu", (unsigned long long)bytes,
                (unsigned long long)told);
  Expect(told <= bytes, std::string(what) + ": not above the size");
  Expect(bytes - told < (1ull << 32), std::string(what) + ": less than 4 GiB below it");
  if (bytes <= kLowHalfSafeLow) {
    Expect(told == bytes, std::string(what) + ": a size up to 4 GiB less 1 MiB stays");
  } else {
    Expect(low == kLowHalfSafeLow, std::string(what) + ": the low half is 4 GiB less 1 MiB");
  }
}

void CheckRule() {
  static_assert(LowHalfSafeSize(0) == 0);
  static_assert(LowHalfSafeSize(64 * 1024) == 64 * 1024);
  static_assert(LowHalfSafeSize(kLowHalfSafeLow) == kLowHalfSafeLow);
  static_assert(LowHalfSafeSize(kLowHalfSafeLow + 1) == kLowHalfSafeLow);
  static_assert(LowHalfSafeSize(4 * kGiB) == kLowHalfSafeLow);
  // The figures of upstream issue 36: 16 GiB free of 64 GiB, both zero in their low half.
  // 16 GiB is told as 16 GiB less 1 MiB.
  static_assert(LowHalfSafeSize(16 * kGiB) == 12 * kGiB + kLowHalfSafeLow);
  static_assert(LowHalfSafeSize(16 * kGiB) == 16 * kGiB - 1024 * 1024);
  static_assert(static_cast<uint32_t>(LowHalfSafeSize(16 * kGiB)) == kLowHalfSafeLow);
  static_assert(static_cast<uint32_t>(LowHalfSafeSize(64 * kGiB)) == kLowHalfSafeLow);
  static_assert(LowHalfSafeSize(16 * kGiB + kLowHalfSafeLow) == 16 * kGiB + kLowHalfSafeLow);
  static_assert(LowHalfSafeSize(UINT64_MAX) == 0xFFFFFFFFFFF00000ull);
  // Round figures a PS5 drive gives (free and capacity), and a sweep across the low halves.
  for (uint64_t gib : {1ull, 3ull, 4ull, 5ull, 8ull, 16ull, 64ull, 100ull, 256ull, 825ull, 1024ull,
                       2048ull, 4096ull, 8192ull}) {
    CheckValue(gib * kGiB);
    CheckValue(gib * kGiB - 1);
    CheckValue(gib * kGiB + 1);
  }
  for (uint64_t high = 0; high < 4; ++high) {
    for (uint64_t low = 0; low <= 0xFFFFFFFFull; low += 0x00F0F0F1ull) {
      CheckValue((high << 32) | low);
    }
    CheckValue((high << 32) | 0xFFFFFFFFull);
    CheckValue((high << 32) | kLowHalfSafeLow);
    CheckValue((high << 32) | (kLowHalfSafeLow - 1));
  }
}
#endif

void CheckTree() {
  const std::string dir = ".deps/xenia-canary/src/xenia/kernel/xam/";
  const std::string cc = Read(dir + "xam_content_device.cc");
  const std::string h = Read(dir + "xam_content_device.h");
  if (cc.empty() || h.empty()) {
    std::printf("skip the tree part (no assembled tree here)\n");
    return;
  }
  Expect(cc.find("#include \"xenia/kernel/xam/low_half_size.h\"") != std::string::npos,
         "xam_content_device.cc includes low_half_size.h");
  Expect(cc.find("low_half_safe") == std::string::npos,
         "the old per-function lambda is gone (one rule, low_half_size.h)");
  Expect(h.find("uint64_t ReportedTotalBytes(const DummyDeviceInfo* device_info);") !=
                 std::string::npos &&
             h.find("uint64_t ReportedFreeBytes(const DummyDeviceInfo* device_info);") !=
                 std::string::npos,
         "xam_content_device.h declares ReportedTotalBytes and ReportedFreeBytes");

  const std::string total = Function(cc, "uint64_t ReportedTotalBytes(");
  const std::string free = Function(cc, "uint64_t ReportedFreeBytes(");
  Expect(total.find("DeviceType::ODD") != std::string::npos &&
             total.find("return device_info->total_bytes;") != std::string::npos &&
             total.find("LowHalfSafeSize(DeviceTotalBytes(device_info))") != std::string::npos,
         "ReportedTotalBytes: the disc's fixed figure, else LowHalfSafeSize of the drive's");
  Expect(free.find("DeviceType::ODD") != std::string::npos &&
             free.find("return device_info->free_bytes;") != std::string::npos &&
             free.find("LowHalfSafeSize(DeviceFreeBytes(device_info))") != std::string::npos,
         "ReportedFreeBytes: the disc's fixed figure, else LowHalfSafeSize of the drive's");

  for (const char* name : {"dword_result_t XamContentGetDeviceData_entry(",
                           "dword_result_t XamContentGetLocalizedDeviceData_entry(",
                           "dword_result_t XamContentCreateDeviceEnumerator_entry("}) {
    const std::string body = Function(cc, name);
    Expect(!body.empty(), std::string(name) + " is in the tree");
    Expect(body.find("device_data->total_bytes = ReportedTotalBytes(device_info);") !=
                   std::string::npos &&
               body.find("device_data->free_bytes = ReportedFreeBytes(device_info);") !=
                   std::string::npos,
           std::string(name) + " gives the reported sizes");
    Expect(body.find("= DeviceTotalBytes(") == std::string::npos &&
               body.find("= DeviceFreeBytes(") == std::string::npos,
           std::string(name) + " gives no raw size");
  }
  Expect(Count(cc, "ReportedFreeBytes(device_info);") == 3,
         "the free space is told in exactly three places");
}

}  // namespace

int main() {
#if HAVE_FIX_144
  CheckRule();
  CheckTree();
#else
  std::printf("skip: fix 144 (low_half_size.h) is not in this tree\n");
  return 0;
#endif
  if (failures) {
    std::printf("check-low-half-size: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-low-half-size: all checks passed\n");
  return 0;
}
