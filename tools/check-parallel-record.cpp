// SPDX-License-Identifier: MIT
// Fix 142 (xenia/gpu/vulkan/parallel_record.h, vulkan_command_processor.cc): the frame's command
// processing split over two threads, a prototype behind gpu_parallel_record (off by default). A
// decode thread walks the ring and the indirect buffers, applies predication, drops shader
// constant writes that leave the constant unchanged and hands ops in order to the GPU command
// thread, which executes them with the handlers it had.
//  (a) parallel-record.txt: mode, fifo, filter and sibling lines, comments, bounds, lines not
//      understood; the defaults (mode 0: off);
//  (b) the FIFO of ops: ops in order, wraps at the end, room (RoomNeeded), retired only at the end
//      of an executed op (never between a wrap and the op after it), a producer and a consumer
//      on two threads;
//  (c) a reference model of the PM4 interpreter (its own reader and packets: type 0/1/2, bad
//      packets, overflow, predication, indirect buffers, register and constant packets,
//      LOAD_ALU_CONSTANT, REG_RMW, COND_WRITE, memory writes, WAIT_REG_MEM, draws and their
//      failures, shaders, swaps, unknown opcodes ending a buffer) against the decoder and the ops
//      executed by the same model: the same guest-visible events in the same order (memory
//      writes, scratch registers, draws with the whole register file, waits with the value they
//      saw, swaps, the read pointer's write-back) and the same registers and memory at the end,
//      with the filter on and off, decoded and executed on one thread (mode 2) and on two (mode 1),
//      for random streams over a wrapping ring;
//  (d) the ordering rules: a WAIT_REG_MEM sees what the stream wrote before it; an indirect
//      buffer that a MEM_WRITE before it changes is read after the write (an ib-write
//      serialization), one it does not change is not waited for;
//  (e) the filter: unchanged constants dropped, everything else kept, the shadow forgotten after
//      packets that may write constants behind the decoder;
//  (f) the 30 s line, and "off" in mode 0;
//  (g) the assembled tree: the hook in WorkerThreadMain (before the ring loop, which is
//      unchanged), RunParallelRecord returning false for mode 0 before it makes anything, the
//      cvars' defaults, the ops' execution, the decode thread's name, the line after fix 134's;
//  (h) the off path: the fix's patch only adds lines (argv[1]: the fixes folder), and the only
//      lines it adds to command_processor.cc are the hook.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xenia/gpu/vulkan/parallel_record.h"
#include "xbox360ps5/frame_cost.hpp"

namespace {

namespace pr = xe::gpu::parallel_record;
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
uint32_t Be(uint32_t v) { return __builtin_bswap32(v); }

// (a)
void CheckParse() {
  pr::Settings s;
  Expect(s.mode == pr::kOff && s.fifo_kib == 4096 && s.filter == 1 && s.sibling == 0,
         "the defaults: off, 4096 KiB, filter on, shared CPUs");
  Expect(std::string(pr::ModeName(0)) == "off", "mode 0 is named off");
  Expect(pr::Parse("", s) == 0 && s == pr::Settings(), "an empty file changes nothing");
  Expect(pr::Parse("mode 1\nfifo 1024\nfilter 0\nsibling 1\n", s) == 0 && s.mode == 1 &&
             s.fifo_kib == 1024 && s.filter == 0 && s.sibling == 1,
         "mode, fifo, filter, sibling");
  pr::Settings t;
  Expect(pr::Parse("# comment\n  mode 2   # inline\n\n", t) == 0 && t.mode == 2,
         "comments and spaces");
  pr::Settings u;
  Expect(pr::Parse("mode 3\nfifo 100\nfifo 70000\nfilter 2\nsibling -1\nmode\nmode 1 2\nchunk 4\n",
                   u) == 8 &&
             u == pr::Settings(),
         "out of range, missing or extra values and unknown keys are not understood");
}

// (b)
void CheckRing() {
  pr::OpRing ring(1);  // rounded up to 256 KiB
  Expect(ring.capacity() == 256 * 1024, "the FIFO's smallest size");
  // Fill close to the end, then an op that does not fit there wraps.
  uint32_t written = 0;
  while (ring.write_position() + pr::OpSpan(8000) <= ring.capacity()) {
    Expect(ring.RoomNeeded(8000) == 0, "room while not full");
    pr::OpHeader* op = ring.Begin(pr::kOpRegisters, 8000, written);
    std::memset(ring.Payload(op), int(written & 0xFF), 8000);
    ring.Grow(8000);
    ring.Commit();
    ++written;
  }
  ring.Publish();
  Expect(ring.RoomNeeded(8000) > 0, "full: room needed");
  // Consume half; the next op wraps.
  uint32_t read = 0;
  while (read < written / 2) {
    const pr::OpHeader* op = ring.Front();
    Expect(op && op->kind == pr::kOpRegisters && op->arg == read && op->bytes == 8000 &&
               ring.Payload(op)[7999] == uint8_t(read & 0xFF),
           "ops in order with their payload");
    ring.Pop();
    ++read;
  }
  Expect(ring.RoomNeeded(8000) == 0, "room once retired");
  pr::OpRing ring2(256 * 1024);
  uint64_t pos = 0;
  for (int n = 0; n < 31; ++n) {
    ring2.Begin(pr::kOpPackets, 8192, n);
    ring2.Grow(8192);
    ring2.Commit();
  }
  ring2.Publish();
  for (int n = 0; n < 30; ++n) {
    ring2.Front();
    ring2.Pop();
  }
  pos = ring2.write_position();
  Expect(ring2.RoomNeeded(8192) == 0, "room after retiring");
  ring2.Begin(pr::kOpPackets, 8192, 77);  // wraps: the end has less than 8208 bytes
  ring2.Grow(8192);
  ring2.Commit();
  Expect(ring2.write_position() > pos + pr::OpSpan(8192), "the wrap takes the rest of the buffer");
  ring2.Publish();
  const pr::OpHeader* last = ring2.Front();  // the 31st
  Expect(last && last->arg == 30, "the op before the wrap");
  ring2.Pop();
  const uint64_t before_wrap = ring2.retired();
  const pr::OpHeader* after = ring2.Front();  // takes the wrap
  Expect(after && after->arg == 77, "the op after the wrap");
  Expect(ring2.retired() == before_wrap, "a wrap alone does not move retired");
  ring2.Pop();
  Expect(ring2.retired() == ring2.write_position(), "retired at the end of the op after the wrap");

  // Two threads: 200,000 ops of varying size through a 256 KiB FIFO.
  pr::OpRing shared(256 * 1024);
  std::atomic<bool> done{false};
  uint64_t bad = 0, seen = 0;
  std::thread consumer([&] {
    uint32_t expect = 0;
    for (;;) {
      const pr::OpHeader* op = shared.Front();
      if (!op) {
        if (done.load() && !shared.Front()) break;
        shared.WaitForOps(std::chrono::microseconds(200), 100);
        continue;
      }
      const uint8_t* p = shared.Payload(op);
      if (op->arg != expect || op->bytes != (expect % 97) * 8 ||
          (op->bytes && p[op->bytes - 1] != uint8_t(expect)))
        ++bad;
      ++expect;
      ++seen;
      shared.Pop();
    }
  });
  for (uint32_t n = 0; n < 200000; ++n) {
    const uint32_t bytes = (n % 97) * 8;
    while (const uint64_t need = shared.RoomNeeded(bytes)) {
      shared.Publish();
      shared.WaitRetired(shared.retired() + need, [] { return false; });
    }
    pr::OpHeader* op = shared.Begin(pr::kOpRegisters, bytes, n);
    std::memset(shared.Payload(op), int(n & 0xFF), bytes);
    shared.Grow(bytes);
    shared.Commit();
    if (n % 7 == 0) shared.Publish();
  }
  shared.Publish();
  shared.WaitRetired(shared.write_position(), [] { return false; });
  done = true;
  shared.Wake();
  consumer.join();
  Expect(bad == 0 && seen == 200000, "two threads: every op once, in order, intact");
}

// ---------------------------------------------------------------------------------------------
// (c) The reference model.
constexpr uint32_t kMemory = 4 * 1024 * 1024;
constexpr uint32_t kRegisters = 0x5003;
constexpr uint32_t kRing = 0x10000, kRingBytes = 0x8000;  // the ring
constexpr uint32_t kIbArea = 0x40000;                     // indirect buffers
constexpr uint32_t kWriteArea = 0x200000;                 // MEM_WRITE and fences
constexpr uint32_t kConstArea = 0x300000;                 // LOAD_ALU_CONSTANT's source
constexpr uint32_t kShaderArea = 0x380000;
constexpr uint32_t kScratchFirst = 0x578;

struct Model {
  std::vector<uint8_t> memory = std::vector<uint8_t>(kMemory, 0);
  std::vector<uint32_t> regs = std::vector<uint32_t>(kRegisters, 0);
  uint64_t bin_mask = 0xFFFFFFFFull, bin_select = 0xFFFFFFFFull;
  std::vector<std::string> events;

