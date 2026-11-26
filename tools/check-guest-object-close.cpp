// SPDX-License-Identifier: MIT
// Fix 174 (tools/source-1.0.2/fixes/174-guest-object-close.patch; Harry Potter Kinect, issue
// 76): NtClose runs the close procedure of a type the title defined for ObCreateObject. See
// include/xbox360ps5/guest_object_close.hpp.
//
//   check-guest-object-close        run from an assembled tree's root
//
//  1. the registry: an object made with a title type is noted; the handles ObOpenObjectByPointer
//     gives are counted; a close reports the count before it (1 for the last) and the type;
//     nothing for an object not noted, a handle never counted, or a header that names another
//     type now (the memory reused); an object kept after its last close can be opened again;
//     a reused address starts over; a full table forgets closed objects first; kernel types
//     (inside the kernel's globals block) are not title types;
//  2. a model of the NUI library's image streams (Harry Potter: NuiImageStreamOpen 82A4CAD8,
//     its stream table at 82E4F6F8, the type's close procedure 82A4C858): without the close
//     procedure a camera thread's second open of stream type 2 gets 0x83010003 ("already open")
//     and the title stops on its own check (827E0F78); with it the slot is free again; a first
//     close of two handles leaves the stream open;
//  3. the tree: ObCreateObject notes title types, ObOpenObjectByPointer counts its handles, and
//     the guest's NtClose runs the close procedure (object, count) on the calling thread before it
//     releases the handle, logged "(fix 174)"; the host's NtClose(handle) is unchanged.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "xbox360ps5/guest_object_close.hpp"

namespace goc = xbox360ps5::guest_object_close;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

constexpr uint32_t kNuiStreamType = 0x82D18CF8;  // Harry Potter's 'Nuio' type
constexpr uint32_t kStream = 0x407D4010;

static void CheckRegistry() {
  goc::Registry r;
  // Not noted: nothing to run.
  CHECK(!r.Opened(kStream));
  CHECK(r.Closing(kStream, kNuiStreamType).type == 0);
  r.Created(kStream, kNuiStreamType);
  CHECK(r.size() == 1);
  // Created but never opened by pointer: a close of some other handle runs nothing.
  CHECK(r.Closing(kStream, kNuiStreamType).type == 0);
  CHECK(r.Opened(kStream));
  auto close = r.Closing(kStream, kNuiStreamType);
  CHECK(close.type == kNuiStreamType && close.handle_count == 1);
  CHECK(r.Closing(kStream, kNuiStreamType).type == 0);  // no handle left
  // Two handles: the first close says 2, the last 1.
  CHECK(r.Opened(kStream) && r.Opened(kStream));
  CHECK(r.Closing(kStream, kNuiStreamType).handle_count == 2);
  CHECK(r.Closing(kStream, kNuiStreamType).handle_count == 1);
  // The memory now holds an object of another type: forgotten.
  CHECK(r.Opened(kStream));
  CHECK(r.Closing(kStream, 0x82000000).type == 0);
  CHECK(r.size() == 0);
  CHECK(!r.Opened(kStream));
  // A reused address starts over (no handles carried over).
  r.Created(kStream, kNuiStreamType);
  r.Opened(kStream);
  r.Created(kStream, kNuiStreamType);
  CHECK(r.Closing(kStream, kNuiStreamType).type == 0);
  r.Created(0, kNuiStreamType);
  r.Created(kStream + 0x100, 0);
  CHECK(r.size() == 1);
  // Full: the objects without an open handle go first; one with a handle stays.
  goc::Registry full;
  full.Created(0x40000000, kNuiStreamType);
  full.Opened(0x40000000);
  for (uint32_t i = 1; i < goc::Registry::kMaxObjects; ++i)
    full.Created(0x40000000 + i * 0x100, kNuiStreamType);
  CHECK(full.size() == goc::Registry::kMaxObjects);
  full.Created(0x50000000, kNuiStreamType);
  CHECK(full.size() == 2);
  CHECK(full.Closing(0x40000000, kNuiStreamType).handle_count == 1);
  CHECK(full.Opened(0x50000000));
  // Title types are those outside the kernel's globals block.
  CHECK(goc::TitleType(kNuiStreamType, 0x80040000, 0x80041000));
  CHECK(!goc::TitleType(0x80040100, 0x80040000, 0x80041000));
  CHECK(goc::TitleType(0x80041000, 0x80040000, 0x80041000));
  CHECK(!goc::TitleType(0, 0x80040000, 0x80041000));
  CHECK(goc::kCloseProc == 0x8 && goc::kObjectHeaderSize == 0x10);
}

