// SPDX-License-Identifier: MIT
// Fix 173b (tools/source-1.0.2/fixes/173b-pipeline-cache-skip.patch): a Vulkan pipeline cache snapshot
// equal to the file on the drive (size and 64-bit hash) is not written again (GTA IV's exit waited
// 12.3 s for an unchanged 16 MB file on a throttled drive).
//  (a) include/xbox360ps5/pipeline_cache_skip.hpp: the hash: the same bytes the same; one byte changed,
//      another size or a different tail another;
//  (b) the store: nothing known, nothing unchanged; noted, the same snapshot unchanged; a changed byte or
//      another size not; each cache path apart; forgotten, not;
//  (c) a failed write keeps the old size and hash, so the next snapshot is written; a successful one
//      takes the new;
//  (d) the tree: the cache notes the data it loaded once its header is accepted, checks a snapshot
//      before WriteCacheAtomically (logging "unchanged, not written" and counting it as saved), and
//      notes a snapshot only after a successful write.
// Fix 173c (173c-pipeline-cache-no-new-pipeline.patch): the driver orders its cache differently each
// time, so 173b's bytes never matched (1.0.6-work-fix173b: the same size, written again, timed out).
//  (e) a snapshot of the size on the drive is not written when no pipeline was made since the file was
//      noted; one made, written; another size, written; the same bytes still not written; noting the
//      written file starts the count again;
//  (f) <file>.pending-N beside the cache (a write cut off) removed, other files kept;
//  (g) the tree: a pipeline made in ConfigurePipeline counted (not the start's stored ones), the stale
//      files removed before the load, and the snapshot decided by Check before its write.
// Run from the assembled tree's root.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <vector>

#include "xbox360ps5/pipeline_cache_skip.hpp"

namespace {
namespace pcs = xbox360ps5::pipeline_cache_skip;
int failures = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok) ++failures;
}
std::vector<uint8_t> Data(size_t size, uint8_t seed) {
  std::vector<uint8_t> d(size);
  for (size_t i = 0; i < size; ++i) d[i] = uint8_t(seed + i * 31 + (i >> 7));
  return d;
}

void Hash() {
  const auto a = Data(1 << 20, 3), b = a;
  Expect(pcs::Hash(a.data(), a.size()) == pcs::Hash(b.data(), b.size()), "(a) the same bytes, the same hash");
  auto c = a;
  c[123457] ^= 1;
  Expect(pcs::Hash(c.data(), c.size()) != pcs::Hash(a.data(), a.size()), "(a) one bit changed, another hash");
  Expect(pcs::Hash(a.data(), a.size() - 1) != pcs::Hash(a.data(), a.size()), "(a) another size, another hash");
  auto t = Data(37, 9), u = t;
  u[36] ^= 0x80;
  Expect(pcs::Hash(t.data(), t.size()) != pcs::Hash(u.data(), u.size()), "(a) a changed tail byte, another hash");
}

void Store() {
  pcs::Store store;
  const std::string path = "/data/homebrew/PPSA83600/cache/core/shaders/local/545407F2.vk.bin";
  const auto loaded = Data(15951112 / 16, 5);
  Expect(!store.Unchanged(path, loaded.data(), loaded.size()), "(b) nothing known: written");
  store.Note(path, loaded.data(), loaded.size());
  auto snapshot = loaded;
  Expect(store.Unchanged(path, snapshot.data(), snapshot.size()), "(b) the same as loaded: not written");
  snapshot[42] ^= 1;
  Expect(!store.Unchanged(path, snapshot.data(), snapshot.size()), "(b) a changed byte: written");
  auto grown = loaded;
  grown.push_back(7);
  Expect(!store.Unchanged(path, grown.data(), grown.size()), "(b) grown: written");
  Expect(!store.Unchanged(path + ".other", loaded.data(), loaded.size()), "(b) another cache path: its own");
  store.Forget(path);
  Expect(!store.Unchanged(path, loaded.data(), loaded.size()), "(b) forgotten: written");

  // (c) a failed write leaves the old values; a successful one takes the new.
  store.Note(path, loaded.data(), loaded.size());
  Expect(!store.Unchanged(path, grown.data(), grown.size()), "(c) a new snapshot: to be written");
  // The write fails: nothing noted.
  Expect(!store.Unchanged(path, grown.data(), grown.size()), "(c) after a failed write, the next snapshot is written");
  store.Note(path, grown.data(), grown.size());  // the write succeeded
  Expect(store.Unchanged(path, grown.data(), grown.size()) && !store.Unchanged(path, loaded.data(), loaded.size()),
         "(c) after a successful write, the new data is what the drive has");
}

