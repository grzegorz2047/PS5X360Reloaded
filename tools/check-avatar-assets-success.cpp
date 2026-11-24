// SPDX-License-Identifier: MIT
// Fix 156p (tools/source-1.0.2/fixes/156p-avatar-assets-success.patch; Kinect Sports Season Two
// 4D5309D6, issue 63): XamAvatarGetAssets completes with success for a title that requires
// Kinect. Upstream completes it with ERROR_FUNCTION_FAILED after fix 99's empty asset set;
// Season Two's avatar loader keeps the call in an async record (82533478) that turns any result
// other than 0 or 0x3E4 into "failed" for good, and its sport loads (darts, tennis) wait for
// "done" forever (1.0.6-work-fix156o's object dump: the record in state 3).
//
//   check-avatar-assets-success        run from an assembled tree's root
//
//  (a) GetAssetsCompletion: success for a Kinect title, upstream's failure for any other;
//  (b) a model of Season Two's record (82533478): the failure leaves it failed, success done;
//      a model of Kinect Sports' state machine (825252B8): the loader runs either way;
//  (c) the tree: XamAvatarGetAssets completes with GetAssetsCompletion(KinectRequiredTitle()).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/avatar_assets.hpp"

namespace aa = xbox360ps5::avatar_assets;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

static std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  std::string out;
  for (char c : s.str())
    if (c != '\r') out += c;
  return out;
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

// Season Two 82533478: state 1 polls the overlapped (XGetOverlappedResult, no wait).
static uint32_t SeasonTwoRecord(uint32_t state, uint32_t overlapped_result) {
  if (state != 1) return state;
  if (overlapped_result == 0) return 2;      // done
  if (overlapped_result == 0x3E4) return 1;  // ERROR_IO_INCOMPLETE: still pending
  return 3;                                  // failed, for good
}
// Kinect Sports 825252B8: a failure the first time retries; a second failure or success loads.
static int KinectSportsRequests(uint32_t result) {
  int requests = 1;
  if (result != 0) ++requests;  // the retry, which fails the same way
  return requests;              // then the loader 82524770
}

static void CheckCompletion() {
  const auto kinect = aa::GetAssetsCompletion(true);
  const auto other = aa::GetAssetsCompletion(false);
  CHECK(kinect.result == 0 && kinect.extended == 0);
  CHECK(other.result == 0x65B && other.extended == 0x80004005);  // upstream (58410907)
  CHECK(SeasonTwoRecord(1, other.result) == 3);   // the sport load never ends
  CHECK(SeasonTwoRecord(1, kinect.result) == 2);  // done
  CHECK(SeasonTwoRecord(1, 0x3E4) == 1);
  CHECK(KinectSportsRequests(other.result) == 2 && KinectSportsRequests(kinect.result) == 1);
}

static void CheckTree() {
  const std::string avatar = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_avatar.cc");
  if (avatar.empty()) {
    std::printf("FAIL: no assembled tree here (xam_avatar.cc)\n");
    ++failures;
    return;
  }
  const size_t entry = avatar.find("dword_result_t XamAvatarGetAssets_entry(");
  const size_t next = avatar.find("DECLARE_XAM_EXPORT1(XamAvatarGetAssets,", entry);
  CHECK(entry != std::string::npos && next != std::string::npos);
  const std::string fn = entry == std::string::npos ? "" : avatar.substr(entry, next - entry);
  CHECK(Has(fn, "xbox360ps5::avatar_assets::GetAssetsCompletion(KinectRequiredTitle(kernel_state()));"));
  CHECK(Has(fn, "overlapped_ptr, completion.result, completion.extended, 0);"));
  CHECK(!Has(fn, "X_ERROR_FUNCTION_FAILED, X_E_FAIL"));
  CHECK(Has(fn, "PrepareEmptyAssets("));  // fix 99's empty set is still written first
  CHECK(Has(fn, "Avatars: the asset request completes with {:X} (fix 156p)"));
}

int main() {
  CheckCompletion();
  CheckTree();
  if (failures) {
    std::printf("check-avatar-assets-success: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-avatar-assets-success: ok\n");
  return 0;
}
