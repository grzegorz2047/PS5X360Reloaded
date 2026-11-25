// SPDX-License-Identifier: MIT
// Fix 171 (tools/source-1.0.2/fixes/171-xbox-fitness.patch; Nike+ Kinect Training 4D53090F,
// issue 74; Zumba Fitness Core 4D4A07F0): the Xbox Fitness exports of xam. Titles that link
// the FITNESS library (2.0.21173.0) look them up with XexGetProcedureAddress by ordinal; none
// existed, every call came back 0x80004001, and Nike's "GET ATHLETE FIT" screen waited forever
// after XamShowFitnessBodyProfileUI failed.
//
//   check-xbox-fitness        run from an assembled tree's root
//
//  (a) xbox360ps5/xbox_fitness.hpp: the 0x10-byte body profile record (packed, big-endian:
//      version, gender, height in metres, weight in kilograms, birth year), read back the way
//      the FITNESS library reads it (Zumba 82504474..82504530: height x 100, weight, current
//      year minus birth year, male unless the gender byte is 2) gives the library's own
//      defaults, 175 cm, 75 kg, 28 years, male; a short buffer gets only the leading bytes;
//      MsgTime is seconds since 2003-01-01 (xam's FILETIME base 0x01C2B128BA2B4000) and goes
//      back to the same date; the privacy setting is 1 off Live and 2 on it;
//  (b) the tree: each export is listed at its ordinal and declared in xam_fitness.cc, whose
//      group is registered (otherwise the static library would drop the file); Initialize
//      writes 1; the body profile comes from the header; the three screens complete at once
//      through xeXamDispatchHeadless without an overlapped and succeed;
//      XamFitnessConvertFloatMetToByteMet is left out on purpose (where its output pointer
//      goes is not known); XNuiDelayUI still answers success, no longer logged as
//      "unimplemented".
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/xbox_fitness.hpp"

namespace f = xbox360ps5::fitness;

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
static std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

static uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static float BeFloat(const uint8_t* p) {
  const uint32_t bits = Be32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

static void CheckRecord() {
  // The default body as the title's library reads it.
  const f::BodyProfile profile = f::DefaultBodyProfile(2026);
  uint8_t buffer[0x14];
  std::memset(buffer, 0xCD, sizeof(buffer));
  CHECK(f::WriteBodyProfileRecord(buffer, 0x10, profile) == 0x10);
  const uint8_t expected[0x10] = {0x01, 0x01, 0x3F, 0xE0, 0x00, 0x00, 0x42, 0x96,
                                  0x00, 0x00, 0x07, 0xCE, 0x00, 0x00, 0x00, 0x00};
  CHECK(std::memcmp(buffer, expected, sizeof(expected)) == 0);
  CHECK(buffer[0x10] == 0xCD && buffer[0x13] == 0xCD);  // nothing past the record
  const float centimetres = BeFloat(buffer + 2) * 100.0f;
  const float kilograms = BeFloat(buffer + 6);
  const int age = 2026 - (buffer[0xA] << 8 | buffer[0xB]);
  const bool male = buffer[1] != 2;
  CHECK(centimetres > 174.99f && centimetres < 175.01f);
  CHECK(kilograms == 75.0f);
  CHECK(age == 28);
  CHECK(male);
  // A female profile and a short buffer.
  f::BodyProfile woman = profile;
  woman.female = true;
  std::memset(buffer, 0xCD, sizeof(buffer));
  CHECK(f::WriteBodyProfileRecord(buffer, 2, woman) == 2);
  CHECK(buffer[0] == 1 && buffer[1] == 2 && buffer[2] == 0xCD);
  // A larger buffer still gets the 0x10 bytes only.
  std::memset(buffer, 0xCD, sizeof(buffer));
  CHECK(f::WriteBodyProfileRecord(buffer, 0x14, profile) == 0x10);
  CHECK(buffer[0x10] == 0xCD);
}

static void CheckTime() {
  // xam's MsgTime base (FILETIME of 2003-01-01) is the date used here.
  const int64_t days_1601_to_2003 = f::DaysFromCivil(2003, 1, 1) - f::DaysFromCivil(1601, 1, 1);
  CHECK(uint64_t(days_1601_to_2003) * 864000000000ull == 0x01C2B128BA2B4000ull);
  f::SystemTime t = {2003, 1, 0, 1, 0, 0, 0, 0};
  CHECK(f::ValidSystemTime(t));
  CHECK(f::SystemTimeToMsgTime(t) == 0);
  f::SystemTime back = f::MsgTimeToSystemTime(0);
  CHECK(back.year == 2003 && back.month == 1 && back.day == 1 && back.day_of_week == 3);
  // The bug report's time: 2026-11-25 10:21:27 UTC, a Wednesday.
  t = {2026, 11, 0, 25, 10, 21, 27, 0};
  const uint32_t msg = f::SystemTimeToMsgTime(t);
  CHECK(msg == uint32_t((f::DaysFromCivil(2026, 11, 25) - f::DaysFromCivil(2003, 1, 1)) * 86400 +
                        10 * 3600 + 21 * 60 + 27));
  back = f::MsgTimeToSystemTime(msg);
  CHECK(back.year == 2026 && back.month == 11 && back.day == 25 && back.hour == 10 &&
        back.minute == 21 && back.second == 27 && back.day_of_week == 3);
  // Leap day and year ends go back to themselves.
  const f::SystemTime dates[] = {{2024, 2, 0, 29, 23, 59, 59, 0},
                                 {2012, 12, 0, 31, 0, 0, 1, 0},
                                 {2100, 3, 0, 1, 12, 0, 0, 0}};
  for (const auto& d : dates) {
    const f::SystemTime r = f::MsgTimeToSystemTime(f::SystemTimeToMsgTime(d));
    CHECK(r.year == d.year && r.month == d.month && r.day == d.day && r.hour == d.hour &&
          r.minute == d.minute && r.second == d.second);
  }
  CHECK(!f::ValidSystemTime({2026, 13, 0, 1, 0, 0, 0, 0}));
  CHECK(!f::ValidSystemTime({2026, 1, 0, 0, 0, 0, 0, 0}));
  CHECK(!f::ValidSystemTime({2002, 12, 0, 31, 0, 0, 0, 0}));  // before MsgTime's start
  CHECK(!f::ValidSystemTime({2026, 1, 0, 1, 24, 0, 0, 0}));
  // The default birth year follows the host's year.
  CHECK(f::YearOfUnixTime(1795602087) == 2026);  // 2026-11-25
  CHECK(f::YearOfUnixTime(1798761599) == 2026);  // 2026-12-31 23:59:59
  CHECK(f::YearOfUnixTime(1798761600) == 2027);
  CHECK(f::YearOfUnixTime(0) == 2003);  // a clock at 1970 still gives a sane age
  CHECK(f::DefaultBodyProfile(f::YearOfUnixTime(1798761600)).birth_year == 1999);
  CHECK(f::PrivacySetting(false) == 1);
  CHECK(f::PrivacySetting(true) == 2);
}

static void CheckTree() {
  const std::string fitness = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_fitness.cc");
  const std::string table = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_table.inc");
  const std::string groups =
      ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_module_export_groups.inc");
  const std::string ui = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_ui.cc");
  const std::string app = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/apps/xam_app.cc");
  if (table.empty() || groups.empty() || ui.empty() || app.empty()) {
    std::printf("FAIL: no assembled tree here (xam_table.inc, xam_ui.cc, xam_app.cc)\n");
    ++failures;
    return;
  }
  struct Export {
    const char* ordinal;
    const char* name;
  };
  const Export exports[] = {
      {"0x000004CC", "XamFitnessInitialize"},
      {"0x00000523", "XamFitnessInitializeForOneUser"},
      {"0x000004DE", "XamFitnessUnInitialize"},
      {"0x000004C2", "XamFitnessGetCurrentBodyProfileRecord"},
      {"0x000004C6", "XamFitnessAddFitnessEvent"},
      {"0x000004FB", "XamFitnessGetPrivacySettings"},
      {"0x00000524", "XamFitnessGetSuggestedPrivacySettings"},
      {"0x000004DB", "XamFitnessSystemTimeToMsgTime"},
      {"0x000004DA", "XamFitnessMsgTimeToSystemTime"},
      {"0x000004B9", "XamShowFitnessBodyProfileUI"},
      {"0x000004DC", "XamShowFitnessWarnAboutPrivacyUI"},
      {"0x000004DD", "XamShowFitnessWarnAboutTimeUI"},
  };
  for (const auto& e : exports) {
    const std::string row = std::string("XE_EXPORT(xam,      ") + e.ordinal + ", " + e.name + ",";
    if (!Has(table, row.c_str())) std::printf("  missing table row: %s\n", row.c_str());
    CHECK(Has(table, row.c_str()));
    const std::string entry = std::string("dword_result_t ") + e.name + "_entry(";
    const std::string declared = std::string("DECLARE_XAM_EXPORT1(") + e.name + ", ";
    if (!Has(fitness, entry.c_str()) || !Has(fitness, declared.c_str()))
      std::printf("  not exported: %s\n", e.name);
    CHECK(Has(fitness, entry.c_str()));
    CHECK(Has(fitness, declared.c_str()));
  }
  // Deliberately left out: its output pointer's register is not known.
  CHECK(!Has(fitness, "XamFitnessConvertFloatMetToByteMet_entry"));
  // The group makes the linker keep the file.
  CHECK(Has(groups, "XE_MODULE_EXPORT_GROUP(xam, Fitness)"));
  CHECK(Has(fitness, "DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Fitness);"));
  CHECK(Has(fitness, "#include \"xbox360ps5/xbox_fitness.hpp\""));

  const std::string init = Body(fitness, "dword_result_t XamFitnessInitialize_entry(");
  CHECK(Has(init, "if (!ready) return X_E_INVALIDARG;"));
  CHECK(Has(init, "*ready = 1;"));
  CHECK(Has(init, "return X_E_SUCCESS;"));
  CHECK(Has(Body(fitness, "dword_result_t XamFitnessUnInitialize_entry("), "return X_E_SUCCESS;"));

  const std::string body =
      Body(fitness, "dword_result_t XamFitnessGetCurrentBodyProfileRecord_entry(");
  CHECK(Has(body, "uint32_t(size) & 0xFFFF"));
  CHECK(Has(body, "return X_E_INVALIDARG;"));
  CHECK(Has(body, "return X_E_FAIL;"));  // no profile signed in at that index, as xam
  CHECK(Has(body, "fitness::DefaultBodyProfile(CurrentYear())"));
  CHECK(Has(body, "fitness::WriteBodyProfileRecord(buffer.as<uint8_t*>(), length, profile);"));
  CHECK(Has(body, "return X_E_SUCCESS;"));

  CHECK(Has(Body(fitness, "dword_result_t XamFitnessAddFitnessEvent_entry("),
            "return X_E_SUCCESS;"));
  const std::string privacy = Body(fitness, "dword_result_t XamFitnessGetPrivacySettings_entry(");
  CHECK(Has(privacy, "fitness::PrivacySetting(user->IsSignedInToLive())"));

  // The screens: no overlapped, XN_SYS_UI up and down, success.
  const std::string screen = Body(fitness, "X_RESULT FitnessScreen(");
  CHECK(Has(screen, "xeXamDispatchHeadless([]() -> X_RESULT { return X_ERROR_SUCCESS; }, 0)"));
  for (const char* name : {"XamShowFitnessBodyProfileUI", "XamShowFitnessWarnAboutPrivacyUI",
                           "XamShowFitnessWarnAboutTimeUI"}) {
    const std::string entry = Body(fitness, std::string("dword_result_t ") + name + "_entry(");
    CHECK(Has(entry, "return FitnessScreen("));
  }
  // xam_ui.cc's helper keeps the signature xam_fitness.cc declares (external linkage).
  CHECK(Has(fitness, "X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, "
                     "uint32_t overlapped);"));
  const std::string headless = Body(ui, "X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()>");
  CHECK(!headless.empty());
  CHECK(!Has(ui, "static X_RESULT xeXamDispatchHeadless("));
  CHECK(Has(headless, "BroadcastNotification(kXNotificationSystemUI, true)"));
  CHECK(Has(headless, "BroadcastNotification(kXNotificationSystemUI, false)"));
  CHECK(Has(headless, "if (!overlapped) {"));

  // XNuiDelayUI (FE/2B001): success, not "unimplemented".
  const size_t at = app.find("case 0x0002B001: {");
  CHECK(at != std::string::npos);
  if (at != std::string::npos) {
    const std::string delay = app.substr(at, app.find("\n    }\n", at) - at);
    CHECK(Has(delay, "return X_E_SUCCESS;"));
    CHECK(Has(delay, "buffer_length >= 4"));
    CHECK(!Has(delay, "XNuiDelayUI unimplemented"));
    CHECK(Has(delay, "XNuiDelayUI({} ms): no Kinect system interface here, success (fix 171)"));
  }
}

int main() {
  CheckRecord();
  CheckTime();
  CheckTree();
  if (failures) {
    std::printf("check-xbox-fitness: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-xbox-fitness: ok\n");
  return 0;
}
