// SPDX-License-Identifier: MIT
// Fix 72 (tools/source-1.0.2/fixes/72-phone-page-version.patch): the console's camera server
// gives its page's version to the page and with every status answer, so an open page of an older
// build reloads once (tools/check-page-version.mjs checks the page's side). The assembled
// port/phone_camera_tls.cpp: the served page has __PAGE_VERSION__ replaced by kPageVersion, and
// GET /api/motion/status answers with an X-Page-Version header carrying it.
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
  return text.str();
}

void CheckTree() {
  const std::string code = Read("port/phone_camera_tls.cpp");
  const size_t page = code.find("std::string page = phone_camera::kCameraPage;");
  const size_t placeholder = code.find("page.find(\"__PAGE_VERSION__\")", page);
  const size_t replaced = code.find("page.replace(at, sizeof(\"__PAGE_VERSION__\") - 1, phone_camera::kPageVersion);", placeholder);
  const size_t served = code.find("return Response(\"200 OK\", \"text/html; charset=utf-8\", page);", page);
  Expect(page != std::string::npos && placeholder != std::string::npos && replaced != std::string::npos && replaced < served,
         "the served page carries its version");
  const size_t status = code.find("if (method == \"GET\" && target == \"/api/motion/status\")  // fix 72");
  const size_t header = code.find("(std::string(\"X-Page-Version: \") + phone_camera::kPageVersion + \"\\r\\n\").c_str());", status);
  Expect(status != std::string::npos && header != std::string::npos && header - status < 300,
         "every status answer names the page's version");
  Expect(Read("port/phone_camera_page.hpp").find("inline constexpr char kPageVersion[] = \"") != std::string::npos,
         "the header declares it");
}

}  // namespace

int main() {
  CheckTree();
  if (failures) {
    std::printf("check-page-version: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-page-version: ok\n");
  return 0;
}
