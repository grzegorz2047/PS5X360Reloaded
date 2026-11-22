// SPDX-License-Identifier: MIT
// Fix 79 (tools/source-1.0.2/fixes/79-system-heap-holders.patch): the emulator's allocations in
// the guest's 4 KiB heap by holder. Battlefield 3 on 1.0.4 and 1.0.5 candidates: that heap grows
// by about 0.45 MiB a second in play while the game's own allocations stay flat, until after
// about 14 minutes an allocation fails and the game reads through a null pointer.
//  (a) include/xbox360ps5/system_heap_watch.hpp: each allocation noted with its caller until it
//      is freed; a kernel object's two blocks (XObject::CreateNative) moved to its object type;
//      the line gives the holders that gained the most since the previous line, then the most
//      held, with what they allocated and freed, and the anchor's address;
//  (b) the assembled tree: Memory::SystemHeapAlloc notes each virtual allocation with its
//      caller, SystemHeapFree each free, CreateNative both blocks with the object's type, and
//      the summary gives the line after fix 78's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/system_heap_watch.hpp"

#include <cstdio>
#include <fstream>
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
  return text.str();
}
namespace sh = xbox360ps5::system_heap;
const void* const kLeaker = reinterpret_cast<const void*>(uintptr_t(0x5000));
const void* const kTidy = reinterpret_cast<const void*>(uintptr_t(0x6000));

void CheckHolders() {
  (void)sh::Line(0x1234);
  uint32_t next = 0x30000000;
  // A caller that frees everything, from two threads at once.
  std::vector<std::thread> threads;
  for (int t = 0; t < 2; ++t) {
    threads.emplace_back([t] {
      for (uint32_t n = 0; n < 500; ++n) {
        const uint32_t address = 0x31000000 + (uint32_t(t) << 24) + (n << 12);  // apart: 2 MiB each
        sh::NoteAlloc(address, 64, kTidy);
        sh::NoteFree(address);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  // A caller that keeps 40 of 50 blocks of 24 bytes.
  std::vector<uint32_t> kept;
  for (int n = 0; n < 50; ++n) {
    sh::NoteAlloc(next, 24, kLeaker);
    if (n % 5 == 0) sh::NoteFree(next); else kept.push_back(next);
    next += 0x1000;
  }
  // Three events made by XObject::CreateNative (object and type blocks), one of them closed.
  std::vector<std::pair<uint32_t, uint32_t>> events;
  for (int n = 0; n < 3; ++n) {
    const uint32_t object = next, type = next + 0x1000;
    next += 0x2000;
    sh::NoteAlloc(object, 0x20, kLeaker);
    sh::NoteObject(object, 2);
    sh::NoteAlloc(type, 0x10, kLeaker);
    sh::NoteObject(type, 2);
    events.push_back({object, type});
  }
  sh::NoteFree(events[0].first);
  sh::NoteFree(events[0].second);
  sh::NoteFree(0x3FFFF000);  // never noted: ignored
  const std::string line = sh::Line(0x1234);
  std::printf("   %s\n", line.c_str());
  Expect(line.find("System heap (emulator allocations in the guest 4 KiB heap): 44 live, ") == 0, "44 blocks held in all");
  Expect(line.find("; by holder: caller 0x5000 live 40 (+40 since the previous line), allocated 50, freed 10, 24 bytes each; "
                   "object Event live 4 (+4 since the previous line), allocated 6, freed 2, 24 bytes each; anchor 0x1234") !=
             std::string::npos,
         "the holders that gained most, the object's blocks under its type, a caller that freed all left out");
  for (uint32_t address : kept) sh::NoteFree(address);
  for (int n = 0; n < 5; ++n) sh::NoteAlloc(next + (uint32_t(n) << 12), 8, kTidy);
  const std::string second = sh::Line(0x1234);
  std::printf("   %s\n", second.c_str());
  Expect(second.find("by holder: caller 0x6000 live 5 (+5 since the previous line)") != std::string::npos &&
             second.find("object Event live 4 (+0 since the previous line)") != std::string::npos &&
             second.find("caller 0x5000") == std::string::npos,
         "since the previous line; a holder with nothing left is not shown");
}

void CheckTree() {
  const std::string memory = Read(".deps/xenia-canary/src/xenia/memory.cc");
  const size_t alloc = memory.find("uint32_t Memory::SystemHeapAlloc(uint32_t size, uint32_t alignment,");
  const size_t zero = memory.find("Zero(address, size);", alloc);
  const size_t note = memory.find("xbox360ps5::system_heap::NoteAlloc(address, size, __builtin_return_address(0));", alloc);
  const size_t free_fn = memory.find("void Memory::SystemHeapFree(uint32_t address, uint32_t* out_region_size) {");
  const size_t note_free = memory.find("xbox360ps5::system_heap::NoteFree(address);  // fix 79", free_fn);
  const size_t release = memory.find("heap->Release(address, out_region_size);", free_fn);
  Expect(alloc != std::string::npos && zero != std::string::npos && note > zero && note < free_fn && note_free != std::string::npos &&
             note_free < release,
         "SystemHeapAlloc notes each allocation with its caller, SystemHeapFree each free before the release");
  const std::string object = Read(".deps/xenia-canary/src/xenia/kernel/xobject.cc");
  const size_t create = object.find("uint8_t* XObject::CreateNative(uint32_t size) {");
  const size_t mem_note = object.find("xbox360ps5::system_heap::NoteObject(mem, uint32_t(type()));  // fix 79", create);
  const size_t type_alloc = object.find("auto object_type = memory()->SystemHeapAlloc(sizeof(X_OBJECT_TYPE));", create);
  const size_t type_note = object.find("xbox360ps5::system_heap::NoteObject(object_type, uint32_t(type()));  // fix 79", type_alloc);
  Expect(create != std::string::npos && mem_note != std::string::npos && type_alloc != std::string::npos && type_note != std::string::npos &&
             type_note - type_alloc < 200,
         "CreateNative moves both blocks to the object's type");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t hot = main_code.find("XELOGW(\"{}\", frame_cost_hot.Line(double(summary_samples)));");
  const size_t heap = main_code.find("XELOGW(\"{}\", xbox360ps5::system_heap::Line());");
  Expect(hot != std::string::npos && heap != std::string::npos && hot < heap, "the summary gives the line after fix 78's");
}

}  // namespace

int main() {
  CheckHolders();
  CheckTree();
  if (failures) {
    std::printf("check-system-heap: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-system-heap: ok\n");
  return 0;
}
