// SPDX-License-Identifier: MIT
// Fix 142e (research/parallel-gpu-commands/STAGE2.md, section 13): RenderTargetCache::Update is
// skipped for a clipped draw whose key matches the last Update's, when nothing changed the
// cache's state since and that Update changed nothing (xenia/gpu/render_target_skip.h).
//  (a) render-target-skip.txt, the key, the outputs and the memo;
//  (b) a model cache (EDRAM ranges with owners, render targets created on first use, layouts,
//      accumulated bindings) with resolves, clears and trims between the draws: in mode 1 every
//      predicted Update changes nothing, and mode 2 leaves the state and every draw's bindings
//      exactly as mode 0 does;
//  (c) the line's arithmetic and text;
//  (d) the assembled tree: the key holds every register Update, EstimateMaxY (clipped) and
//      GetScissor read; Update's calls and settings are the reviewed ones; every function that
//      touches the state Update reads bumps the version or is listed as only reading it; what
//      Update writes is in the outputs compared; the memo is wired in Update and the line at
//      the end of a submission;
//  (e) the patch only adds lines (argv[1]: the fixes folder).
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xenia/gpu/render_target_skip.h"

namespace {

namespace rts = xe::gpu::render_target_skip;
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

// (a)
void CheckParse() {
  uint32_t mode = 7;
  Expect(rts::ParseMode("1\n", mode) && mode == 1, "\"1\" is verify");
  Expect(rts::ParseMode("# skip it\n  2  # yes\n", mode) && mode == 2, "comments and spaces");
  Expect(rts::ParseMode("0", mode) && mode == 0, "\"0\" is off");
  mode = 1;
  Expect(!rts::ParseMode("3\n", mode) && mode == 1, "3 is not a mode, the mode kept");
  Expect(!rts::ParseMode("1 2\n", mode) && mode == 1, "two numbers are not understood");
  Expect(!rts::ParseMode("1\n2\n", mode) && mode == 1, "two lines are not understood");
  Expect(!rts::ParseMode("on\n", mode) && mode == 1, "a word is not understood");
  Expect(!rts::ParseMode("12\n", mode) && mode == 1, "12 is not a mode");
  Expect(!rts::ParseMode("", mode) && mode == 1, "an empty file keeps the settings' mode");
  Expect(!rts::ParseMode("# nothing\n", mode) && mode == 1, "only a comment");
}

void CheckKey() {
  rts::Key a, b;
  for (size_t i = 0; i < rts::kRegisterCount; ++i) a.registers[i] = b.registers[i] = uint32_t(i);
  a.Add(uint32_t(1));
  a.Add(int32_t(-3));
  a.Add(uint64_t(5) << 40);
  b.Add(uint32_t(1));
  b.Add(int32_t(-3));
  b.Add(uint64_t(5) << 40);
  Expect(a == b && a.FirstDifference(b) == -1, "equal keys");
  rts::Key c = b;
  c.registers[3] ^= 1;
  Expect(!(a == c) && a.FirstDifference(c) == 3, "a register differs");
  rts::Key d = b;
  d.words[3] ^= 1;  // the high half of int32_t(-3)
  Expect(!(a == d) && a.FirstDifference(d) == int(rts::kRegisterCount), "a parameter differs");
  rts::Key e;
  rts::Key f;
  e.Add(true);
  f.Add(uint32_t(1));
  Expect(e == f, "the words, not the types");
  rts::Key g, h;
  for (int i = 0; i < 20; ++i) {
    g.Add(uint32_t(i));
    h.Add(uint32_t(i));
  }
  Expect(!(g == h) && !(g == g), "a key past its words never matches (no skip)");
  rts::Key fits;
  for (int i = 0; i < 8; ++i) fits.Add(uint32_t(i));
  Expect(fits == fits, "eight words of parameters fit (the tree's key adds seven)");
}

void CheckOutputs() {
  rts::Outputs a, b;
  int x = 0, y = 0;
  a.Add(&x);
  a.Add(uint32_t(7));
  a.Add(size_t(0));
  b.Add(&x);
  b.Add(uint32_t(7));
  b.Add(size_t(0));
  Expect(a == b && a.FirstDifference(b) == -1, "equal outputs");
  rts::Outputs c;
  c.Add(&y);
  c.Add(uint32_t(7));
  c.Add(size_t(0));
  Expect(!(a == c) && a.FirstDifference(c) == 0, "another render target");
  rts::Outputs d;
  d.Add(&x);
  d.Add(uint32_t(7));
  d.Add(size_t(1));
  Expect(a.FirstDifference(d) == 2, "a transfer");
  rts::Outputs big1, big2;
  for (int i = 0; i < 40; ++i) {
    big1.Add(uint32_t(i));
    big2.Add(uint32_t(i));
  }
  Expect(!(big1 == big2) && big1.FirstDifference(big2) == 32,
         "outputs past the words never match (the Update counts as a change)");
}

void CheckMemo() {
  rts::Memo memo;
  rts::Key key;
  key.Add(uint32_t(9));
  Expect(!memo.Predict(key, 0), "nothing predicted before an Update");
  memo.Record(key, true, true, 4);
  Expect(!memo.Predict(key, 4), "an Update that changed something is not a memo");
  memo.Record(key, true, false, 4);
  Expect(memo.Predict(key, 4), "an Update that changed nothing, same key and version");
  Expect(!memo.Predict(key, 5), "the state changed since");
  rts::Key other;
  other.Add(uint32_t(10));
  Expect(!memo.Predict(other, 4), "another key");
  memo.Record(key, false, false, 4);
  Expect(!memo.Predict(key, 4), "a failed Update is not a memo");
  memo.Record(key, true, false, 4);
  memo.Invalidate();
  Expect(!memo.Predict(key, 4), "invalidated (another path, an unclipped draw, a mode change)");
  bool ran = false;
  {
    const rts::AtExit at_exit([&]() { ran = true; });
    Expect(!ran, "not before the scope ends");
  }
  Expect(ran, "at the end of the scope");
}

// (b) A model of the cache: EDRAM tiles with owners, render targets created on first use with
// layouts, the accumulated bindings and the transfers of the last Update. Every change of the
// state bumps the version, as Fix142eTouch does in the tree.
struct ModelDraw {
  uint32_t base, length, target, layout_wanted;
  bool clipped;
};
struct Model {
  static constexpr uint32_t kTiles = 16;
  uint32_t owner[kTiles] = {};
  std::set<uint32_t> created;
  std::map<uint32_t, uint32_t> layout;
  uint32_t accumulated = 0;
  bool accumulated_valid = false;
  uint32_t transfers = 0;
  uint64_t version = 0;
  void Touch() { ++version; }
  // RenderTargetCache::Update with the Vulkan part: claim the range (a transfer for every tile
  // owned by another target), create the target, transition it, accumulate the binding.
  void Update(const ModelDraw& draw) {
    transfers = 0;
    for (uint32_t i = 0; i < draw.length; ++i) {
      uint32_t& tile = owner[(draw.base + i) % kTiles];
      if (tile != draw.target) {
        Touch();
        if (tile) ++transfers;
        tile = draw.target;
      }
    }
    if (!created.count(draw.target)) {
      Touch();
      created.insert(draw.target);
      layout[draw.target] = 0;
    }
    if (layout[draw.target] != draw.layout_wanted) {
      Touch();
      layout[draw.target] = draw.layout_wanted;
    }
    if (!accumulated_valid || accumulated != draw.target) {
      accumulated = draw.target;
      accumulated_valid = true;
    }
  }
  void Outputs(rts::Outputs& outputs) const {
    outputs.Add(accumulated);
    outputs.Add(accumulated_valid);
    outputs.Add(transfers);
  }
  // Between draws: a resolve (copies out of a target), a cleared cache, a new frame.
  void Resolve(uint32_t target) {
    Touch();
    layout[target] = 9;
  }
  void Clear() {
    Touch();
    for (uint32_t& tile : owner) tile = 0;
    created.clear();
    layout.clear();
    accumulated_valid = false;
  }
  void NewFrame() {
    Touch();
    accumulated_valid = false;
  }
  bool operator==(const Model& o) const {
    return std::equal(owner, owner + kTiles, o.owner) && created == o.created &&
           layout == o.layout && accumulated == o.accumulated &&
           accumulated_valid == o.accumulated_valid && transfers == o.transfers;
  }
};

// The tree's VulkanRenderTargetCache::Update with the memo around the model's Update.
struct ModelRun {
  Model model;
  rts::Memo memo;
  rts::Stats stats;
  uint32_t mode;
  explicit ModelRun(uint32_t m) : mode(m) {}
  void Draw(const ModelDraw& draw) {
    rts::Key key;
    key.Add(draw.base);
    key.Add(draw.length);
    key.Add(draw.target);
    key.Add(draw.layout_wanted);
    bool eligible = false, predicted = false;
    rts::Outputs before;
    const uint64_t version_before = model.version;
    if (mode != rts::kOff) {
      ++stats.updates;
      if (!draw.clipped) {
        ++stats.unclipped;
      } else {
        eligible = true;
        ++stats.eligible;
        predicted = memo.Predict(key, model.version);
        if (predicted) {
          ++stats.predicted;
          if (mode == rts::kSkip) {
            ++stats.skipped;
            return;
          }
        }
        model.Outputs(before);
      }
    }
    const rts::AtExit finish([&]() {
      if (!eligible) {
        memo.Invalidate();
        return;
      }
      rts::Outputs after;
      model.Outputs(after);
      const bool changed = model.version != version_before || !(after == before);
      stats.changed += uint64_t(changed);
      memo.Record(key, true, changed, model.version);
      if (predicted && changed) ++stats.mismatches[rts::kMismatchState];
    });
    model.Update(draw);
  }
};

void CheckModel() {
  for (uint32_t seed = 1; seed <= 40; ++seed) {
    std::mt19937 rng(seed);
    auto pick = [&](uint32_t n) { return uint32_t(rng() % n); };
    ModelRun off(rts::kOff), verify(rts::kVerify), skip(rts::kSkip);
    // A few draw setups repeated in runs, as a game's passes do.
    std::vector<ModelDraw> setups;
    for (int i = 0; i < 6; ++i) {
      setups.push_back({pick(Model::kTiles), 1 + pick(8), 1 + pick(4), pick(2), pick(5) != 0});
    }
    size_t setup = 0;
    for (int step = 0; step < 4000; ++step) {
      const uint32_t roll = pick(100);
      if (roll < 2) {
        const uint32_t target = 1 + pick(4);
        for (ModelRun* run : {&off, &verify, &skip}) run->model.Resolve(target);
      } else if (roll < 3) {
        for (ModelRun* run : {&off, &verify, &skip}) run->model.Clear();
      } else if (roll < 4) {
        for (ModelRun* run : {&off, &verify, &skip}) run->model.NewFrame();
      } else {
        if (pick(8) == 0) setup = pick(uint32_t(setups.size()));
        for (ModelRun* run : {&off, &verify, &skip}) run->Draw(setups[setup]);
        Expect(skip.model == off.model, "mode 2 leaves what mode 0 leaves, seed " +
                                            std::to_string(seed) + " step " +
                                            std::to_string(step));
        if (!(skip.model == off.model)) return;
      }
    }
    Expect(verify.model == off.model, "mode 1 changes nothing");
    Expect(verify.stats.MismatchTotal() == 0, "no mismatch in mode 1, seed " + std::to_string(seed));
    Expect(verify.stats.predicted > 1000, "most repeated draws are predicted, seed " +
                                              std::to_string(seed) + ": " +
                                              std::to_string(verify.stats.predicted));
    Expect(skip.stats.skipped == skip.stats.predicted && skip.stats.skipped > 1000,
           "mode 2 skips what it predicts");
    Expect(verify.stats.updates == verify.stats.eligible + verify.stats.unclipped,
           "every update clipped or not");
  }
  // A change that does not bump the version is what mode 1 is there to catch.
  ModelRun verify(rts::kVerify);
  const ModelDraw draw{0, 4, 1, 1, true};
  verify.Draw(draw);
  verify.Draw(draw);
  verify.model.layout[1] = 5;  // a layout changed behind the cache's back
  verify.Draw(draw);
  Expect(verify.stats.predicted == 1 && verify.stats.MismatchTotal() == 1,
         "a change without a bump shows as a mismatch in mode 1");
}

// (c)
void CheckLine() {
  rts::Stats before, now;
  now.updates = 2000;
  now.eligible = 1800;
  now.unclipped = 200;
  now.predicted = 1500;
  now.changed = 300;
  now.mismatches[rts::kMismatchOutputs] = 2;
  before.mismatches[rts::kMismatchState] = 1;
  now.mismatches[rts::kMismatchState] = 1;
  const std::string line = rts::Line(rts::kVerify, now, before, 2.0);
  const std::string want =
      "Render target skip (fix 142e): mode 1 (verify), per second 1000 updates, 900 clipped, "
      "100 unclipped, 750 could skip (75%), 0 skipped, 150 changed; mismatches 2 (state 0, "
      "outputs 2, failed 0), 3 in all";
  Expect(line == want, "the line: " + line);
  rts::Stats skipping;
  skipping.updates = 100;
  skipping.eligible = 100;
  skipping.predicted = 90;
  skipping.skipped = 90;
  const std::string line2 = rts::Line(rts::kSkip, skipping, rts::Stats(), 1.0);
  Expect(line2.find("mode 2 (skip)") != std::string::npos &&
             line2.find("90 could skip (90%), 90 skipped") != std::string::npos,
         "the line in mode 2: " + line2);
}

// (d)
std::string Body(const std::string& text, const std::string& signature) {
  const size_t at = text.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t open = text.find('{', at);
  int depth = 0;
  for (size_t i = open; i < text.size(); ++i) {
    if (text[i] == '{') ++depth;
    if (text[i] == '}' && --depth == 0) return text.substr(at, i + 1 - at);
  }
  return std::string();
}
std::set<std::string> Registers(const std::string& text) {
  std::set<std::string> out;
  static const std::regex reg(R"(\b(?:reg::|XE_GPU_REG_)([A-Z][A-Z0-9_]+))");
  for (std::sregex_iterator it(text.begin(), text.end(), reg), end; it != end; ++it) {
    const std::string name = (*it)[1];
    out.insert(name);
    if (name == "RB_COLOR_INFO") {  // rt_register_indices: the four render targets
      out.insert("RB_COLOR1_INFO");
      out.insert("RB_COLOR2_INFO");
      out.insert("RB_COLOR3_INFO");
    }
  }
  return out;
}
// The functions defined in a .cc file of the class, each with its text up to the next one.
std::map<std::string, std::string> Functions(const std::string& text, const std::string& cls) {
  std::map<std::string, std::string> out;
  static const std::regex def(
      R"(\n(?:[A-Za-z][^\n;(]*?[ *&])?(?:Vulkan)?RenderTargetCache::([A-Za-z0-9_]+)\()");
  std::vector<std::pair<size_t, std::string>> starts;
  for (std::sregex_iterator it(text.begin(), text.end(), def), end; it != end; ++it) {
    const std::string whole = (*it)[0];
    const size_t class_at = whole.rfind(cls + "::");
    if (class_at == std::string::npos ||
        (class_at > 0 && std::isalnum(static_cast<unsigned char>(whole[class_at - 1])))) {
      continue;
    }
    starts.emplace_back(size_t(it->position()), std::string((*it)[1]));
  }
  for (size_t i = 0; i < starts.size(); ++i) {
    const size_t end = i + 1 < starts.size() ? starts[i + 1].first : text.size();
    out[starts[i].second] += text.substr(starts[i].first, end - starts[i].first);
  }
  return out;
}

void CheckTree() {
  const std::string gpu = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string base = Read(gpu + "render_target_cache.cc");
  const std::string base_h = Read(gpu + "render_target_cache.h");
  const std::string vk = Read(gpu + "vulkan/vulkan_render_target_cache.cc");
  const std::string vk_h = Read(gpu + "vulkan/vulkan_render_target_cache.h");
  const std::string cp = Read(gpu + "vulkan/vulkan_command_processor.cc");
  const std::string estimator = Read(gpu + "draw_extent_estimator.cc");
  const std::string draw_util = Read(gpu + "draw_util.cc");
  const std::string skip_h = Read(gpu + "render_target_skip.h");
  Expect(!base.empty() && !vk.empty() && !cp.empty() && !estimator.empty() &&
             !draw_util.empty() && !skip_h.empty(),
         "the tree's files are there (run from the assembled tree's root)");

  // The registers.
  const std::string update = Body(base, "bool RenderTargetCache::Update(");
  const std::string vk_update = Body(vk, "bool VulkanRenderTargetCache::Update(");
  const std::string estimate = Body(estimator, "uint32_t DrawExtentEstimator::EstimateMaxY(");
  const std::string scissor = Body(draw_util, "static inline void GetScissorTmpl(");
  Expect(!update.empty() && !vk_update.empty() && !estimate.empty() && !scissor.empty(),
         "Update, the Vulkan Update, EstimateMaxY and GetScissorTmpl found");
  std::set<std::string> read;
  for (const std::string* text : {&update, &vk_update, &estimate, &scissor}) {
    const std::set<std::string> some = Registers(*text);
    read.insert(some.begin(), some.end());
  }
  read.erase("PA_CL_CLIP_CNTL");  // read again below; listed too
  read.insert("PA_CL_CLIP_CNTL");
  std::set<std::string> listed;
  {
    const std::string list = Body(skip_h, "constexpr Register kRegisters[] =");
    for (const std::string& name : Registers(list)) listed.insert(name);
    Expect(listed.size() == rts::kRegisterCount, "the list's names are its registers");
  }
  for (const std::string& name : read) {
    Expect(listed.count(name) != 0, "the key holds " + name + ", which Update reads");
  }
  // A clipped draw's height comes from the registers: the vertices only where clip_disable.
  const size_t clip = estimate.find("PA_CL_CLIP_CNTL>().clip_disable");
  const size_t vertices = estimate.find("EstimateVertexMaxY(");
  const size_t viewport = estimate.find("// Viewport.");
  Expect(clip != std::string::npos && vertices > clip && viewport > vertices,
         "EstimateMaxY runs the vertices only for an unclipped draw");
  Expect(vk_update.find("Get<reg::PA_CL_CLIP_CNTL>().clip_disable") != std::string::npos,
         "the memo only for clipped draws");

  // Update's calls and settings, reviewed (a new one may read something the key does not hold).
  const std::set<std::string> reviewed_calls = {
      "AddWindowOffsetToEdramBase", "ChangeOwnership", "EstimateMaxY", "GetColorResourceFormat",
      "GetMaxRenderTargetWidth", "GetOrCreateRenderTarget", "GetPSIColorFormatInfo", "GetPath",
      "GetRenderTargetHeight", "GetScissor", "GetStorageColorFormat", "IsHostDepthCurrent",
      "IsScaleNativeForPitch", "RequestPixelShaderInterlockBarrier",
      "WouldOwnershipChangeRequireTransfers", "XELOGE", "TODO", "Update",
      "IsColorRenderTargetFormat64bpp"};
  static const std::regex call(R"(\b([A-Z][A-Za-z0-9]+)\()");
  const std::string update_inner = update.substr(update.find('{'));
  for (std::sregex_iterator it(update_inner.begin(), update_inner.end(), call), end; it != end;
       ++it) {
    Expect(reviewed_calls.count((*it)[1]) != 0,
           "Update calls " + std::string((*it)[1]) + ": review what it reads (fix 142e key)");
  }
  static const std::regex cvar(R"(cvars::([a-z0-9_]+))");
  const std::string key_for = Body(base, "void RenderTargetCache::Fix142eKeyFor(");
  Expect(!key_for.empty(), "Fix142eKeyFor found");
  const std::string scale_native = Body(base, "bool RenderTargetCache::IsScaleNativeForPitch(");
  for (const std::string* text : {&update, &scale_native}) {
    for (std::sregex_iterator it(text->begin(), text->end(), cvar), end; it != end; ++it) {
      const std::string name = (*it)[1];
      if (name == "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend") continue;  // FSI
      Expect(key_for.find("cvars::" + name) != std::string::npos,
             "the key holds the setting " + name + ", which Update reads");
    }
  }
  Expect(key_for.find("kRegisters[i]") != std::string::npos &&
             key_for.find("normalized_depth_control.value") != std::string::npos &&
             key_for.find("normalized_color_mask") != std::string::npos &&
             key_for.find("window_offset_tiles") != std::string::npos &&
             key_for.find("is_rasterization_done") != std::string::npos &&
             key_for.find("draw_resolution_scale_x()") != std::string::npos &&
             key_for.find("draw_resolution_scale_y()") != std::string::npos &&
             key_for.find("trim_frame_") != std::string::npos,
         "the key holds the registers, the parameters, the scale and the frame");

  // Who changes the state Update reads.
  const std::vector<std::string> tracked = {
      "ownership_ranges_", "render_targets_", "are_accumulated_render_targets_valid_",
      "last_update_", "kept_native_", "trim_frame_", "SetUsage(", "render_passes_",
      "framebuffers_", "ResetAccumulatedRenderTargets(", "KeepScaleNativeForPitch(",
      "ChangeOwnership(", "TransitionRenderTarget(", "CreateRenderTarget("};
  // Bump the version (Fix142eTouch), on entry or where they change something.
  const std::set<std::string> bump = {
      "InitializeCommon", "DestroyAllRenderTargets", "ClearCache", "BeginFrame",
      "TrimUnusedRenderTargets", "PrepareHostRenderTargetsResolveClear",
      "PixelShaderInterlockFullEdramBarrierPlaced", "GetOrCreateRenderTarget", "ChangeOwnership",
      "PrepareFullEdram1280xRenderTargetForSnapshotRestoration",
      "Initialize", "Shutdown", "TrimUnusedRenderTargetsVulkan", "Resolve", "DumpRenderTargets",
      "TransitionRenderTarget", "PerformTransfersAndResolveClears"};
  // Only read it, or change only caches looked up by the same key, or are the memo's own.
  const std::set<std::string> reads = {
      "GetLastUpdateBoundRenderTargets",   // const
      "WouldOwnershipChangeRequireTransfers",  // const
      "IsHostDepthCurrent",                // const
      "GetResolveCopyRectanglesToDump",    // const
      "IsScaleNativeForPitch",             // const, kept_native_ bumps where it is set
      "GetHostRenderTargetsRenderPass",    // a render pass per key, the same for the same key
      "GetHostRenderTargetsFramebuffer",   // a framebuffer per key; Update's choice is an output
      "CreateRenderTarget",                // only from GetOrCreateRenderTarget, which bumps
      "Fix142eKeyFor", "Fix142eAddVulkanOutputs", "Fix142eEndSubmission"};
  const std::set<std::string> updates = {"Update"};  // the memo compares what they leave
  std::set<std::string> seen;
  for (const auto* file : {&base, &vk}) {
    for (const auto& [name, text] :
         Functions(*file, file == &vk ? "VulkanRenderTargetCache" : "RenderTargetCache")) {
      seen.insert(name);
      bool mentions = false;
      for (const std::string& token : tracked) mentions |= text.find(token) != std::string::npos;
      if (!mentions && !bump.count(name)) continue;
      if (updates.count(name)) continue;
      if (bump.count(name)) {
        Expect(text.find("Fix142eTouch(\"") != std::string::npos,
               name + " bumps the state version (fix 142e)");
      } else {
        Expect(reads.count(name) != 0,
               name + " touches the state RenderTargetCache::Update reads: make it bump the "
                      "version (Fix142eTouch) or list it as only reading (fix 142e)");
      }
    }
  }
  for (const std::set<std::string>* names : {&bump, &reads}) {
    for (const std::string& name : *names) {
      Expect(seen.count(name) != 0, name + " is defined (renamed? check fix 142e's lists)");
    }
  }
  // The header's inline functions that change it, and nothing else there writing it.
  const std::string reset = Body(base_h, "void ResetAccumulatedRenderTargets() {");
  const std::string keep = Body(base_h, "void KeepScaleNativeForPitch(");
  Expect(reset.find("Fix142eTouch(") != std::string::npos &&
             keep.find("Fix142eTouch(") != std::string::npos,
         "ResetAccumulatedRenderTargets and KeepScaleNativeForPitch bump the version");
  for (const std::string* header : {&base_h, &vk_h}) {
    std::istringstream lines(*header);
    std::string line;
    while (std::getline(lines, line)) {
      const size_t first = line.find_first_not_of(' ');
      if (first == std::string::npos) continue;
      const std::string statement = line.substr(first);
      for (const std::string& token : tracked) {
        if (token.back() == '(' || statement.rfind(token, 0) != 0) continue;
        const bool writes = statement.find("= ") != std::string::npos &&
                            statement.find("== ") == std::string::npos;
        if (!writes) continue;
        Expect(statement == "are_accumulated_render_targets_valid_ = false;" ||
                   statement.rfind("kept_native_[pitch_tiles_at_32bpp * 4", 0) == 0,
               "a header writes " + token + ": " + statement + " (fix 142e: bump the version)");
      }
    }
  }
  // Where ChangeOwnership and GetOrCreateRenderTarget bump.
  const std::string change = Body(base, "void RenderTargetCache::ChangeOwnership(");
  const size_t claim = change.find("// Claim the current range.");
  Expect(claim != std::string::npos &&
             change.find("Fix142eTouch(\"ChangeOwnership\")", claim) <
                 change.find("it->second.render_target = dest;", claim),
         "ChangeOwnership bumps before it claims a range");
  const size_t split = change.find("// Different render target overlapping the range");
  Expect(split != std::string::npos &&
             change.find("Fix142eTouch(\"ChangeOwnership\")", split) <
                 change.find("ownership_ranges_.emplace_hint", split),
         "ChangeOwnership bumps before it splits a range");
  const std::string get_or_create =
      Body(base, "RenderTargetCache::RenderTarget* RenderTargetCache::GetOrCreateRenderTarget(");
  const size_t create = get_or_create.find("render_target = CreateRenderTarget(key);");
  Expect(create != std::string::npos &&
             get_or_create.rfind("Fix142eTouch(\"GetOrCreateRenderTarget\")", create) >
                 get_or_create.find("} else {"),
         "GetOrCreateRenderTarget bumps when it creates");
  const std::string transition = Body(vk, "void VulkanRenderTargetCache::TransitionRenderTarget(");
  Expect(transition.find("Fix142eTouch(\"TransitionRenderTarget\")") <
             transition.find("render_target.SetUsage("),
         "TransitionRenderTarget bumps when the layout or the access changes");

  // What Update writes is in the outputs compared.
  const std::string outputs_base = Body(base_h, "void Fix142eAddOutputs(");
  const std::string outputs_vk = Body(vk, "void VulkanRenderTargetCache::Fix142eAddVulkanOutputs(");
  static const std::regex written(
      R"(\b(last_update_[a-z0-9_]+_|are_accumulated_render_targets_valid_)\s*(\[[^\]]*\])?\s*=[^=])");
  for (const std::string* text : {&update, &vk_update}) {
    for (std::sregex_iterator it(text->begin(), text->end(), written), end; it != end; ++it) {
      const std::string member = (*it)[1];
      Expect(outputs_base.find(member) != std::string::npos ||
                 outputs_vk.find(member) != std::string::npos,
             "the outputs compared hold " + member + ", which Update writes");
    }
  }
  for (const std::string member : {"last_update_used_render_targets_",
                                   "last_update_accumulated_render_targets_",
                                   "last_update_transfers_"}) {
    Expect(outputs_base.find(member) != std::string::npos, "the outputs hold " + member);
  }

  // The memo in the Vulkan Update, the line at the end of a submission.
  const size_t memo_start = vk_update.find("fix142e_memo_.Predict(");
  const size_t skipped = vk_update.find("++fix142e_stats_.skipped;");
  const size_t finish = vk_update.find("rts::AtExit fix142e_finish(");
  const size_t common = vk_update.find("!RenderTargetCache::Update(");
  Expect(memo_start != std::string::npos && skipped > memo_start && finish > skipped &&
             common > finish,
         "the Vulkan Update predicts, skips in mode 2, then runs the common Update");
  Expect(vk_update.find("return true;", skipped) < finish,
         "mode 2 returns with the last Update's results before anything runs");
  const size_t succeeded = vk_update.rfind("fix142e_succeeded = true;");
  Expect(succeeded != std::string::npos && vk_update.find("return", succeeded) ==
                                               vk_update.rfind("return true;"),
         "the Update's success is noted right before its last return");
  Expect(vk_update.find("GetPath() == Path::kHostRenderTargets") < memo_start,
         "host render targets only");
  const std::string end_submission = Body(cp, "bool VulkanCommandProcessor::EndSubmission(");
  Expect(end_submission.find("render_target_cache_->Fix142eEndSubmission();") !=
             std::string::npos,
         "the mode and the line at the end of a submission");
  Expect(vk.find("DEFINE_uint32(render_target_skip, 0,") != std::string::npos,
         "render_target_skip, 0 (off) by default");
  Expect(vk.find("/render-target-skip.txt") != std::string::npos, "the file in the title folder");
  const std::string end_skip = Body(vk, "void VulkanRenderTargetCache::Fix142eEndSubmission(");
  Expect(end_skip.find("cvars::render_target_skip < render_target_skip::kModes") !=
                 std::string::npos &&
             end_skip.find(": uint32_t(render_target_skip::kOff);") != std::string::npos,
         "a setting that is not a mode is off");
  Expect(end_skip.find("fix142e_memo_.Invalidate();") != std::string::npos,
         "a mode change clears the memo");
}

// (e)
void CheckPatch(const std::string& fixes) {
  const std::string patch = Read(fixes + "/142e-render-target-skip.patch");
  Expect(!patch.empty(), "the fix's patch is there");
  std::istringstream lines(patch);
  std::string line;
  int removed = 0;
  while (std::getline(lines, line)) {
    if (line.rfind("--- ", 0) == 0) continue;
    if (!line.empty() && line[0] == '-') ++removed;
  }
  Expect(removed == 0, "the patch only adds lines: with the mode at 0 Update runs as before");
}

}  // namespace

int main(int argc, char** argv) {
  CheckParse();
  CheckKey();
  CheckOutputs();
  CheckMemo();
  CheckModel();
  CheckLine();
  const bool model_only = argc > 1 && std::string(argv[1]) == "--model-only";
  if (!model_only) {
    CheckTree();
    if (argc > 1) CheckPatch(argv[1]);
  }
  if (failures) {
    std::printf("check-render-target-skip: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-render-target-skip: ok\n");
  return 0;
}
