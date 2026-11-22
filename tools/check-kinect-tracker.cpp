// SPDX-License-Identifier: MIT
// Fix 106 (Kinect Sports, issue 15): the console side of the phone tracker's change.
//
//   check-kinect-tracker        run from an assembled tree's root
//
//  (a) include/xbox360ps5/phone_latency.hpp: the page's report with its reasons and calibration
//      restarts gives the line "Motion: phone frames without a body, last 2 s: ..."; nothing for a
//      report without any; the stages line reads the same report as before; bad input is ignored;
//  (b) port/phone_camera_tls.cpp serves the title's tracker (phone_camera_tracker.hpp) at
//      /kinect_tracker.mjs before the package's files, and logs the new line with the stages';
//  (c) the tracker module compiles in, made from the package's with the fix 106 changes and its
//      licence. Its behaviour is checked with node (tools/check-kinect-tracker.mjs).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../port/phone_camera_tracker.hpp"  // the check runs as <tree>/tools/check-kinect-tracker.cpp
#include "xbox360ps5/phone_latency.hpp"

namespace pl = xbox360ps5::phone_latency;

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
  return s.str();
}

int main() {
  // (a) A report as StageWindow makes it (tools/check-kinect-tracker.mjs writes the same shape).
  const std::string report =
      "{\"fps\":25.3,\"frames\":51,\"queue\":2,\"stages\":{\"camera\":[17,37,51],\"detect\":[32,33,51],"
      "\"track\":[-1,-1,0],\"network\":[-1,-1,0],\"smoothing\":[-1,-1,0]},\"reasons\":{\"no_landmarks\":0,"
      "\"stale\":2,\"no_body\":40,\"out_of_range\":0,\"out_of_order\":0,\"hips_inferred\":120,\"hips_hidden\":3},"
      "\"restarts\":\"recentre 640x480, frame 1280x960\"}";
  const auto line = pl::ReasonsLine(report);
  CHECK(line.has_value());
  if (line) {
    std::printf("   %s\n", line->c_str());
    CHECK(*line == "Motion: phone frames without a body, last 2 s: stale 2, no_body 40; kept with hips inferred "
                   "120, hips hidden 3; calibration restarted: recentre 640x480, frame 1280x960");
  }
  const auto stages = pl::Line(report);
  CHECK(stages.has_value() && stages->find("detection 32 ms (p95 33), tracking to send none") != std::string::npos &&
        stages->find("; 25.3 fps, queue 2") != std::string::npos);
  // Nothing to say: no line.
  CHECK(!pl::ReasonsLine("{\"fps\":30,\"reasons\":{\"no_body\":0,\"stale\":0},\"restarts\":\"\"}").has_value());
  CHECK(!pl::ReasonsLine("{\"fps\":30,\"queue\":0}").has_value());  // a page before fix 106
  // Only kept bodies, or only a restart.
  CHECK(pl::ReasonsLine("{\"reasons\":{\"hips_inferred\":5}}").value_or("") ==
        "Motion: phone frames without a body, last 2 s: none; kept with hips inferred 5");
  CHECK(pl::ReasonsLine("{\"restarts\":\"camera 640x480\"}").value_or("") ==
        "Motion: phone frames without a body, last 2 s: none; calibration restarted: camera 640x480");
  // Bad input: not an object, too long, an open reasons object, odd characters in the restarts.
  CHECK(!pl::ReasonsLine("").has_value() && !pl::ReasonsLine("[1]").has_value());
  CHECK(!pl::ReasonsLine("{\"reasons\":{\"no_body\":4").has_value());
  CHECK(!pl::ReasonsLine("{" + std::string(4000, ' ') + "}").has_value());
  CHECK(pl::ReasonsLine("{\"restarts\":\"frame<script>%n 1x1\\\"}").value_or("") ==
        "Motion: phone frames without a body, last 2 s: none; calibration restarted: framescriptn 1x1");
  // A reason outside the object is not counted.
  CHECK(!pl::ReasonsLine("{\"reasons\":{},\"no_body\":9}").has_value());

  // (c) The module.
  const std::string tracker = phone_camera::kTrackerModule;
  CHECK(tracker.find("export class KinectTracker") != std::string::npos);
  CHECK(tracker.find("import {skeleton} from './pose.mjs?v=kinect2';") != std::string::npos);
  CHECK(tracker.find("PS5X360Reloaded fix 106") != std::string::npos);
  CHECK(tracker.find("if ([11,12].some(n => vis(world[n]) < .5)) {") != std::string::npos);
  CHECK(tracker.find("return {pose: null, status: this.status('hips_hidden')};") == std::string::npos);
  CHECK(tracker.find("takeEvents()") != std::string::npos);

  const std::string header = ReadText("port/phone_camera_tracker.hpp");
  const std::string tls = ReadText("port/phone_camera_tls.cpp");
  if (header.empty() || tls.empty()) {
    std::printf("no assembled tree here; the server not checked\n");
  } else {
    CHECK(header.find("MIT, Copyright (c) 2026 Xbox360PS5\n// contributors") != std::string::npos);
    // (b) Served before the package's files, and the new line logged with the stages'.
    const size_t ours = tls.find("if (method == \"GET\" && target == \"/kinect_tracker.mjs\")  // fix 106");
    const size_t theirs = tls.find("target == \"/pose.mjs\" || target == \"/kinect_tracker.mjs\"");
    CHECK(ours != std::string::npos && theirs != std::string::npos && ours < theirs);
    CHECK(tls.find("phone_camera::kTrackerModule);", ours) < theirs);
    CHECK(tls.find("#include \"phone_camera_tracker.hpp\"") != std::string::npos);
    const size_t stages_log = tls.find("if (const auto line = phone_latency::Line(body)) XELOGW(\"{}\", *line);");
    const size_t reasons_log = tls.find("if (const auto reasons = phone_latency::ReasonsLine(body)) XELOGW(\"{}\", *reasons);");
    CHECK(stages_log != std::string::npos && reasons_log != std::string::npos && stages_log < reasons_log &&
          reasons_log - stages_log < 200);
    std::printf("tree: the title's tracker served first, the reasons line logged\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kinect-tracker: all passed\n");
  return 0;
}
