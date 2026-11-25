// SPDX-License-Identifier: MIT
// Fix 178 (tools/source-1.0.2/fixes/178-virtual-mic-array.patch; Harry Potter Kinect, issue 76):
// a virtual Kinect microphone array that is there and hears silence. See
// include/xbox360ps5/virtual_mic.hpp.
//
//   check-virtual-mic        run from an assembled tree's root
//
//  1. the clock: 187.5 tics a second, blocks end at 16-bit tics divisible by 3, a block's
//     sequence is the tic it started at;
//  2. a request (Zumba's 82A6E510 layout: size 0x68, five buffers and their sizes) is accepted
//     only with every buffer writable at its size; completing it zeroes the five buffers, writes
//     the sequence and a centred beam, and leaves the sizes;
//  3. the queue: oldest first, only at a block's end, at most 8 waiting;
//  4. a model of the NUI audio library's pump (Zumba 8254DDE8: the noted tic, the step of 3,
//     the lock onto the stream, its reset) fed by the pump thread's loop: it locks on within 4
//     blocks, gets a block every 3 tics after that and never resets in 60000 tics; across the
//     16-bit wrap it resets at most once and locks on again; a late thread (skipped tics) only
//     costs a resynchronisation;
//  5. the tree: the four render-driver exports and XamVoiceGetMicArrayUnderrunStatus are
//     defined; XamVoiceGetMicArrayAudioEx queues for the virtual array before fix 156f's answer;
//     the switch kinect_microphone (on); fix 170's audio read rule is off with the array.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "xbox360ps5/virtual_mic.hpp"

namespace vm = xbox360ps5::virtual_mic;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// Guest memory: big-endian bytes in pages; pages not listed are unmapped, `read_only` ones
// cannot be written.
struct FakeMemory : vm::Memory {
  std::map<uint32_t, std::vector<uint8_t>> pages;  // 4 KiB pages
  std::map<uint32_t, bool> read_only;
  void Map(uint32_t from, uint32_t size, uint8_t fill = 0xAB) {
    for (uint32_t page = from & ~4095u; page < from + size; page += 4096)
      pages.emplace(page, std::vector<uint8_t>(4096, fill));
  }
  uint8_t* At(uint32_t address) {
    auto it = pages.find(address & ~4095u);
    return it == pages.end() ? nullptr : &it->second[address & 4095];
  }
  bool Mapped(uint32_t address, uint32_t size, bool write) {
    for (uint64_t at = address; at < uint64_t(address) + size; at = (at & ~4095ull) + 4096) {
      if (!pages.count(uint32_t(at) & ~4095u)) return false;
      if (write && read_only[uint32_t(at) & ~4095u]) return false;
    }
    return true;
  }
  bool Read32(uint32_t address, uint32_t* value) override {
    if (!Mapped(address, 4, false)) return false;
    *value = 0;
    for (int i = 0; i < 4; ++i) *value = (*value << 8) | *At(address + i);
    return true;
  }
  bool Writable(uint32_t address, uint32_t size) override { return Mapped(address, size, true); }
  void Zero(uint32_t address, uint32_t size) override {
    for (uint32_t i = 0; i < size; ++i) *At(address + i) = 0;
  }
  void Write16(uint32_t address, uint16_t value) override {
    *At(address) = uint8_t(value >> 8);
    *At(address + 1) = uint8_t(value);
  }
  void Write32(uint32_t address, uint32_t value) override {
    for (int i = 0; i < 4; ++i) *At(address + i) = uint8_t(value >> (24 - 8 * i));
  }
  uint16_t Read16(uint32_t address) { return uint16_t(*At(address) << 8 | *At(address + 1)); }
};

