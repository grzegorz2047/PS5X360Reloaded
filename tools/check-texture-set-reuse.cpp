// SPDX-License-Identifier: MIT
// Fix 104 (tools/source-1.0.2/fixes/104-texture-set-reuse.patch): a draw's texture descriptor set
// is reused when it would hold the same descriptors. VulkanCommandProcessor::UpdateBindings
// allocated, wrote (vkUpdateDescriptorSets) and bound a new set of textures and samplers for each
// shader stage of every draw ("TODO(Triang3l): Reuse texture and sampler bindings if not
// changed"); bindings are the largest part of a draw's own time (Grand Theft Auto IV 177 ms/s in
// 196,000 draws a second on 1.0.5-work-fix93e, Modern Warfare 2 156 ms/s). Checked:
//  (a) xenia/gpu/vulkan/texture_set_reuse.h, the code the console runs: a set written in this
//      frame and epoch, with the same layout, image views and samplers in order, is reused; any
//      difference (frame, epoch, layout, a view, a sampler, the counts, never written, forgotten)
//      writes a new one. A model of frames of draws (stages sharing textures, a texture destroyed
//      and its handle given to a new view, a sampler destroyed and recreated, a new frame) binds
//      for every draw a set whose descriptors are exactly the draw's, and writes far fewer sets;
//      without the epoch the same model would bind a stale set;
//  (b) include/xbox360ps5/frame_cost.hpp: the sets reused and written, and the summary line;
//  (c) the assembled tree: the texture cache's epoch changes whenever textures are destroyed (for
//      memory, or all of them) and whenever a sampler is destroyed, not when a texture is only
//      outdated (Grand Theft Auto IV has some every frame; their views stay); UpdateBindings
//      fills both stages' image infos, reuses or writes each stage (a reused set rebound only when
//      another was bound), remembers each set it writes, and the pools' clearing forgets them;
//      the summary gives the line after fix 81's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"
#include "xenia/gpu/vulkan/texture_set_reuse.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
namespace tsr = xe::gpu::vulkan::texture_set_reuse;
namespace fc = xbox360ps5::frame_cost;
using Written = tsr::Written<uint32_t, uint64_t, uint64_t, uint32_t>;

void CheckMatches() {
  Written w;
  const uint64_t views[] = {11, 12, 13}, samplers[] = {21, 22};
  Expect(!w.Matches(5, 1, 7, views, 3, samplers, 2), "never written: not reused");
  w.Remember(5, 1, 7, views, 3, samplers, 2, 99);
  Expect(w.Matches(5, 1, 7, views, 3, samplers, 2) && w.set == 99, "the same frame, epoch, layout and handles: reused");
  const uint64_t other_view[] = {11, 14, 13}, other_sampler[] = {21, 23}, swapped[] = {12, 11, 13};
  Expect(!w.Matches(6, 1, 7, views, 3, samplers, 2), "another frame: written");
  Expect(!w.Matches(5, 2, 7, views, 3, samplers, 2), "another epoch: written");
  Expect(!w.Matches(5, 1, 8, views, 3, samplers, 2), "another layout: written");
  Expect(!w.Matches(5, 1, 7, other_view, 3, samplers, 2), "another image view: written");
  Expect(!w.Matches(5, 1, 7, swapped, 3, samplers, 2), "the same views in another order: written");
  Expect(!w.Matches(5, 1, 7, views, 3, other_sampler, 2), "another sampler: written");
  Expect(!w.Matches(5, 1, 7, views, 2, samplers, 2) && !w.Matches(5, 1, 7, views, 3, samplers, 1),
         "other counts: written");
  w.Forget();
  Expect(!w.Matches(5, 1, 7, views, 3, samplers, 2), "forgotten (the pools cleared): written");
}

// Frames of draws, as UpdateBindings makes them: a driver that keeps the contents of every set
// written, and for every draw checks that the set bound holds the draw's descriptors (what the
// handles stand for, not the handles: a destroyed view's handle can come back for another view).
struct Model {
  bool use_epoch = true;
  uint64_t frame = 1, epoch = 1;
  uint32_t next_set = 1;
  uint64_t next_object = 1000;
  std::map<uint64_t, uint64_t> object_of_handle;     // live handle -> the object it stands for
  std::map<uint32_t, std::vector<uint64_t>> contents;  // set -> objects written into it
  Written written[2];
  int writes = 0, reuses = 0, wrong = 0;