void Wiring() {
  std::ifstream input(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc", std::ios::binary);
  std::stringstream buffer;
  buffer << input.rdbuf();
  std::string code;
  for (char c : buffer.str())
    if (c != ' ' && c != '\r' && c != '\n' && c != '\t') code += c;
  const size_t accepted = code.find("\"Vulkancache:accepted{}bytesfortitle{:08X}\"");
  const size_t note_load = code.find("xbox360ps5::pipeline_cache_skip::Global().Note(vk_pipeline_cache_path_.string(),"
                                     "pipeline_cache_data.data(),pipeline_cache_data.size());",
                                     accepted);
  Expect(accepted != std::string::npos && note_load != std::string::npos,
         "(d) the loaded data noted once its header is accepted");
#ifdef XBOX360PS5_PIPELINE_CACHE_SKIP_PIPELINES
  const size_t check = code.find("constautoverdict=on_drive.Check(vk_pipeline_cache_path_.string(),data.data(),written);");
  const size_t skipped = code.find("unchanged,not\"\"written(fix173c:{})\"", check);
#else
  const size_t check = code.find("if(on_drive.Unchanged(vk_pipeline_cache_path_.string(),data.data(),written)){");
  const size_t skipped = code.find("unchanged,not\"\"written(fix173b)\"", check);
#endif
  const size_t returned = code.find("returntrue;", skipped);
  const size_t write = code.find("constboolsaved=xbox360ps5::WriteCacheAtomically(", check);
  const size_t note_save = code.find("if(saved){on_drive.Note(vk_pipeline_cache_path_.string(),data.data(),written);}", write);
  Expect(check != std::string::npos && skipped != std::string::npos && returned != std::string::npos &&
             write != std::string::npos && returned < write && note_save != std::string::npos,
         "(d) an unchanged snapshot logged and counted as saved before the write; noted only after a successful write");
#ifdef XBOX360PS5_PIPELINE_CACHE_SKIP_PIPELINES
  const size_t configure = code.find("boolVulkanPipelineCache::ConfigurePipeline(");
  const size_t emplace = code.find("*pipelines_.emplace(description,Pipeline(pipeline_layout)).first;", configure);
  const size_t counted = code.find("xbox360ps5::pipeline_cache_skip::Global().NotePipelineCreated();", configure);
  const size_t stored = code.find("*pipelines_.emplace(pipeline_description,Pipeline(pipeline_layout))");
  Expect(configure != std::string::npos && emplace != std::string::npos && counted != std::string::npos &&
             emplace < counted && counted - emplace < 200 && stored != std::string::npos &&
             code.find("NotePipelineCreated", stored) == std::string::npos,
         "(g) a pipeline made in ConfigurePipeline counted, not the start's stored ones");
  const size_t stale = code.find("xbox360ps5::pipeline_cache_skip::RemoveStalePending(vk_pipeline_cache_path_)");
  const size_t load = code.find("//TrytoloadexistingVkPipelineCachedata.", stale);
  Expect(stale != std::string::npos && load != std::string::npos, "(g) the stale files removed before the load");
#endif
}

#ifdef XBOX360PS5_PIPELINE_CACHE_SKIP_PIPELINES
void NoNewPipeline() {
  pcs::Store store;
  const std::string path = "/data/cache/545407F2.vk.bin";
  const auto loaded = Data(4096 + 13, 1);
  auto reordered = loaded;
  std::reverse(reordered.begin() + 32, reordered.end());  // the same size, the driver's other order
  Expect(store.Check(path, reordered.data(), reordered.size()) == pcs::Verdict::kWrite, "(e) nothing known: written");
  store.Note(path, loaded.data(), loaded.size());
  Expect(store.Check(path, loaded.data(), loaded.size()) == pcs::Verdict::kSameBytes, "(e) the same bytes: not written");
  Expect(store.Check(path, reordered.data(), reordered.size()) == pcs::Verdict::kSameSizeNoNewPipeline &&
             !store.Unchanged(path, reordered.data(), reordered.size()),
         "(e) the same size in another order, no new pipeline: not written (173b alone would write)");
  store.NotePipelineCreated();
  Expect(store.Check(path, reordered.data(), reordered.size()) == pcs::Verdict::kWrite,
         "(e) a pipeline made this session: written");
  auto grown = loaded;
  grown.resize(grown.size() + 512, 9);
  Expect(store.Check(path, grown.data(), grown.size()) == pcs::Verdict::kWrite, "(e) another size: written");
  store.Note(path, reordered.data(), reordered.size());  // the write succeeded
  Expect(store.Check(path, loaded.data(), loaded.size()) == pcs::Verdict::kSameSizeNoNewPipeline,
         "(e) noted after the write: the count starts again");
}

void Stale() {
  const auto folder = std::filesystem::temp_directory_path() / "check-pipeline-cache-skip";
  std::error_code error;
  std::filesystem::remove_all(folder, error);
  std::filesystem::create_directories(folder, error);
  const auto cache = folder / "545407F2.vk.bin";
  for (const char* name : {"545407F2.vk.bin", "545407F2.vk.bin.pending-0", "545407F2.vk.bin.pending-3",
                           "4D5308C9.vk.bin.pending-0", "545407F2.xpso"}) {
    std::ofstream(folder / name) << "x";
  }
  Expect(pcs::RemoveStalePending(cache) == 2 && std::filesystem::exists(cache) &&
             !std::filesystem::exists(folder / "545407F2.vk.bin.pending-0") &&
             std::filesystem::exists(folder / "4D5308C9.vk.bin.pending-0") &&
             std::filesystem::exists(folder / "545407F2.xpso"),
         "(f) this cache's .pending-N files removed; the cache, other titles' and other files kept");
  Expect(pcs::RemoveStalePending(folder / "missing" / "X.vk.bin") == 0, "(f) a missing folder: nothing");
  std::filesystem::remove_all(folder, error);
}
#endif
}  // namespace

int main() {
  Hash();
  Store();
#ifdef XBOX360PS5_PIPELINE_CACHE_SKIP_PIPELINES
  NoNewPipeline();
  Stale();
#endif
  if (std::ifstream(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc")) {
    Wiring();
  } else {
    Expect(false, "run from the assembled tree's root");
  }
  if (failures) {
    std::printf("FAIL: %d\n", failures);
    return 1;
  }
  std::printf("PASS: an unchanged pipeline cache snapshot is not written again\n");
  return 0;
}