  uint32_t Load(uint32_t address) const {
    address &= 0x1FFFFFFF;
    if (address + 4 > kMemory) return 0;
    uint32_t v;
    std::memcpy(&v, &memory[address], 4);
    return Be(v);
  }
  void Store(uint32_t address, uint32_t value) {
    address &= 0x1FFFFFFF;
    if (address + 4 > kMemory) return;
    const uint32_t raw = Be(value);
    std::memcpy(&memory[address], &raw, 4);
    events.push_back("mem " + std::to_string(address) + "=" + std::to_string(value));
  }
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisters) return;
    regs[index] = value;
    if (index >= kScratchFirst && index < kScratchFirst + 8) {
      events.push_back("scratch " + std::to_string(index) + "=" + std::to_string(value));
    }
  }
  uint64_t Hash() const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : regs) h = (h ^ v) * 1099511628211ull;
    return h;
  }

  // The model's own reader (RingBuffer's read_count semantics).
  struct Reader {
    const uint8_t* base;
    uint32_t capacity, offset, remaining;
    uint32_t Read() {
      uint32_t v;
      std::memcpy(&v, base + offset, 4);
      offset = (offset + 4) % capacity;
      remaining -= 4;
      return Be(v);
    }
    void Skip(uint32_t dwords) {
      offset = (offset + dwords * 4) % capacity;
      remaining -= dwords * 4;
    }
  };

  bool Draw(Reader& r, uint32_t n) {
    if (!n) return false;
    const uint32_t initiator = r.Read();
    --n;
    regs[0x21FC] = initiator;
    if (((initiator >> 6) & 3) == 0) {
      if (!n) return false;
      const uint32_t base = r.Read();
      --n;
      if (!n) return false;
      const uint32_t size = r.Read();
      --n;
      regs[0x21FA] = base;
      regs[0x21FB] = size;
    }
    r.Skip(n);
    events.push_back("draw " + std::to_string(initiator) + " " + std::to_string(Hash()));
    return true;
  }

  bool Packet(Reader& r) {
    const uint32_t packet = r.Read();
    if (!packet || packet == 0x0BADF00D) return true;
    const uint32_t type = packet >> 30;
    if (type == 2) return true;
    if (type == 1) {
      const uint32_t a = r.Read(), b = r.Read();
      WriteRegister(packet & 0x7FF, a);
      WriteRegister((packet >> 11) & 0x7FF, b);
      return true;
    }
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (r.remaining < count * 4) return false;
    if (type == 0) {
      const uint32_t base = packet & 0x7FFF;
      const bool one = (packet >> 15) & 1;
      for (uint32_t i = 0; i < count; ++i) WriteRegister(one ? base : base + i, r.Read());
      return true;
    }
    const uint32_t opcode = (packet >> 8) & 0x7F;
    if (packet & 1) {
      if (!(bin_select & bin_mask) || opcode == pr::pm4::kXeSwap) {
        r.Skip(count);
        return true;
      }
    }
    switch (opcode) {
      case pr::pm4::kIndirectBuffer:
      case pr::pm4::kIndirectBufferPfd: {
        const uint32_t ptr = r.Read() & 0x1FFFFFFF;
        const uint32_t length = r.Read() & 0xFFFFF;
        if (length) {
          Reader ib{&memory[ptr], length * 4, 0, length * 4};
          do {
            if (!Packet(ib)) break;
          } while (ib.remaining);
        }
        return true;
      }
      case pr::pm4::kWaitRegMem: {
        const uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read();
        r.Read();
        const uint32_t value = (info & 0x10) ? Load(poll & ~3u) : regs[poll % kRegisters];
        events.push_back("wait " + std::to_string(value) + ((value & mask) == ref ? " met" : " unmet"));
        return true;
      }
      case pr::pm4::kRegRmw: {
        const uint32_t info = r.Read(), and_mask = r.Read(), or_mask = r.Read();
        const uint32_t index = info & 0x1FFF;
        WriteRegister(index, (regs[index] & and_mask) | or_mask);
        return true;
      }
      case pr::pm4::kRegToMem: {
        const uint32_t reg = r.Read() & 0x1FFF, address = r.Read();
        Store(address, regs[reg]);
        return true;
      }
      case pr::pm4::kMemWrite: {
        const uint32_t address = r.Read();
        for (uint32_t i = 1; i < count; ++i) Store(address + (i - 1) * 4, r.Read());
        return true;
      }
      case pr::pm4::kCondWrite: {
        const uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read();
        const uint32_t write = r.Read(), data = r.Read();
        const uint32_t value = (info & 0x10) ? Load(poll) : regs[poll % kRegisters];
        if ((value & mask) == ref) {
          if (info & 0x100) Store(write, data); else WriteRegister(write % kRegisters, data);
        }
        return true;
      }
      case pr::pm4::kEventWrite:
        events.push_back("event " + std::to_string(r.Read()));
        r.Skip(count - 1);
        return true;
      case pr::pm4::kEventWriteShd: {
        r.Read();
        const uint32_t address = r.Read(), value = r.Read();
        Store(address, value);
        return true;
      }
      case pr::pm4::kEventWriteExt: {
        r.Read();
        const uint32_t address = r.Read();
        Store(address, 1);
        Store(address + 4, 2);
        Store(address + 8, 3);
        r.Skip(count - 2);
        return true;
      }
      case pr::pm4::kEventWriteZpd:
        events.push_back("zpd");
        r.Skip(count);
        return true;
      case pr::pm4::kDrawIndx: {
        if (count < 1) return false;
        r.Read();  // VIZ token
        return Draw(r, count - 1);
      }
      case pr::pm4::kDrawIndx2:
        return Draw(r, count);
      case pr::pm4::kSetConstant: {
        const uint32_t offset_type = r.Read();
        const uint32_t type = (offset_type >> 16) & 0xFF;
        static const uint32_t bases[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
        if (type > 4) {
          r.Skip(count - 1);
          return true;
        }
        const uint32_t base = bases[type] + (offset_type & 0x7FF);
        for (uint32_t i = 0; i + 1 < count; ++i) WriteRegister(base + i, r.Read());
        return true;
      }
      case pr::pm4::kSetConstant2:
      case pr::pm4::kSetShaderConstants: {
        const uint32_t base = r.Read() & 0xFFFF;
        for (uint32_t i = 0; i + 1 < count; ++i) WriteRegister(base + i, r.Read());
        return true;
      }
      case pr::pm4::kLoadAluConstant: {
        const uint32_t address = r.Read() & 0x3FFFFFFF, offset_type = r.Read();
        const uint32_t size = r.Read() & 0xFFF;
        const uint32_t type = (offset_type >> 16) & 0xFF;
        static const uint32_t bases[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
        if (type > 4) return true;
        for (uint32_t i = 0; i < size; ++i)
          WriteRegister(bases[type] + (offset_type & 0x7FF) + i, Load(address + i * 4));
        return true;
      }
      case pr::pm4::kImLoad: {
        const uint32_t addr_type = r.Read(), start_size = r.Read();
        uint64_t h = 0;
        for (uint32_t i = 0; i < (start_size & 0xFFFF); ++i) h = h * 31 + Load((addr_type & ~3u) + i * 4);
        events.push_back("shader " + std::to_string(addr_type & 3) + " " + std::to_string(h));
        return (addr_type & 3) <= 1;
      }
      case pr::pm4::kImLoadImmediate: {
        const uint32_t type = r.Read(), start_size = r.Read();
        uint64_t h = 0;
        for (uint32_t i = 0; i < (start_size & 0xFFFF); ++i) h = h * 31 + r.Read();
        events.push_back("shader " + std::to_string(type) + " " + std::to_string(h));
        return type <= 1;
      }
      case pr::pm4::kXeSwap:
        events.push_back("swap");
        r.Skip(count);
        return true;
      case pr::pm4::kInterrupt:
        events.push_back("interrupt " + std::to_string(r.Read()));
        return true;
      case pr::pm4::kSetBinMaskLo:
        bin_mask = (bin_mask & 0xFFFFFFFF00000000ull) | r.Read();
        return true;
      case pr::pm4::kSetBinMaskHi:
        bin_mask = (bin_mask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
        return true;
      case pr::pm4::kSetBinSelectLo:
        bin_select = (bin_select & 0xFFFFFFFF00000000ull) | r.Read();
        return true;
      case pr::pm4::kSetBinSelectHi:
        bin_select = (bin_select & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
        return true;
      case pr::pm4::kSetBinMask: {
        const uint64_t hi = r.Read(), lo = r.Read();
        bin_mask = (hi << 32) | lo;
        return true;
      }
      case pr::pm4::kSetBinSelect: {
        const uint64_t hi = r.Read(), lo = r.Read();
        bin_select = (hi << 32) | lo;
        return true;
      }
      case pr::pm4::kNop:
      case pr::pm4::kMeInit:
      case pr::pm4::kWaitIbPfdComplete:
        r.Skip(count);
        return true;
      case pr::pm4::kInvalidateState:
      case pr::pm4::kContextUpdate:
      case pr::pm4::kWaitForIdle:
        r.Read();
        return true;
      default:
        r.Skip(count);  // HitUnimplementedOpcode
        events.push_back("unknown " + std::to_string(opcode));
        return false;
    }
  }

  // ExecutePrimaryBuffer and the write-back after it.
  void Primary(uint32_t read_index, uint32_t write_index) {
    events.push_back("begin");
    const uint32_t offset = read_index * 4 % kRingBytes, end = write_index * 4 % kRingBytes;
    Reader r{&memory[kRing], kRingBytes, offset,
             offset <= end ? end - offset : kRingBytes - offset + end};
    do {
      if (!Packet(r)) break;
    } while (r.remaining);
    events.push_back("end");
    events.push_back("rptr " + std::to_string(write_index));
  }

  // The GPU command thread's side of the split: an op.
  void Op(const pr::OpHeader& op, const uint8_t* payload) {
    switch (op.kind) {
      case pr::kOpPackets: {
        Reader r{payload, op.bytes, 0, op.bytes};
        do {
          if (!Packet(r)) break;
        } while (r.remaining);
      } break;
      case pr::kOpRegisters:
        for (uint32_t i = 0; i < op.bytes / 4; ++i) {
          uint32_t raw;
          std::memcpy(&raw, payload + i * 4, 4);
          WriteRegister(op.arg + i, Be(raw));
        }
        break;
      case pr::kOpPrimaryBegin:
        events.push_back("begin");
        break;
      case pr::kOpPrimaryEnd:
        events.push_back("end");
        break;
      case pr::kOpReadPointer:
        events.push_back("rptr " + std::to_string(op.arg));
        break;
      default:
        events.push_back("bad op");
    }
  }
};

// The PM4 stream builder.
struct Stream {
  std::vector<uint32_t> dwords;  // host values
  void Type0(uint32_t base, const std::vector<uint32_t>& values, bool one = false) {
    dwords.push_back((uint32_t(values.size() - 1) << 16) | (one ? 0x8000 : 0) | base);
    dwords.insert(dwords.end(), values.begin(), values.end());
  }
  void Type3(uint32_t opcode, const std::vector<uint32_t>& data, bool predicated = false) {
    dwords.push_back(0xC0000000u | (uint32_t(data.size() - 1) << 16) | (opcode << 8) |
                     (predicated ? 1 : 0));
    dwords.insert(dwords.end(), data.begin(), data.end());
  }
};

class TestHost : public pr::Host {
 public:
  TestHost(Model& model, pr::OpRing& ring, bool threaded) : model_(model), ring_(ring), threaded_(threaded) {}
  const uint8_t* Translate(uint32_t address) override { return &model_.memory[address & 0x1FFFFFFF]; }
  void Drain(uint32_t) override {
    if (threaded_) {
      ring_.WaitRetired(ring_.published(), [] { return false; });
    } else {
      ExecuteAll();
    }
  }
  bool MakeRoom(uint32_t payload_bytes) override {
    if (!threaded_) {
      ExecuteAll();
      return true;
    }
    while (const uint64_t need = ring_.RoomNeeded(payload_bytes)) {
      ring_.WaitRetired(ring_.retired() + need, [] { return false; });
    }
    return true;
  }
  void ExecuteAll() {
    while (const pr::OpHeader* op = ring_.Front()) {
      model_.Op(*op, ring_.Payload(op));
      ring_.Pop();
    }
  }

 private:
  Model& model_;
  pr::OpRing& ring_;
  bool threaded_;
};

// A random stream: register runs (constants from small sets, so many are unchanged), constant
// packets, memory writes, fences, waits, draws, shaders, predication, indirect buffers (some
// changed by a MEM_WRITE just before them), bad packets and the occasional buffer end.
struct Generated {
  std::vector<uint32_t> ring_dwords;               // the ring's packets
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> ibs;  // address, dwords
  std::vector<uint32_t> parts;                     // dword counts of the primaries
};

// 32-bit random values (mt19937's are unsigned long here).
struct Rng {
  std::mt19937 engine;
  uint32_t operator()() { return uint32_t(engine()); }
};

uint32_t RandomConstantRegister(Rng& rng) {
  switch (rng() % 3) {
    case 0: return 0x4000 + rng() % 64;
    case 1: return 0x4800 + rng() % 24;
    default: return 0x4900 + rng() % 0x28;
  }
}

void RandomPacket(Rng& rng, Stream& s, Generated& g, uint32_t& ib_next, int depth,
                  bool allow_end) {
  const uint32_t pick = rng() % 100;
  auto small_value = [&] { return uint32_t(rng() % 4); };
  if (pick < 28) {
    // A run of constants, mostly unchanged values.
    const uint32_t base = RandomConstantRegister(rng);
    std::vector<uint32_t> v(1 + rng() % 12);
    for (auto& x : v) x = small_value();
    s.Type0(base, v);
  } else if (pick < 34) {
    // Other registers (some scratch registers, a guest-visible effect).
    const uint32_t base = (rng() % 4 == 0) ? kScratchFirst + rng() % 8 : 0x2000 + rng() % 0x200;
    std::vector<uint32_t> v(1 + rng() % 4);
    for (auto& x : v) x = rng() % 3;
    s.Type0(base, v);
  } else if (pick < 36) {
    s.Type0(RandomConstantRegister(rng), {small_value(), small_value()}, true);  // one register
  } else if (pick < 38) {
    s.dwords.push_back(0x40000000u | (0x100 + rng() % 0x10) | ((0x200 + rng() % 0x10) << 11));
    s.dwords.push_back(rng() % 5);
    s.dwords.push_back(rng() % 5);
  } else if (pick < 46) {
    const uint32_t type = rng() % 5;
    const uint32_t index = type == 0 ? rng() % 64 : type == 1 ? rng() % 24 : type == 4 ? 0x100 + rng() % 16 : rng() % 8;
    std::vector<uint32_t> data{(type << 16) | index};
    const uint32_t n = 1 + rng() % 6;
    for (uint32_t i = 0; i < n; ++i) data.push_back(small_value());
    s.Type3(pr::pm4::kSetConstant, data, rng() % 8 == 0);
  } else if (pick < 49) {
    std::vector<uint32_t> data{RandomConstantRegister(rng)};
    for (uint32_t i = 0; i < 1 + rng() % 4; ++i) data.push_back(small_value());
    s.Type3(rng() % 2 ? pr::pm4::kSetConstant2 : pr::pm4::kSetShaderConstants, data);
  } else if (pick < 52) {
    s.Type3(pr::pm4::kLoadAluConstant,
            {kConstArea + (rng() % 64) * 4, (uint32_t(rng() % 3) << 16) | (rng() % 16), 1 + rng() % 8});
  } else if (pick < 54) {
    s.Type3(pr::pm4::kRegRmw, {RandomConstantRegister(rng), 0xFFFFFFFEu, rng() % 2});
  } else if (pick < 56) {
    const bool memory = rng() % 2;
    s.Type3(pr::pm4::kCondWrite,
            {(memory ? 0x100u : 0u), 0x2000 + rng() % 8, 0, 0, memory ? kWriteArea + (rng() % 32) * 4 : RandomConstantRegister(rng), small_value()});
  } else if (pick < 60) {
    std::vector<uint32_t> data{kWriteArea + (rng() % 64) * 4};
    for (uint32_t i = 0; i < 1 + rng() % 3; ++i) data.push_back(rng() % 100);
    s.Type3(pr::pm4::kMemWrite, data);
  } else if (pick < 63) {
    s.Type3(pr::pm4::kEventWriteShd, {0, kWriteArea + (rng() % 64) * 4, rng() % 100});
  } else if (pick < 64) {
    s.Type3(pr::pm4::kEventWriteExt, {0, kWriteArea + 0x1000 + (rng() % 16) * 16});
  } else if (pick < 65) {
    s.Type3(pr::pm4::kRegToMem, {0x4000 + rng() % 16, kWriteArea + (rng() % 64) * 4});
  } else if (pick < 68) {
    // A wait on a fence the stream may have written.
    s.Type3(pr::pm4::kWaitRegMem, {0x13, kWriteArea + (rng() % 64) * 4, rng() % 100, 0xFFFFFFFFu, 1});
  } else if (pick < 79) {
    const bool dma = rng() % 2;
    const uint32_t initiator = 4 | (dma ? 0 : (2u << 6));
    if (rng() % 2) {
      std::vector<uint32_t> data{0, initiator};
      if (dma) { data.push_back(0x100000); data.push_back(16); }
      s.Type3(pr::pm4::kDrawIndx, data, rng() % 10 == 0);
    } else {
      std::vector<uint32_t> data{initiator | (1u << 6)};  // immediate indices
      for (uint32_t i = 0; i < rng() % 4; ++i) data.push_back(i);
      s.Type3(pr::pm4::kDrawIndx2, data);
    }
  } else if (pick < 81) {
    s.Type3(pr::pm4::kImLoad, {kShaderArea + (rng() % 8) * 64 + (rng() % 2), 4});
  } else if (pick < 82) {
    s.Type3(pr::pm4::kImLoadImmediate, {rng() % 2, 3, 11, 22, 33});
  } else if (pick < 85) {
    // Predication state.
    switch (rng() % 4) {
      case 0: s.Type3(pr::pm4::kSetBinMaskLo, {rng() % 2 ? 0u : 1u}); break;
      case 1: s.Type3(pr::pm4::kSetBinSelectLo, {rng() % 2 ? 0u : 3u}); break;
      case 2: s.Type3(pr::pm4::kSetBinMask, {0, 1}); break;
      default: s.Type3(pr::pm4::kSetBinSelect, {0, 1}); break;
    }
  } else if (pick < 91 && depth < 2) {
    // An indirect buffer, sometimes changed by a MEM_WRITE just before it.
    Stream ib;
    const uint32_t n = 1 + rng() % 12;
    for (uint32_t i = 0; i < n; ++i) RandomPacket(rng, ib, g, ib_next, depth + 1, true);
    // Its first packet a register run of one value, so the change below is one dword.
    Stream head;
    head.Type0(0x4000 + rng() % 8, {small_value()});
    ib.dwords.insert(ib.dwords.begin(), head.dwords.begin(), head.dwords.end());
    const uint32_t address = ib_next;
    ib_next += uint32_t(ib.dwords.size() * 4 + 64);
    g.ibs.push_back({address, ib.dwords});
    if (rng() % 4 == 0) s.Type3(pr::pm4::kMemWrite, {address + 4, 7 + rng() % 3});
    s.Type3(rng() % 2 ? pr::pm4::kIndirectBuffer : pr::pm4::kIndirectBufferPfd,
            {address, uint32_t(ib.dwords.size())}, rng() % 10 == 0);
  } else if (pick < 93) {
    s.Type3(pr::pm4::kXeSwap, {0, 0, 0, 0}, rng() % 8 == 0);
  } else if (pick < 94) {
    s.Type3(pr::pm4::kInterrupt, {1});
  } else if (pick < 95) {
    s.Type3(pr::pm4::kEventWrite, {rng() % 4});
  } else if (pick < 96) {
    s.dwords.push_back(rng() % 2 ? 0u : 0x0BADF00Du);  // bad packets
  } else if (pick < 97) {
    s.dwords.push_back(0x80000000u);  // type 2
  } else if (pick < 98) {
    s.Type3(pr::pm4::kNop, {1, 2, 3});
  } else if (pick < 99 && allow_end && rng() % 3 == 0) {
    switch (rng() % 3) {
      case 0: s.Type3(0x44, {0, 0}); break;                   // COND_EXEC: unknown, the buffer ends
      case 1: s.Type3(pr::pm4::kDrawIndx, {0}); break;        // too small: the buffer ends
      default: s.Type3(pr::pm4::kImLoad, {kShaderArea + 2, 4}); break;  // shader type 2: ends
    }
  } else {
    s.Type3(pr::pm4::kEventWriteZpd, {0});
  }
}

Generated Generate(uint32_t seed, uint32_t packets) {
  Rng rng{std::mt19937(seed)};
  Generated g;
  uint32_t ib_next = kIbArea;
  Stream ring;
  std::vector<uint32_t> ends;  // where each top-level packet ends
  while (ring.dwords.size() < packets) {
    RandomPacket(rng, ring, g, ib_next, 0, rng() % 50 == 0);
    ends.push_back(uint32_t(ring.dwords.size()));
    if (ring.dwords.size() > kRingBytes / 4 - 600) break;
  }
  g.ring_dwords = ring.dwords;
  // Primaries of random sizes, cut between packets (as the guest's write pointer moves).
  uint32_t done = 0;
  size_t next = 0;
  while (done < g.ring_dwords.size()) {
    const uint32_t want = done + 1 + rng() % 400;
    while (next + 1 < ends.size() && ends[next] < want) ++next;
    const uint32_t end = ends[next++];
    g.parts.push_back(end - done);
    done = end;
  }
  return g;
}

void Load(Model& model, const Generated& g, uint32_t start_index) {
  for (uint32_t i = 0; i < g.ring_dwords.size(); ++i) {
    const uint32_t raw = Be(g.ring_dwords[i]);
    std::memcpy(&model.memory[kRing + ((start_index + i) * 4) % kRingBytes], &raw, 4);
  }
  for (const auto& ib : g.ibs) {
    for (uint32_t i = 0; i < ib.second.size(); ++i) {
      const uint32_t raw = Be(ib.second[i]);
      std::memcpy(&model.memory[ib.first + i * 4], &raw, 4);
    }
  }
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t raw = Be(i % 3);
    std::memcpy(&model.memory[kConstArea + i * 4], &raw, 4);
  }
}

struct RunResult {
  std::vector<std::string> events;
  std::vector<uint32_t> regs;
  std::vector<uint8_t> memory;
  pr::DecodeStats stats;
};

RunResult RunReference(const Generated& g, uint32_t start_index) {
  Model model;
  Load(model, g, start_index);
  uint32_t read = start_index;
  for (uint32_t part : g.parts) {
    const uint32_t write = (read + part) % (kRingBytes / 4);
    model.Primary(read, write);
    read = write;
  }
  return {model.events, model.regs, model.memory, {}};
}

RunResult RunSplit(const Generated& g, uint32_t start_index, bool filter, bool threaded) {
  Model model;
  Load(model, g, start_index);
  pr::OpRing ring(256 * 1024);
  TestHost host(model, ring, threaded);
  pr::Decoder decoder(ring, host, filter, model.bin_mask, model.bin_select);
  std::atomic<bool> done{false};
  std::thread consumer;
  if (threaded) {
    consumer = std::thread([&] {
      for (;;) {
        const pr::OpHeader* op = ring.Front();
        if (!op) {
          if (done.load() && !ring.Front()) break;
          ring.WaitForOps(std::chrono::microseconds(100), 50);
          continue;
        }
        model.Op(*op, ring.Payload(op));
        ring.Pop();
      }
    });
  }
  uint32_t read = start_index;
  for (uint32_t part : g.parts) {
    const uint32_t write = (read + part) % (kRingBytes / 4);
    decoder.DecodePrimary(&model.memory[kRing], kRingBytes, read, write);
    if (!threaded) host.ExecuteAll();
    read = write;
  }
  decoder.Publish();
  if (threaded) {
    ring.WaitRetired(ring.published(), [] { return false; });
    done = true;
    ring.Wake();
    consumer.join();
  }
  return {model.events, model.regs, model.memory, decoder.stats()};
}

void CheckEquivalence() {
  uint64_t dropped = 0, written = 0, ib_writes = 0, waits = 0, predicated = 0, failed = 0;
  int mismatches = 0;
  for (uint32_t seed = 1; seed <= 160; ++seed) {
    const Generated g = Generate(seed, 600 + (seed % 7) * 500);
    const uint32_t start = (kRingBytes / 4) - (seed * 37) % 3000;  // wraps the ring
    const RunResult reference = RunReference(g, start);
    for (int variant = 0; variant < 3; ++variant) {
      const bool filter = variant != 1;
      const bool threaded = variant == 2;
      const RunResult split = RunSplit(g, start, filter, threaded);
      const bool same = split.events == reference.events && split.regs == reference.regs &&
                        split.memory == reference.memory;
      if (!same && mismatches++ < 3) {
        size_t at = 0;
        while (at < split.events.size() && at < reference.events.size() &&
               split.events[at] == reference.events[at])
          ++at;
        std::printf("seed %u variant %d: events differ at %zu of %zu/%zu (%s vs %s)\n", seed, variant,
                    at, split.events.size(), reference.events.size(),
                    at < split.events.size() ? split.events[at].c_str() : "-",
                    at < reference.events.size() ? reference.events[at].c_str() : "-");
      }
      Expect(same, "seed " + std::to_string(seed) + " variant " + std::to_string(variant) +
                       ": the split does what the interpreter did");
      if (variant == 0) {
        dropped += split.stats.registers_dropped;
        written += split.stats.registers_written;
        ib_writes += split.stats.serialize[pr::kSerializeIbWrite];
        waits += split.stats.serialize[pr::kSerializeWait];
        predicated += split.stats.predicated_out;
        failed += split.stats.failed;
      }
    }
  }
  std::printf("equivalence: %llu of %llu register writes dropped unchanged, %llu waits, %llu ib-write "
              "serializations, %llu predicated out, %llu buffers ended early\n",
              (unsigned long long)dropped, (unsigned long long)written, (unsigned long long)waits,
              (unsigned long long)ib_writes, (unsigned long long)predicated,
              (unsigned long long)failed);
  Expect(dropped > written / 50, "the generated streams have many unchanged constants");
  Expect(waits > 0 && ib_writes > 0 && predicated > 0 && failed > 0,
         "the streams covered waits, ib-write serializations, predication and buffer ends");
}

// (d) The ordering rules, alone, in mode 2 (where only the drains execute anything early).
void CheckOrdering() {
  // WAIT_REG_MEM after a fence written by the stream: it sees the fence.
  {
    Stream s;
    s.Type3(pr::pm4::kEventWriteShd, {0, kWriteArea, 42});
    s.Type3(pr::pm4::kWaitRegMem, {0x13, kWriteArea, 42, 0xFFFFFFFFu, 1});
    Generated g;
    g.ring_dwords = s.dwords;
    g.parts = {uint32_t(s.dwords.size())};
    const RunResult split = RunSplit(g, 0, true, false);
    bool met = false;
    for (const auto& e : split.events) met |= e == "wait 42 met";
    Expect(met && split.stats.serialize[pr::kSerializeWait] == 1,
           "a WAIT_REG_MEM sees the fence the stream wrote before it (one wait serialization)");
  }
  // An indirect buffer changed by a MEM_WRITE before it, and one that is not.
  for (int changed = 0; changed < 2; ++changed) {
    Stream ib;
    ib.Type0(0x4001, {5});
    ib.Type3(pr::pm4::kDrawIndx2, {4u | (2u << 6)});
    Stream s;
    const uint32_t address = kIbArea;
    s.Type3(pr::pm4::kMemWrite, {changed ? address + 4 : kWriteArea, 9});
    s.Type3(pr::pm4::kIndirectBuffer, {address, uint32_t(ib.dwords.size())});
    Generated g;
    g.ring_dwords = s.dwords;
    g.ibs.push_back({address, ib.dwords});
    g.parts = {uint32_t(s.dwords.size())};
    const RunResult reference = RunReference(g, 0);
    const RunResult split = RunSplit(g, 0, true, false);
    Expect(split.regs[0x4001] == (changed ? 9u : 5u) && split.regs == reference.regs &&
               split.events == reference.events,
           changed ? "an indirect buffer is read after the MEM_WRITE that changes it"
                   : "an indirect buffer the MEM_WRITE does not touch");
    Expect(split.stats.serialize[pr::kSerializeIbWrite] == uint64_t(changed),
           changed ? "one ib-write serialization" : "no ib-write serialization");
  }
}

// (e) The filter alone.
void CheckFilter() {
  Stream s;
  s.Type0(0x4000, {1, 2, 3});
  s.Type0(0x4000, {1, 2, 3});              // all unchanged: dropped
  s.Type0(0x4001, {2, 9});                 // 2 unchanged, 9 new
  s.Type0(kScratchFirst, {1});             // scratch: always kept
  s.Type0(kScratchFirst, {1});
  s.Type3(pr::pm4::kLoadAluConstant, {kConstArea, 0, 2});  // forgets 0x4000 and 0x4001
  s.Type0(0x4000, {1});                    // kept: the shadow forgot it
  s.Type3(pr::pm4::kRegRmw, {0x4002, 0xFFFFFFFFu, 0});    // forgets them all
  s.Type0(0x4002, {9});                    // kept
  Generated g;
  g.ring_dwords = s.dwords;
  g.parts = {uint32_t(s.dwords.size())};
  const RunResult reference = RunReference(g, 0);
  const RunResult split = RunSplit(g, 0, true, false);
  Expect(split.regs == reference.regs && split.events == reference.events,
         "the filter's stream does what the interpreter did");
  Expect(split.stats.registers_written == 12 && split.stats.registers_dropped == 4,
         "dropped: the three unchanged constants and the unchanged one of the next run");
  const RunResult unfiltered = RunSplit(g, 0, false, false);
  Expect(unfiltered.stats.registers_dropped == 0 && unfiltered.regs == reference.regs,
         "filter 0 drops nothing");
}

// (f)
void CheckLine() {
  namespace fc = xbox360ps5::frame_cost;
  fc::ParallelRecordReporter reporter;
  fc::parallel_mode_now.store(0);
  Expect(reporter.Line(30, 1500) == "Frame time parallel record (fix 142): off", "the line when off");
  fc::parallel_mode_now.store(1);
  fc::parallel_fifo_kib_now.store(4096);
  fc::parallel_filter_now.store(1);
  fc::parallel_decode_ns.fetch_add(9000000000ull);   // 300 ms/s over 30 s
  fc::parallel_execute_ns.fetch_add(21000000000ull); // 700 ms/s
  fc::parallel_draws.fetch_add(1500 * 4400);
  fc::parallel_registers.fetch_add(30 * 1000000);
  fc::parallel_dropped.fetch_add(30 * 250000);
  fc::parallel_serialize[0].fetch_add(1500);
  fc::parallel_serialize[2].fetch_add(3000);
  fc::parallel_ahead_sum.fetch_add(10 * 65536);
  fc::parallel_ahead_samples.fetch_add(10);
  fc::parallel_ahead_max.store(256 * 1024);
  const std::string line = reporter.Line(30, 1500);
  Expect(line.find("Frame time parallel record (fix 142, mode 1 (a decode thread), FIFO 4096 KiB, "
                   "filter 1): decode 300.0 ms/s") == 0,
         "the line's start and the decode time: " + line);
  Expect(line.find("GPU thread executing 700.0 ms/s") != std::string::npos, "executing");
  Expect(line.find("draws 4400") != std::string::npos, "draws per frame");
  Expect(line.find("registers 1000000/s, dropped unchanged 250000/s (25.0%)") != std::string::npos,
         "registers dropped");
  Expect(line.find("serialized per frame: wait 1.00, room 0.00, ib-write 2.00") != std::string::npos,
         "serializations by reason");
  Expect(line.find("ahead avg 64.0 KiB, max 256.0 KiB") != std::string::npos, "how far ahead");
  const std::string next = reporter.Line(30, 1500);
  Expect(next.find("decode 0.0 ms/s") != std::string::npos && next.find("max 0.0 KiB") != std::string::npos,
         "the next line counts from the last");
  fc::parallel_mode_now.store(0);
}

// (g)
void CheckTree() {
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/command_processor.cc");
  const std::string cph = Read(".deps/xenia-canary/src/xenia/gpu/command_processor.h");
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string vkh = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  const std::string fc = Read("include/xbox360ps5/frame_cost.hpp");
  const std::string main = Read("src/canary_game_main.cpp");
  const std::string place = Read("platform/ps5/thread_place.cpp");
  Expect(cph.find("virtual bool RunParallelRecord() { return false; }") != std::string::npos,
         "the hook's default is false");
  const size_t worker = cp.find("void CommandProcessor::WorkerThreadMain() {");
  const size_t name = cp.find("xbox360ps5::hang_watch::NameThread(\"gpu\");", worker);
  const size_t hook = cp.find("  if (RunParallelRecord()) {\n    ShutdownContext();\n    return;\n  }\n", worker);
  const size_t loop = cp.find("  while (worker_running_) {", worker);
  Expect(worker != std::string::npos && name < hook && hook < loop && loop != std::string::npos,
         "WorkerThreadMain: the hook after SetupContext and the hang watch's name, before the ring loop");
  Expect(cp.find("read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);", loop) !=
             std::string::npos,
         "the ring loop still executes the primary buffer itself");
  Expect(vk.find("DEFINE_uint32(gpu_parallel_record, 0,") != std::string::npos &&
             vk.find("DEFINE_uint32(gpu_parallel_record_fifo_kib, 4096,") != std::string::npos &&
             vk.find("DEFINE_bool(gpu_parallel_record_filter, true,") != std::string::npos &&
             vk.find("DEFINE_bool(gpu_parallel_record_sibling, false,") != std::string::npos,
         "the cvars and their defaults (off)");
  const size_t run = vk.find("bool VulkanCommandProcessor::RunParallelRecord() {");
  const size_t off = vk.find("  if (settings.mode == parallel_record::kOff) {\n    return false;\n  }", run);
  const size_t first_effect = std::min({vk.find("parallel_settings_ = settings;", run),
                                        vk.find("XELOGW(", run), vk.find("parallel_ring_ =", run),
                                        vk.find(".store(", run)});
  Expect(run != std::string::npos && off != std::string::npos && off < first_effect,
         "mode 0 returns false before anything is set, logged or made");
  Expect(vk.find("parallel_front_thread_->set_name(\"GPU Decode\");") != std::string::npos,
         "the decode thread's name");
  const size_t exec = vk.find("void VulkanCommandProcessor::ExecuteParallelOp(");
  const std::string body = vk.substr(exec, vk.find("\n}\n", exec) - exec);
  Expect(body.find("case parallel_record::kOpPackets:") != std::string::npos &&
             body.find("ExecutePacket()") != std::string::npos &&
             body.find("WriteRegistersSwapped(op.arg,") != std::string::npos &&
             body.find("CheckContentUnits();") != std::string::npos &&
             body.find("OnPrimaryBufferEnd();") != std::string::npos &&
             body.find("read_ptr_writeback_ptr_") != std::string::npos,
         "an op does what ExecutePrimaryBuffer, ExecutePacket and fix 103's runs did");
  Expect(vk.find("static_assert(parallel_record::pm4::kNop == xenos::PM4_NOP") != std::string::npos,
         "the opcodes checked against xenos.h");
  Expect(vkh.find("#include \"xenia/gpu/vulkan/parallel_record.h\"  // fix 142") != std::string::npos,
         "the header included");
  Expect(fc.find("class ParallelRecordReporter {") != std::string::npos &&
             main.find("frame_cost_parallel_record.Line(") != std::string::npos &&
             main.find("frame_cost_parallel_record") > main.find("frame_cost_async_submit.Line("),
         "the 30 s line, after fix 134's");
  Expect(place.find("void PlaceCallingThreadBesideGpu() {") != std::string::npos,
         "the decode thread's placement beside the GPU thread");
}

// (h)
void CheckPatch(const std::string& fixes) {
  const std::string patch = Read(fixes + "/142-parallel-record.patch");
  Expect(!patch.empty(), "the fix's patch is there");
  std::istringstream lines(patch);
  std::string line, file;
  int removed = 0, cp_added = 0;
  std::vector<std::string> cp_lines;
  while (std::getline(lines, line)) {
    if (line.rfind("+++ ", 0) == 0) {
      file = line.substr(4);
      continue;
    }
    if (line.rfind("--- ", 0) == 0) continue;
    if (!line.empty() && line[0] == '-') ++removed;
    if (file.find("xenia/gpu/command_processor.cc") != std::string::npos && !line.empty() &&
        line[0] == '+') {
      ++cp_added;
      cp_lines.push_back(line);
    }
  }
  Expect(removed == 0, "the patch removes no line: the code that runs with it off is unchanged");
  Expect(cp_added == 6 && cp_lines[2] == "+  if (RunParallelRecord()) {",
         "command_processor.cc gets the hook alone");
}

}  // namespace

int main(int argc, char** argv) {
  CheckParse();
  CheckRing();
  CheckFilter();
  CheckOrdering();
  CheckEquivalence();
  CheckLine();
  const bool model_only = argc > 1 && std::string(argv[1]) == "--model-only";
  if (!model_only) {
    CheckTree();
    if (argc > 1) CheckPatch(argv[1]);
  }
  if (failures) {
    std::printf("check-parallel-record: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-parallel-record: ok\n");
  return 0;
}