  uint64_t Create(uint64_t handle) {
    object_of_handle[handle] = next_object++;
    return handle;
  }
  void Destroy(uint64_t handle) {  // the texture cache resets its bindings, or destroys a sampler
    object_of_handle.erase(handle);
    ++epoch;
  }
  void Draw(uint32_t stage, uint32_t layout, const std::vector<uint64_t>& views, const std::vector<uint64_t>& samplers) {
    std::vector<uint64_t> wanted;
    for (uint64_t h : views) wanted.push_back(object_of_handle.at(h));
    for (uint64_t h : samplers) wanted.push_back(object_of_handle.at(h));
    const uint64_t e = use_epoch ? epoch : 1;
    uint32_t set;
    if (written[stage].Matches(frame, e, layout, views.data(), uint32_t(views.size()), samplers.data(),
                               uint32_t(samplers.size()))) {
      set = written[stage].set;
      ++reuses;
    } else {
      set = next_set++;
      contents[set] = wanted;  // vkUpdateDescriptorSets
      written[stage].Remember(frame, e, layout, views.data(), uint32_t(views.size()), samplers.data(),
                              uint32_t(samplers.size()), set);
      ++writes;
    }
    if (contents[set] != wanted) ++wrong;
  }
};

void RunFrames(Model& m) {
  std::mt19937 random(104);
  std::vector<uint64_t> views = {m.Create(1), m.Create(2), m.Create(3), m.Create(4)};
  std::vector<uint64_t> samplers = {m.Create(51), m.Create(52)};
  for (int frame = 0; frame < 40; ++frame) {
    for (int draw = 0; draw < 200; ++draw) {
      // Most draws use the material of the draw before (runs of the same textures).
      if (random() % 8 == 0) std::swap(views[random() % 4], views[random() % 4]);
      const uint32_t layout = random() % 16 == 0 ? 2 : 1;
      m.Draw(0, layout, {views[0]}, {samplers[0]});
      m.Draw(1, layout, {views[1], views[2]}, {samplers[0], samplers[1]});
      if (draw == 100 && frame % 5 == 2) {
        // A texture not bound now is destroyed (memory limit) and its view's handle comes back
        // for a new texture, still in this frame.
        const uint64_t handle = views[3];
        m.Destroy(handle);
        m.Create(handle);
        std::swap(views[3], views[1]);  // the next draws bind the new texture with the old handle
      }
      if (draw == 150 && frame % 7 == 3) {
        const uint64_t handle = samplers[1];
        m.Destroy(handle);  // a sampler destroyed for another; the same handle again
        m.Create(handle);
      }
    }
    ++m.frame;
  }
}

void CheckModel() {
  Model m;
  RunFrames(m);
  std::printf("   %d texture sets written, %d reused in 40 frames of 200 draws\n", m.writes, m.reuses);
  Expect(m.wrong == 0, "every draw binds a set holding exactly its descriptors (" + std::to_string(m.wrong) + " wrong)");
  Expect(m.reuses > 3 * m.writes, "most sets are reused");
  Model no_epoch;
  no_epoch.use_epoch = false;
  RunFrames(no_epoch);
  Expect(no_epoch.wrong > 0, "without the epoch a destroyed handle's set would be bound for a new object");
}

void CheckCounting() {
  fc::TextureSetReporter reporter;
  (void)reporter.Line(1);
  fc::CountTextureSets(5, 5);  // not the GPU thread: not counted
  std::thread([] {
    fc::MarkGpuThread();
    for (int n = 0; n < 30; ++n) fc::CountTextureSets(n % 3 ? 2 : 1, n % 3 ? 0 : 1);
  }).join();
  const std::string line = reporter.Line(2);
  std::printf("   %s\n", line.c_str());
  Expect(line == "Performance texture sets (GPU thread, fix 104): 25/s reused, 5/s written (83% reused)", "the line");
  Expect(reporter.Line(1) == "Performance texture sets (GPU thread, fix 104): 0/s reused, 0/s written (0% reused)",
         "only since the previous line");
}