// Zumba Fitness Core's request 0 (82A6E510) and its buffers.
constexpr uint32_t kRequest = 0x82A6E510;
constexpr uint32_t kBuffers[5] = {0x82B0D630, 0x82B0B630, 0x82B0BA30, 0x82B0BE30, 0x82B0C230};
constexpr uint32_t kSizes[5] = {0x200, 0x400, 0x400, 0x400, 0x400};
static void PutRequest(FakeMemory& m) {
  m.Map(0x82A6E000, 0x1000, 0);
  m.Map(0x82B0B000, 0x3000);
  m.Write32(kRequest, 0x68);
  for (int i = 0; i < 5; ++i) {
    m.Write32(kRequest + 4 + 4 * i, kBuffers[i]);
    m.Write32(kRequest + 0x18 + 4 * i, kSizes[i]);
  }
  m.Write32(kRequest + 0x64, 0x20);
}

static void CheckClock() {
  CHECK(vm::TicAt(0) == 0);
  CHECK(vm::TicAt(1000000000ull) == 187);  // 187.5 a second
  CHECK(vm::TicAt(2000000000ull) == 375);
  for (uint32_t tic : {1u, 3u, 999u, 65535u, 65536u, 1000000u})
    CHECK(vm::TicAt(vm::TicStart(tic)) == tic && vm::TicAt(vm::TicStart(tic) - 1) == tic - 1);
  CHECK(vm::BlockEnds(3) && vm::BlockEnds(6) && !vm::BlockEnds(4) && !vm::BlockEnds(5));
  // The pump's rule is on the 16-bit tic: after the wrap 0x10002 is 2 (no), 0x10003 is 3 (yes).
  CHECK(!vm::BlockEnds(0x10002) && vm::BlockEnds(0x10003) && vm::BlockEnds(0x10000));
  CHECK(vm::BlockSequence(6) == 3 && vm::BlockSequence(0x10002) == 0xFFFF);
  CHECK(vm::SequenceDifference(6, 3) == 3 && vm::SequenceDifference(2, 0xFFFF) == 3);
  CHECK(vm::kSpeakerFrameBytes == 0x1800);
}

static void CheckRequest() {
  FakeMemory m;
  PutRequest(m);
  CHECK(vm::ValidRequest(m, kRequest));
  vm::CompleteSilent(m, kRequest, 0x1236);
  for (int i = 0; i < 5; ++i) {
    bool zero = true;
    for (uint32_t b = 0; b < kSizes[i]; ++b) zero = zero && *m.At(kBuffers[i] + b) == 0;
    CHECK(zero);
    uint32_t size = 0;
    CHECK(m.Read32(kRequest + 0x18 + 4 * i, &size) && size == kSizes[i]);
  }
  CHECK(*m.At(kBuffers[0] + 0x200) == 0xAB);  // nothing past the mono block
  CHECK(m.Read16(kRequest + 0x40) == 0x1236);
  uint32_t beam = 1;
  CHECK(m.Read32(kRequest + 0x44, &beam) && beam == 0 && m.Read32(kRequest + 0x48, &beam) &&
        beam == 0);
  uint32_t flags = 0;
  CHECK(m.Read32(kRequest + 0x64, &flags) && flags == 0x20);
  // Refused: another size, a buffer that cannot be written, a null or unmapped buffer, a
  // buffer too large, no request.
  FakeMemory bad = m;
  bad.Write32(kRequest, 0x60);
  CHECK(!vm::ValidRequest(bad, kRequest));
  bad = m;
  bad.read_only[0x82B0C000] = true;
  CHECK(!vm::ValidRequest(bad, kRequest));
  bad = m;
  bad.Write32(kRequest + 4, 0);
  CHECK(!vm::ValidRequest(bad, kRequest));
  bad = m;
  bad.Write32(kRequest + 4, 0x90000000);
  CHECK(!vm::ValidRequest(bad, kRequest));
  bad = m;
  bad.Write32(kRequest + 0x18, 0x20000);
  CHECK(!vm::ValidRequest(bad, kRequest));
  CHECK(!vm::ValidRequest(m, 0) && !vm::ValidRequest(m, 0x90000000));
}