// ---- A model of the NUI library's image streams -------------------------------------------
constexpr int32_t kAlreadyOpen = int32_t(0x83010003);
struct NuiLibrary {
  std::map<uint32_t, uint32_t> slot;  // channel -> stream object (82E4F6F8 + 4 * channel)
  uint32_t next_object = 0x407D4010;
};
struct Kernel {
  goc::Registry registry;
  bool runs_close_procedure;  // fix 174
  std::map<uint32_t, uint32_t> handles;  // handle -> object
  uint32_t next_handle = 0xF80002AC;
};
// NuiImageStreamOpen (82A4CAD8): refuse a taken slot, else ObCreateObject + ObOpenObjectByPointer.
static int32_t StreamOpen(NuiLibrary& nui, Kernel& k, uint32_t channel, uint32_t* handle) {
  if (nui.slot[channel]) return kAlreadyOpen;
  const uint32_t object = nui.next_object;
  nui.next_object += 0x100;
  k.registry.Created(object, kNuiStreamType);
  nui.slot[channel] = object;
  k.registry.Opened(object);
  *handle = k.next_handle;
  k.next_handle += 4;
  k.handles[*handle] = object;
  return 0;
}
// The type's close procedure (82A4C858): a title thread's last handle frees the slot.
static void StreamCloseProcedure(NuiLibrary& nui, uint32_t object, uint32_t handle_count) {
  if (handle_count != 1) return;
  for (auto& [channel, held] : nui.slot)
    if (held == object) held = 0;
}
static void NtClose(NuiLibrary& nui, Kernel& k, uint32_t handle) {
  const uint32_t object = k.handles[handle];
  k.handles.erase(handle);
  if (!k.runs_close_procedure) return;
  const auto close = k.registry.Closing(object, kNuiStreamType);
  if (close.type == kNuiStreamType) StreamCloseProcedure(nui, object, close.handle_count);
}
// Harry Potter's camera thread function (827E0F28): open stream type 2, use it, close it.
static int32_t CameraThread(NuiLibrary& nui, Kernel& k) {
  uint32_t handle = 0;
  const int32_t opened = StreamOpen(nui, k, 2, &handle);
  if (opened < 0) return opened;  // 827E0F74: bge, else stb r27,0(0)
  NtClose(nui, k, handle);
  return 0;
}

static void CheckModel() {
  {
    NuiLibrary nui;
    Kernel k{{}, false};
    CHECK(CameraThread(nui, k) == 0);              // thread 28
    CHECK(CameraThread(nui, k) == kAlreadyOpen);   // thread 44: the crash at 68 s
  }
  {
    NuiLibrary nui;
    Kernel k{{}, true};
    CHECK(CameraThread(nui, k) == 0);
    CHECK(CameraThread(nui, k) == 0);
    CHECK(CameraThread(nui, k) == 0);
  }
  {
    // Two handles to one stream: the first close (count 2) keeps it open.
    NuiLibrary nui;
    Kernel k{{}, true};
    uint32_t first = 0;
    CHECK(StreamOpen(nui, k, 2, &first) == 0);
    const uint32_t object = k.handles[first];
    CHECK(k.registry.Opened(object));
    const uint32_t second = 0xF8000400;
    k.handles[second] = object;
    NtClose(nui, k, first);
    uint32_t other = 0;
    CHECK(StreamOpen(nui, k, 2, &other) == kAlreadyOpen);
    NtClose(nui, k, second);
    CHECK(StreamOpen(nui, k, 2, &other) == 0);
  }
}

// ---- The tree -----------------------------------------------------------------------------
static std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  std::string out;
  for (char c : s.str())
    if (c != '\r') out += c;
  return out;
}
static std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
static bool Has(const std::string& text, const char* what) {
  return text.find(what) != std::string::npos;
}

static void CheckTree() {
  const std::string ob = ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_ob.cc");
  if (ob.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_ob.cc)\n");
    ++failures;
    return;
  }
  CHECK(Has(ob, "#include \"xbox360ps5/guest_object_close.hpp\""));
  const std::string create = Body(ob, "dword_result_t ObCreateObject_entry(");
  CHECK(Has(create, "goc::TitleType(object_factory.guest_address(), kernel_types,"));
  CHECK(Has(create, "goc::registry().Created(out_object_tmp, object_factory.guest_address());"));
  const std::string open = Body(ob, "dword_result_t ObOpenObjectByPointer_entry(");
  CHECK(Has(open, "goc::registry().Opened(object->guest_object());"));
  CHECK(open.find("object->RetainHandle();") < open.find("goc::registry().Opened("));
  const std::string guest_close = Body(ob, "dword_result_t NtClose_entry(");
  CHECK(Has(guest_close, "const ppc_context_t& ctx"));
  CHECK(Has(guest_close, "goc::registry().Closing(object->guest_object(), header_type)"));
  CHECK(Has(guest_close, "ctx->processor->Execute(ctx->thread_state, close_proc, args, 2);"));
  CHECK(Has(guest_close, "(fix 174)"));
  CHECK(guest_close.find("Execute(") < guest_close.find("return NtClose(handle);"));
  // The host's NtClose (XAM's own handles) runs no guest code.
  const std::string host_close = Body(ob, "uint32_t NtClose(uint32_t handle) {");
  CHECK(!host_close.empty() && !Has(host_close, "Execute("));
  CHECK(Has(ob, "DECLARE_XBOXKRNL_EXPORT1(NtClose, kNone, kImplemented);"));
}

int main() {
  CheckRegistry();
  CheckModel();
  CheckTree();
  if (failures) {
    std::printf("check-guest-object-close: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-guest-object-close: ok\n");
  return 0;
}
