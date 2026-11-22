// SPDX-License-Identifier: MIT
// Fix 118 (Kinect Sports, issue 15): the Kinect system windows with the virtual Kinect.
//
//   check-nui-hud-check     run from an assembled tree's root
//
//  (a) xbox360ps5/nui_hud.hpp: the NUI HUD check lets a window through with the console's Kinect
//      flag (as before) or with the virtual Kinect on for a Kinect title (the flag missing, as on
//      this console), and refuses it without either, or without the NUI library's state;
//  (b) the tree: NuiHudCheck answers through it, the virtual Kinect read only when the flag is
//      missing, for titles that require Kinect, logged (at most 8 times); XamShowNuiDeviceSelectorUI
//      (Kinect Sports' "where to save" for a signed-in player) still asks the check and then goes
//      to the ordinary XamShowDeviceSelectorUI (the save drive from the settings or the window),
//      with its arguments in order (tracking id first, then the device selector's own).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xbox360ps5/nui_hud.hpp"

namespace m = xbox360ps5::motion;

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
  std::string text = s.str();
  for (size_t at; (at = text.find('\r')) != std::string::npos;) text.erase(at, 1);
  return text;
}

int main() {
  // (a) Every combination.
  for (const bool data : {false, true})
    for (const bool flag : {false, true})
      for (const bool virtual_kinect : {false, true})
        CHECK(m::NuiHudAllowed(data, flag, virtual_kinect) == (data && (flag || virtual_kinect)));
  CHECK(m::NuiHudAllowed(true, false, true));    // this console with the virtual Kinect: the fix
  CHECK(!m::NuiHudAllowed(true, false, false));  // the motion option off: refused as before
  CHECK(m::NuiHudAllowed(true, true, false));    // a console with the flag: as before

  // (b) The tree.
  const std::string nui = ReadText(".deps/xenia-canary/src/xenia/kernel/nui.cc");
  const std::string ui = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_ui.cc");
  CHECK(!nui.empty() && !ui.empty());
  CHECK(nui.find("#include \"xbox360ps5/nui_hud.hpp\"") != std::string::npos);
  CHECK(nui.find("#include \"xenia/kernel/kinect_title.h\"") != std::string::npos);
  const size_t check = nui.find("uint32_t NUIController::NuiHudCheck(uint64_t tracking_id) {");
  CHECK(check != std::string::npos);
  const std::string body = nui.substr(check, nui.find("\n}", check) - check);
  CHECK(body.find("const bool kinect_initialized = NuiInitialized();") != std::string::npos);
  CHECK(body.find("!kinect_initialized && xbox360ps5::motion::enabled.load() &&\n"
                  "      KinectRequiredTitle(KernelState::shared());") != std::string::npos);
  CHECK(body.find("if (!xbox360ps5::motion::NuiHudAllowed(GetNUIDataPtr() && GetUnknown2(),\n"
                  "                                         kinect_initialized, virtual_kinect)) {\n"
                  "    return X_ERROR_ACCESS_DENIED;") != std::string::npos);
  CHECK(body.find("if (virtual_kinect && shown.fetch_add(1) < 8) {") != std::string::npos);
  CHECK(body.find("allowed with the virtual Kinect (fix 118)") != std::string::npos);
  CHECK(body.find("!NuiInitialized()") == std::string::npos);  // the old refusal is gone
  CHECK(body.find("SetEngagedTrackingId(tracking_id);") != std::string::npos);

  const size_t nui_selector = ui.find("dword_result_t XamShowNuiDeviceSelectorUI_entry(");
  CHECK(nui_selector != std::string::npos);
  const std::string selector = ui.substr(nui_selector, ui.find("DECLARE_XAM_EXPORT1", nui_selector) - nui_selector);
  CHECK(selector.find("dword_t tracking_id, dword_t user_index, dword_t content_type,\n"
                      "    dword_t content_flags, qword_t total_requested, lpdword_t device_id_ptr,\n"
                      "    pointer_t<XAM_OVERLAPPED> overlapped) {") != std::string::npos);
  const size_t hud = selector.find("kernel_state()->nui()->NuiHudCheck(tracking_id);");
  const size_t ordinary = selector.find("return XamShowDeviceSelectorUI_entry(user_index, content_type, content_flags,\n"
                                        "                                       total_requested, device_id_ptr,\n"
                                        "                                       overlapped);");
  CHECK(hud != std::string::npos && ordinary != std::string::npos && hud < ordinary);
  CHECK(ui.find("DECLARE_XAM_EXPORT1(XamShowNuiDeviceSelectorUI, kUI, kSketchy);") != std::string::npos);

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-nui-hud-check: ok\n");
  return 0;
}