static void CheckQueue() {
  vm::Queue q;
  CHECK(q.Submit({1, 10}) && q.Submit({2, 20}));
  vm::Request r;
  CHECK(!q.Take(4, &r) && !q.Take(5, &r));
  CHECK(q.Take(6, &r) && r.request == 1 && r.event == 10);
  CHECK(q.Take(9, &r) && r.request == 2);
  CHECK(!q.Take(12, &r));
  for (int i = 0; i < 8; ++i) CHECK(q.Submit({uint32_t(i), 0}));
  CHECK(!q.Submit({99, 0}) && q.size() == 8);
  q.Clear();
  CHECK(q.size() == 0);
}

// ---- A model of the NUI audio library's pump (Zumba 8254DDE8) ------------------------------
constexpr uint16_t kNone = 0xDEF1;
struct Pump {
  struct Slot {
    uint32_t address;
    bool done = false;
    uint16_t sequence = 0;
  };
  Slot slots[2] = {{0x82A6E510}, {0x82A6E578}};
  int index = 0;
  bool locked = false, any_done = false;
  uint16_t noted = kNone, previous = kNone;
  int resets = 0, blocks = 0, blocks_while_locked = 0;
  vm::Queue* queue;
  std::map<uint32_t, int> by_address;
  explicit Pump(vm::Queue* q) : queue(q) {
    by_address[slots[0].address] = 0;
    by_address[slots[1].address] = 1;
    queue->Submit({slots[0].address, 0});
    queue->Submit({slots[1].address, 0});
  }
  void Reset() {
    ++resets;
    locked = false;
    noted = kNone;
  }
  // Called once per tic, after the array completed what ends at this tic.
  void Run(uint32_t tic) {
    const uint16_t t16 = uint16_t(tic);
    if (!locked && any_done && noted == kNone && t16 % 3 == 0) noted = t16;
    Slot& s = slots[index];
    if (!s.done) return;
    s.done = false;
    any_done = true;
    ++blocks;
    bool error = previous != kNone && vm::SequenceDifference(s.sequence, previous) != 3;
    if (!error && noted != kNone && vm::SequenceDifference(s.sequence, noted) >= 0) {
      if (s.sequence != noted) {
        error = true;
      } else {
        locked = true;
        noted = kNone;
      }
    }
    if (error) Reset();
    if (locked) ++blocks_while_locked;
    previous = s.sequence;
    queue->Submit({s.address, 0});  // XamVoiceGetMicArrayAudioEx again
    index ^= 1;
  }
  void Complete(const vm::Request& r, uint16_t sequence) {
    Slot& s = slots[by_address[r.request]];
    s.done = true;
    s.sequence = sequence;
  }
};

// The pump thread's loop over the tics from `first` to `last` (skipping `skip` tics at `at`).
static void RunPump(Pump& pump, vm::Queue& q, uint32_t first, uint32_t last, uint32_t at = 0,
                    uint32_t skip = 0) {
  for (uint32_t tic = first; tic != last; ++tic) {
    if (skip && tic == at) tic += skip;  // a late thread
    vm::Request r;
    if (q.Take(tic, &r)) pump.Complete(r, vm::BlockSequence(tic));
    pump.Run(tic);
  }
}

