// SPDX-License-Identifier: MIT
// Fix 142i: in the fast readback mode, a resolve that would wait for the GPU for its copy (no
// earlier copy of its pass to hand over, or fix 91k's own copy), or whose earlier copy fix 59
// leaves for later, queues the copy; the copy is put in guest memory once the GPU has made it.
// Off by default; readback-deliver.txt switches it while the game runs.
//  (a) the wait is replaced only while on, with the copy queued and a one-shot read watch armed;
//  (b) fix 59's late copy is queued while on;
//  (c) a copy put in memory at the resolve marks its key, so an older queued copy never
//      overwrites it;
//  (d) the queue is emptied at each resolve and each submission's end: only copies the GPU has
//      made, oldest first; a reused slot or an older copy is dropped; fix 91h's nocopy is kept;
//  (e) CPU reads of a queued copy's range are counted as early, at both places reads are taken;
//  (f) the setting is off by default, the file read at most once a second, the line every 30 s.
// Runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
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
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
bool Has(const std::string& text, const std::string& part) {
  const bool found = text.find(part) != std::string::npos;
  if (!found) std::printf("  (not found: %s)\n", part.substr(0, 100).c_str());
  return found;
}
std::string Body(const std::string& text, const std::string& signature) {
  const size_t at = text.find(signature);
  if (at == std::string::npos) return std::string();
  const size_t end = text.find("\n}\n", at);
  return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

void CheckTree() {
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  Expect(!cp.empty(), "the tree is there (run from the assembled tree's root)");
  // (a)
  const size_t miss = cp.find("    } else if (read_cache_miss) {\n");
  const size_t fallback = cp.find("      read_index = write_index;", miss);
  const std::string defer = cp.substr(miss, fallback - miss);
  Expect(miss != std::string::npos && Has(defer, "if (fix142i::enabled) {") &&
             Has(defer, "fix142i::pending.push_back({resolve_key, write_index, rb.submissions[write_index],") &&
             Has(defer, "memory_->EnablePhysicalMemoryAccessCallbacks(\n            written_address, readback_length, false, true);  // fix 142i") &&
             Has(defer, "return true;"),
         "the wait is replaced only while on: the copy queued, a read watch armed, no wait");
  // Its read watch is wrapped apart from fix 91's, so checks that find fix 91's copy-then-arm order
  // by the first such call still find fix 91's own.
  Expect(cp.find("memory_->EnablePhysicalMemoryAccessCallbacks(written_address, readback_length, false,") >
             cp.find("memory::vastcpy(memory_->TranslatePhysical(written_address),"),
         "fix 91's first read watch still comes after its copy");
  Expect(cp.find("AwaitAllQueueOperationsCompletion()", fallback) < cp.find("const uint64_t read_submission = rb.submissions[read_index];", fallback),
         "off, the wait is as before");
  // (b)
  const size_t late = cp.find("if (!auto_readback) xbox360ps5::frame_cost::NoteLateReadback();");
  Expect(late != std::string::npos &&
             cp.find("if (!auto_readback && fix142i::enabled) {", late) < cp.find("return true;", late) &&
             Has(cp, "fix142i::pending.push_back({resolve_key, read_index, read_submission, written_address,"),
         "fix 59's late copy is queued while on");
  // (c)
  const size_t copy = cp.find("memory::vastcpy(memory_->TranslatePhysical(written_address),");
  Expect(copy != std::string::npos && cp.find("newest = std::max(newest, read_submission);", copy) <
                                          cp.find("// Fix 91: a one-shot read watch on the copy", copy),
         "a copy put in memory at the resolve marks its key");
  // (d)
  const std::string deliver = Body(cp, "void VulkanCommandProcessor::Fix142iDeliver() {");
  Expect(!deliver.empty() && Has(deliver, "if (fix142i::pending.empty()) return;") &&
             Has(deliver, "completion_timeline_.UpdateAndGetCompletedSubmission()") &&
             Has(deliver, "return a.submission < b.submission;") &&
             Has(deliver, "if (p.submission > completed) {") &&
             Has(deliver, "it->second.submissions[p.slot] != p.submission") &&
             Has(deliver, "newest >= p.submission") &&
             Has(deliver, "xbox360ps5::readback_keys::NoCopy(g_readback_rules, p.address, rb.cpu_reads)") &&
             Has(deliver, "dfn.vkUnmapMemory(device, rb.memories[p.slot]);"),
         "only made copies, oldest first; reused slots and older copies dropped; nocopy kept");
  const std::string end_submission = Body(cp, "bool VulkanCommandProcessor::EndSubmission(bool is_swap) {");
  Expect(Has(end_submission, "fix142i::Refresh();") && Has(end_submission, "Fix142iDeliver();"),
         "the switch and the queue at each submission's end");
  const size_t key = cp.find("MakeReadbackResolveKey(written_address, written_length);");
  Expect(key != std::string::npos && cp.find("Fix142iDeliver();", key) < cp.find("readback_reads_.Take(", key),
         "the queue at each resolve, before its reads are taken");
  // (e)
  Expect(Has(deliver, "Fix142iEarlyRead(read_address, read_length);") &&
             cp.find("Fix142iEarlyRead(read_address, read_length);  // fix 142i", key) != std::string::npos,
         "early reads counted where reads are taken");
  // (f)
  Expect(Has(cp, "DEFINE_bool(readback_deliver_when_done, false,") &&
             Has(cp, "XBOX360PS5_DATA_ROOT \"/readback-deliver.txt\"") &&
             Has(cp, "if (logged && now - checked < std::chrono::seconds(1)) return;") &&
             Has(cp, "Readback delivery, last 30 s (fix 142i, {}):"),
         "off by default, the file at most once a second, the line every 30 s");
}

}  // namespace

int main() {
  CheckTree();
  if (failures) {
    std::printf("check-readback-deliver: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-deliver: ok\n");
  return 0;
}
