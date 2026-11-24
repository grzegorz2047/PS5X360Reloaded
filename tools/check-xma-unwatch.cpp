// SPDX-License-Identifier: MIT
// Fix 67 (tools/source-1.0.2/fixes/67-xma-clear-unwatch.patch): an XMA context clear lifts the
// memory watch on its context before writing it. Ninja Gaiden II on 1.0.4_candidate_23: the
// clear, an MMIO store emulated inside the fault handler, wrote the context into a page a write
// watch held read-only and faulted again inside the handler (fix 65 handles that, and wrote
// "[X360] FAULT a fault inside the fault handler" at 0x10ffcaa000 and the next contexts).
// The assembled tree: in XmaDecoder::WriteRegister's Context0Clear..9Clear branch the context's
// own 64 bytes (context_data_first_ptr_ + id * sizeof(XMA_CONTEXT_DATA)) go through
// Memory::TriggerPhysicalMemoryCallbacks, with the global critical region taken once, as a write
// and not an exact-range unwatch, before Clear; the first such lift is logged once; console only.
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
  const std::string code = Read(".deps/xenia-canary/src/xenia/apu/xma_decoder.cc");
  Expect(!code.empty(), "xma_decoder.cc");
  // Fix 155 passes the context id too (its trace logs each lift).
  size_t helper = code.find("void UnwatchContext(Memory* memory, uint32_t guest_address, uint32_t bytes) {");
  if (helper == std::string::npos)
    helper = code.find("void UnwatchContext(Memory* memory, uint32_t guest_address, uint32_t bytes, uint32_t context_id) {");
  const size_t trigger = code.find(
      "memory->TriggerPhysicalMemoryCallbacks(xe::global_critical_region::Acquire(), guest_address, bytes,", helper);
  const size_t write = code.find("true, false))", trigger);
  Expect(helper != std::string::npos && trigger != std::string::npos && write != std::string::npos,
         "the helper lifts the watch as a write, the global critical region taken once");
  const size_t guard = code.rfind("#if XE_PLATFORM_PS5", helper);
  Expect(guard != std::string::npos && code.find("#endif", guard) > helper, "console only");
  Expect(code.find("XMA: a context clear lifted the memory watch on its context at {:08X} first (fix 67)") !=
             std::string::npos,
         "the first lift is logged once");

  const size_t clear = code.find("r >= XmaRegister::Context0Clear &&");
  const size_t unwatch = code.find(
      "UnwatchContext(memory(), context_data_first_ptr_ + context_id * uint32_t(sizeof(XMA_CONTEXT_DATA)),", clear);
  size_t bytes = code.find("uint32_t(sizeof(XMA_CONTEXT_DATA)));  // fix 67", unwatch);
  if (bytes == std::string::npos)
    bytes = code.find("uint32_t(sizeof(XMA_CONTEXT_DATA)), context_id);  // fix 67 (fix 155: the id)", unwatch);
  const size_t call = code.find("context.Clear();", clear);
  const size_t next_branch = code.find("} else {", clear);
  Expect(clear != std::string::npos && unwatch != std::string::npos && bytes != std::string::npos &&
             unwatch < call && call < next_branch,
         "the clear branch lifts the context's own 64 bytes before Clear writes them");
}

}  // namespace

int main() {
  CheckTree();
  if (failures) {
    std::printf("check-xma-unwatch: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-xma-unwatch: ok\n");
  return 0;
}