static void CheckPump() {
  {
    vm::Queue q;
    Pump pump(&q);
    RunPump(pump, q, 1000, 1000 + 12);
    CHECK(pump.locked);
    RunPump(pump, q, 1012, 61000);
    CHECK(pump.resets == 0 && pump.locked);
    CHECK(pump.blocks >= 59988 / 3 - 1 && pump.blocks_while_locked >= pump.blocks - 4);
    CHECK(q.size() == 2);  // the pump hands each block's request straight back: both wait
  }
  {
    // Across the 16-bit wrap: at most one reset, locked again afterwards.
    vm::Queue q;
    Pump pump(&q);
    RunPump(pump, q, 65000, 66500);
    CHECK(pump.resets <= 1 && pump.locked);
  }
  {
    // A late pump thread (it skips 4 tics): a resynchronisation at most, then locked again.
    vm::Queue q;
    Pump pump(&q);
    RunPump(pump, q, 3000, 3600, 3300, 4);
    CHECK(pump.resets <= 1 && pump.locked);
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
  const std::string audio =
      ReadText(".deps/xenia-canary/src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc");
  const std::string voice = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_voice.cc");
  const std::string nui = ReadText(".deps/xenia-canary/src/xenia/kernel/xam/xam_nui.cc");
  if (audio.empty() || voice.empty() || nui.empty()) {
    std::printf("FAIL: no assembled tree here (xboxkrnl_audio.cc, xam_voice.cc, xam_nui.cc)\n");
    ++failures;
    return;
  }
  CHECK(Has(audio, "DEFINE_bool(kinect_microphone, true,"));
  CHECK(Has(audio, "#include \"xbox360ps5/virtual_mic.hpp\""));
  for (const char* name : {"XAudioRegisterRenderDriverMECClient", "XAudioUnregisterRenderDriverMECClient"})
    CHECK(Has(audio, (std::string("DECLARE_XBOXKRNL_EXPORT1(") + name + ", kAudio, kImplemented);").c_str()));
  for (const char* name : {"XAudioGetRenderDriverTic", "XAudioCaptureRenderDriverFrame"})
    CHECK(Has(audio, (std::string("DECLARE_XBOXKRNL_EXPORT2(") + name + ", kAudio, kImplemented, kHighFrequency);").c_str()));
  const std::string reg = Body(audio, "dword_result_t XAudioRegisterRenderDriverMECClient_entry(");
  CHECK(reg.find("if (!VirtualMicOn())") < reg.find("mec_running.store(true);"));
  CHECK(Has(reg, "return kNotConnected;") && Has(reg, "*handle_ptr = kMecHandle;"));
  const std::string pump = Body(audio, "void MecPumpMain(uint32_t generation) {");
  CHECK(Has(pump, "mic_requests.Take(tic, &request)") &&
        Has(pump, "vm::CompleteSilent(memory, request.request, vm::BlockSequence(tic));") &&
        Has(pump, "event->Set(0, false);"));
  CHECK(pump.find("CompleteSilent") < pump.find("Execute("));
  CHECK(Has(Body(audio, "dword_result_t XAudioCaptureRenderDriverFrame_entry("),
            "memory.Zero(buffer, vm::kSpeakerFrameBytes);"));
  CHECK(Has(Body(audio, "bool VirtualMicOn() {"),
            "cvars::kinect_microphone && xbox360ps5::motion::enabled.load() &&"));
  const std::string get = Body(voice, "dword_result_t XamVoiceGetMicArrayAudioEx_entry(");
  CHECK(get.find("xboxkrnl::VirtualMicSubmit(") < get.find("return kMicArrayNotConnected;"));
  CHECK(Has(get, "if (xboxkrnl::VirtualMicOn()) {"));
  const std::string underrun = Body(voice, "dword_result_t XamVoiceGetMicArrayUnderrunStatus_entry(");
  CHECK(Has(underrun, "*underrun = 0;") && Has(underrun, "*other = 0;"));
  CHECK(Has(voice, "DECLARE_XAM_EXPORT1(XamVoiceGetMicArrayUnderrunStatus, kNone, kStub);"));
  const std::string status = Body(nui, "dword_result_t XamNuiGetDeviceStatus_entry(");
  CHECK(Has(status, "if (site == xbox360ps5::nui_speech::Site::kRead && xboxkrnl::VirtualMicOn())"));
  CHECK(status.find("xboxkrnl::VirtualMicOn()") < status.find("SpeechStatusBits(site,"));
}

int main() {
  CheckClock();
  CheckRequest();
  CheckQueue();
  CheckPump();
  CheckTree();
  if (failures) {
    std::printf("check-virtual-mic: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-virtual-mic: ok\n");
  return 0;
}