void CheckTree() {
  const std::string G = ".deps/xenia-canary/src/xenia/gpu/";
  const std::string th = Read(G + "texture_cache.h");
  Expect(th.find("uint64_t binding_epoch() const { return binding_epoch_; }") != std::string::npos &&
             th.find("void NoteBindingObjectsDestroyed() { ++binding_epoch_; }  // fix 104") != std::string::npos &&
             th.find("uint64_t binding_epoch_ = 1;  // fix 104") != std::string::npos,
         "the texture cache keeps an epoch of the handles its bindings give out");
  const std::string tc = Read(G + "texture_cache.cc");
  const size_t completed = tc.find("void TextureCache::CompletedSubmissionUpdated(");
  const size_t first_destroy = tc.find("if (!destroyed_any) {\n      destroyed_any = true;\n#if XE_PLATFORM_PS5", completed);
  const size_t bump = tc.find("NoteBindingObjectsDestroyed();", first_destroy);
  const size_t erase = tc.find("textures_.erase(found_texture_it);", bump);
  Expect(completed != std::string::npos && first_destroy != std::string::npos && bump != std::string::npos &&
             erase != std::string::npos && bump - first_destroy < 200,
         "textures destroyed for memory change it before the first is erased");
  Expect(tc.find("void TextureCache::DestroyAllTextures(bool from_destructor) {\n#if XE_PLATFORM_PS5\n"
                 "  NoteBindingObjectsDestroyed();  // fix 104\n#endif\n  ResetTextureBindings(from_destructor);\n"
                 "  textures_.clear();") != std::string::npos,
         "destroying all textures changes it");
  const size_t reset = tc.find("void TextureCache::ResetTextureBindings(bool from_destructor) {");
  const size_t reset_end = tc.find("\n}\n", reset);
  Expect(reset != std::string::npos && reset_end != std::string::npos &&
             tc.substr(reset, reset_end - reset).find("NoteBindingObjectsDestroyed") == std::string::npos,
         "an outdated texture (a bindings reset alone) keeps the sets reusable");
  size_t erases = 0;
  for (size_t at = tc.find("textures_.erase("); at != std::string::npos; at = tc.find("textures_.erase(", at + 1)) ++erases;
  Expect(erases == 1 && tc.find("textures_.clear();") != std::string::npos, "no other place destroys textures");
  const std::string vt = Read(G + "vulkan/vulkan_texture_cache.cc");
  const size_t destroy = vt.find("dfn.vkDestroySampler(device, sampler_used_first_->second.sampler, nullptr);");
  const size_t sampler_bump = vt.find("NoteBindingObjectsDestroyed();  // fix 104", destroy);
  Expect(destroy != std::string::npos && sampler_bump != std::string::npos && sampler_bump - destroy < 120,
         "a sampler destroyed to make room changes it");
  const std::string vh = Read(G + "vulkan/vulkan_command_processor.h");
  Expect(vh.find("#include \"xenia/gpu/vulkan/texture_set_reuse.h\"  // fix 104") != std::string::npos &&
             vh.find("      texture_sets_written_[2];") != std::string::npos,
         "a written set per stage");
  const std::string vk = Read(G + "vulkan/vulkan_command_processor.cc");
  const size_t ub = vk.find("bool VulkanCommandProcessor::UpdateBindings(");
  const size_t fill_both = vk.find("  bool write_vertex_textures = texture_count_vertex || sampler_count_vertex;\n"
                                   "  bool write_pixel_textures = texture_count_pixel || sampler_count_pixel;\n#else", ub);
  const size_t infos = vk.find("size_t pixel_sampler_image_info_offset = descriptor_write_image_info_.size();", fill_both);
  const size_t epoch = vk.find("const uint64_t texture_set_epoch = texture_cache_->binding_epoch();", infos);
  const size_t matches = vk.find("if (written.Matches(frame_current_, texture_set_epoch, layout,", epoch);
  const size_t rebind = vk.find("if (current_graphics_descriptor_sets_[set_index] != written.set) {\n"
                                "        current_graphics_descriptor_sets_[set_index] = written.set;\n"
                                "        current_graphics_descriptor_sets_bound_up_to_date_ &= ~set_bit;",
                                matches);
  const size_t stale = vk.find("current_graphics_descriptor_set_values_up_to_date_ &= ~set_bit;\n"
                               "      current_graphics_descriptor_sets_bound_up_to_date_ &= ~set_bit;\n    }\n  }",
                               rebind);
  const size_t remember_vertex = vk.find("texture_sets_written_[0].Remember(", stale);
  const size_t remember_pixel = vk.find("texture_sets_written_[1].Remember(", remember_vertex);
  const size_t update = vk.find("dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count,", remember_pixel);
  Expect(ub != std::string::npos && fill_both != std::string::npos && infos != std::string::npos &&
             epoch != std::string::npos && matches != std::string::npos && rebind != std::string::npos &&
             stale != std::string::npos && remember_vertex != std::string::npos && remember_pixel != std::string::npos &&
             update != std::string::npos,
         "UpdateBindings fills both stages, reuses or writes each, remembers what it writes");
  Expect(vk.find("void VulkanCommandProcessor::ClearTransientDescriptorPools() {\n#if XE_PLATFORM_PS5\n"
                 "  texture_sets_written_[0].Forget();") != std::string::npos,
         "clearing the pools forgets the sets");
  // Sets go back to the free lists only once their frame has completed.
  Expect(vk.find("if (used_transient_descriptor_set.frame > frame_completed_) {\n        break;") != std::string::npos &&
             vk.find("used_texture_descriptor_set.frame = frame_current_;") != std::string::npos,
         "a set of the current frame is not recycled");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t enums = main_code.find("XELOGW(\"{}\", xbox360ps5::enum_trace::Line());");
  const size_t sets = main_code.find("XELOGW(\"{}\", frame_cost_texture_sets.Line(double(summary_samples)));");
  Expect(enums != std::string::npos && sets != std::string::npos && enums < sets, "the summary gives the line after fix 81's");
}

}  // namespace

int main() {
  CheckMatches();
  CheckModel();
  CheckCounting();
  CheckTree();
  if (failures) {
    std::printf("check-texture-set-reuse: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-texture-set-reuse: ok\n");
  return 0;
}
