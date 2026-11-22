// SPDX-License-Identifier: MIT
// Fix 125 (xenia/gpu/content_check_policy.h, shared_memory.cc): pinned units the guest keeps
// rewriting are compared by content at each part of the ring instead of being protected after
// every upload. The Sims 3 in live mode with Optimized video memory (1.0.5-work-fix121,
// research/sims3-121boost): 8,506 protections a second on the GPU thread (321 ms/s) and 6,277
// write faults a second (236 ms/s), mostly texture pages rewritten every frame.
//  (a) the policy: when a pinned unit starts being checked (heat, GPU-written data, the cap),
//      when it goes back to being watched;
//  (b) a model of one 4 KiB texture unit over 600 frames, written every frame for 300 and then
//      left alone: protection changes and faults before, hashes and changes found after; every
//      change reaches the GPU copy before the next part of the ring reads it, as before; after
//      the writes stop it is watched again;
//  (c) the assembled tree: the setting, the heat of pinned units on a fault, no watch for
//      checked units in MakeRangeValid but their hash before the copy, GPU-written data never
//      checked, the check at each part of the ring (invalidate and fire the watches), aging back
//      to watched, the counters' line.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "xenia/gpu/content_check_policy.h"

namespace {

namespace cc = xe::gpu::content_check;
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

void CheckPolicy() {
  Expect(!cc::StartChecking(false, false, cc::kHeatToCheck - 1, 0), "below the heat: still watched");
  Expect(cc::StartChecking(false, false, cc::kHeatToCheck, 0), "at the heat: checked by content");
  Expect(!cc::StartChecking(true, false, 255, 0), "GPU-written data is never checked");
  Expect(!cc::StartChecking(false, true, 255, 0), "an already checked unit is not started again");
  Expect(!cc::StartChecking(false, false, 255, cc::kMaxCheckedUnits), "the cap holds");
  Expect(cc::StopChecking(1) && !cc::StopChecking(2), "cooled below 2: watched again");
}

// The model: frames of 4 parts of the ring each; the guest writes the unit before the frame's
// second part when `written(frame)`; every part's draws read it.
struct Costs {
  int protections = 0, faults = 0, hashes = 0, changes_found = 0, stale_reads = 0;
  bool checked_at_end = false;
};
template <class Written>
Costs Model(bool content_checks, int frames, Written written) {
  Costs c;
  uint8_t heat = 0;
  bool checked = false, valid = false, protected_now = false;
  uint32_t guest = 0, gpu = 0, hashed = 0;  // the bytes' "version"
  for (int frame = 0; frame < frames; ++frame) {
    if (frame && frame % 120 == 0) {
      heat >>= 1;  // AgeUnwatchedPages
      if (checked && cc::StopChecking(heat)) {
        checked = false;
        valid = false;  // invalidated once, watched from its next upload
      }
    }
    for (int part = 0; part < 4; ++part) {
      if (part == 1 && written(frame)) {
        ++guest;
        if (protected_now) {  // the write faults: unprotect, invalidate, heat
          ++c.faults;
          ++c.protections;
          protected_now = false;
          valid = false;
          if (heat < 255) ++heat;
          if (content_checks && cc::StartChecking(false, checked, heat, 0)) checked = true;
        }
      }
      if (checked && valid) {  // CheckContentUnits at the part's start
        ++c.hashes;
        if (guest != hashed) {
          ++c.changes_found;
          valid = false;
          heat = cc::HeatOnChange(heat);
        }
      }
      // The part's draws request the unit.
      if (!valid) {
        if (checked) {
          hashed = guest;  // MakeRangeValid: hashed before the copy, no watch
        } else {
          ++c.protections;  // protected before the copy
          protected_now = true;
        }
        gpu = guest;
        valid = true;
      }
      if (gpu != guest) ++c.stale_reads;
    }
  }
  c.checked_at_end = checked;
  return c;
}

void CheckModel() {
  const auto busy_then_idle = [](int frame) { return frame < 300; };
  const Costs before = Model(false, 900, busy_then_idle), after = Model(true, 900, busy_then_idle);
  std::printf("one texture unit written in the first 300 of 900 frames: watched %d protection changes and %d faults; "
              "content-checked %d protection changes, %d faults, %d hashes, %d changes found\n",
              before.protections, before.faults, after.protections, after.faults, after.hashes,
              after.changes_found);
  Expect(before.faults == 300 && before.protections == 601, "watched: a fault and two protection changes per write");
  Expect(after.faults == cc::kHeatToCheck, "checked: faults only until it heats up");
  Expect(after.changes_found >= 300 - cc::kHeatToCheck - 1, "checked: every later write is found by its hash");
  Expect(after.protections < 20, "checked: hardly any protection changes");
  Expect(before.stale_reads == 0 && after.stale_reads == 0,
         "no part of the ring reads bytes older than the guest wrote before it");
  Expect(!after.checked_at_end, "after the writes stop it is watched again");
  Expect(cc::HeatOnChange(cc::kHeatWhileChecked) == cc::kHeatWhileChecked && cc::HeatOnChange(0) == 1,
         "a change warms a checked unit up to kHeatWhileChecked");
  const Costs rare = Model(true, 600, [](int frame) { return frame % 100 == 0; });
  Expect(rare.faults == 6 && rare.hashes == 0, "a unit written now and then stays watched");
}

void CheckTree() {
  const std::string gpu = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string sm = Read(gpu + "shared_memory.cc"), smh = Read(gpu + "shared_memory.h");
  Expect(sm.find("DEFINE_bool(gpu_content_checked_units, true,") != std::string::npos &&
             sm.find("return UnwatchedPagesEnabled() && cvars::gpu_content_checked_units;") != std::string::npos,
         "the setting, on with the unwatched units");
  const size_t note = sm.find("void SharedMemory::NoteCpuWrite(");
  const size_t note_end = sm.find("void SharedMemory::ForgetUnwatched(", note);
  const std::string note_body = note != std::string::npos && note_end != std::string::npos ? sm.substr(note, note_end - note) : "";
  Expect(note_body.find("if (UnitBit(pinned_bits_, unit)) {") < note_body.find("content_check::StartChecking(") &&
             note_body.find("system_page_flags_valid_and_gpu_written_") != std::string::npos,
         "a fault heats a pinned unit and may start its check, not for GPU-written data");
  const size_t forget = sm.find("void SharedMemory::ForgetUnwatched(");
  Expect(forget != std::string::npos && sm.find("ClearUnitBit(checked_bits_, unit);  // fix 125", forget) <
                                            sm.find("void SharedMemory::PinWatched(", forget),
         "the guest's own protection change forgets the check");
  const size_t make = sm.find("void SharedMemory::MakeRangeValid(");
  const size_t make_end = sm.find("void SharedMemory::UnlinkWatchRange(", make);
  const std::string make_body = make != std::string::npos && make_end != std::string::npos ? sm.substr(make, make_end - make) : "";
  Expect(make_body.find("if (written_by_gpu) {") != std::string::npos &&
             make_body.find("ClearUnitBit(checked_bits_, unit);") != std::string::npos,
         "GPU-written data is watched as before");
  Expect(make_body.find("!UnitBit(checked_bits_, unit);") != std::string::npos &&
             make_body.find("unit_hash_[unit] = XXH3_64bits(") != std::string::npos,
         "a checked unit gets no watch and its hash at the upload");
  const size_t check = sm.find("void SharedMemory::CheckContentUnits() {");
  const std::string check_body = check != std::string::npos ? sm.substr(check, 3500) : "";
  Expect(check_body.find("if (hash != unit_hash_[unit]) {") != std::string::npos &&
             check_body.find("FireWatches(unit, unit, false);") != std::string::npos &&
             check_body.find("request_cache_.Invalidate();") != std::string::npos,
         "a changed unit is invalidated and its watches fire");
  Expect(sm.find("content_check::StopChecking(heat)") != std::string::npos, "aging takes it back to watched");
  Expect(sm.find("Performance content-checked units (fix 125, {})") != std::string::npos, "the counters' line");
  Expect(smh.find("void CheckContentUnits();") != std::string::npos, "the check is declared");
  const std::string pm4 = Read(gpu + "pm4_command_processor_implement.h");
  const size_t epoch = pm4.find("++xbox360ps5::frame_cost::command_chunk_epoch;");
  const size_t hook = pm4.find("shared_memory_->CheckContentUnits();");
  Expect(epoch != std::string::npos && hook != std::string::npos && hook > epoch && hook - epoch < 300,
         "the check runs at the start of each part of the ring");
}

}  // namespace

int main(int argc, char** argv) {
  CheckPolicy();
  CheckModel();
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-content-checked: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-content-checked: ok\n");
  return 0;
}
