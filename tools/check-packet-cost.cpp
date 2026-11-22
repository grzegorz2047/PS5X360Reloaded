// SPDX-License-Identifier: MIT
// Fix 75 (tools/source-1.0.2/fixes/75-packet-cost.patch): the GPU command thread's "rest of
// executing" by PM4 packet kind (Battlefield 3 468 ms/s, Tekken Tag Tournament 2 316 ms/s on
// 1.0.4_candidate_31, more than their draws).
//  (a) include/xbox360ps5/frame_cost.hpp: packet kinds from the packet word (type 0, type 1, the
//      type 3 opcodes); PacketTimer on the GPU thread (pthread_self, no thread_local) times one in
//      packet_sample_every top packets with everything nested in them; a packet's own time leaves
//      out nested packets and sections (a draw's section inside DRAW_INDX); PacketReporter sorts
//      the kinds by time and scales the timed share back; DrawTimer (fix 73) also tests the GPU
//      thread by pthread_self;
//  (b) the assembled tree: ExecutePacket times every packet right after reading it, on the
//      console, and the summary gives the line after fix 74's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
namespace fc = xbox360ps5::frame_cost;
void Spin(uint64_t ns) {
  const uint64_t end = fc::NowNs() + ns;
  while (fc::NowNs() < end) {
  }
}
constexpr uint32_t Type3(uint32_t opcode, uint32_t count = 1) { return (3u << 30) | ((count - 1) << 16) | (opcode << 8); }
double In(const std::string& line, const std::string& kind) {
  const size_t at = line.find(kind + " ");
  return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + kind.size() + 1);
}

void CheckKinds() {
  Expect(fc::PacketKindOf(0x00012345) == fc::kPacketRegisters, "type 0: register writes");
  Expect(fc::PacketKindOf(1u << 30) == fc::kPacketType1, "type 1");
  Expect(fc::PacketKindOf(Type3(0x2D, 4)) == fc::kPacketConstants, "SET_CONSTANT");
  Expect(fc::PacketKindOf(Type3(0x2F)) == fc::kPacketAluConstants, "LOAD_ALU_CONSTANT");
  Expect(fc::PacketKindOf(Type3(0x22)) == fc::kPacketDraw && fc::PacketKindOf(Type3(0x36)) == fc::kPacketDraw, "draws");
  Expect(fc::PacketKindOf(Type3(0x5A)) == fc::kPacketEvents && fc::PacketKindOf(Type3(0x46)) == fc::kPacketEvents, "events");
  Expect(fc::PacketKindOf(Type3(0x3C)) == fc::kPacketWaitRegMem, "WAIT_REG_MEM");
  Expect(fc::PacketKindOf(Type3(0x3F)) == fc::kPacketIndirect, "INDIRECT_BUFFER");
  Expect(fc::PacketKindOf(Type3(0x64)) == fc::kPacketSwap, "XE_SWAP");
  Expect(fc::PacketKindOf(Type3(0x10)) == fc::kPacketOther, "other");
  Expect(fc::PacketKindOf(Type3(0x2D) | 1) == fc::kPacketConstants, "the predicate bit does not change the kind");
}

void CheckTimer() {
  fc::packet_sample_every = 1;
  fc::PacketReporter reporter;
  (void)reporter.Line(1);
  { fc::PacketTimer not_gpu(Type3(0x2D)); }
  Expect(fc::packet_top == 0, "counted on the GPU thread only");
  const uint64_t start = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    {
      fc::PacketTimer indirect(Type3(0x3F));  // an indirect buffer: 2 ms of its own
      Spin(2000000);
      for (int n = 0; n < 3; ++n) {
        fc::PacketTimer constants(Type3(0x2D, 8));
        Spin(3000000);  // 3 ms each
      }
      {
        fc::PacketTimer draw(Type3(0x22));
        Spin(1000000);  // 1 ms decoding
        fc::SectionScope issue(fc::kDraw);
        Spin(10000000);  // 10 ms in the draw's own section: not the packet's
      }
      fc::PacketTimer registers(0x00002000);
      Spin(1000000);
    }
  }).join();
  const double seconds = double(fc::NowNs() - start) / 1e9;
  const std::string line = reporter.Line(seconds);
  std::printf("   %s\n", line.c_str());
  const auto ms = [&](const char* kind) { return In(line, kind) * seconds; };
  const double ib = ms("INDIRECT_BUFFER"), constants = ms("SET_CONSTANT"), draw = ms("DRAW_INDX (decoding)"),
               regs = ms("register writes (type 0)");
  Expect(ib > 1.5 && ib < 3.5, "the indirect buffer: its own 2 ms, not its nested packets' (" + std::to_string(ib) + ")");
  Expect(constants > 8 && constants < 11, "SET_CONSTANT: 3 x 3 ms (" + std::to_string(constants) + ")");
  Expect(draw > 0.7 && draw < 2, "DRAW_INDX: 1 ms of decoding, without the draw's section (" + std::to_string(draw) + ")");
  Expect(regs > 0.7 && regs < 2, "register writes: 1 ms (" + std::to_string(regs) + ")");
  Expect(line.find("Frame time packets (GPU thread, own time by kind): SET_CONSTANT ") == 0, "the largest kind first");
  Expect(fc::packet_top == 1 && fc::packet_depth == 0, "one top packet, depth back to 0");

  // Sampling: one in 4 top packets timed at random (fix 77), scaled back.
  fc::packet_sample_every = 4;
  fc::PacketReporter sampled;
  (void)sampled.Line(1);
  const uint64_t begin = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    for (int n = 0; n < 400; ++n) {
      fc::PacketTimer constants(Type3(0x2D));
      Spin(100000);
    }
  }).join();
  const double span = double(fc::NowNs() - begin) / 1e9;
  const std::string scaled = sampled.Line(span);
  std::printf("   %s\n", scaled.c_str());
  const double total = In(scaled, "SET_CONSTANT") * span;
  Expect(total > 28 && total < 52, "about 100 of 400 timed, scaled to about 40 ms (" + std::to_string(total) + ")");
}

void CheckTree() {
  const std::string pm4 = Read(".deps/xenia-canary/src/xenia/gpu/pm4_command_processor_implement.h");
  const size_t fn = pm4.find("bool COMMAND_PROCESSOR::ExecutePacket() {");
  const size_t read = pm4.find("const uint32_t packet = reader_.ReadAndSwap<uint32_t>();", fn);
  const size_t timer = pm4.find("xbox360ps5::frame_cost::PacketTimer frame_cost_packet(packet);", read);
  const size_t guard = pm4.rfind("#if XE_PLATFORM_PS5", timer);
  const size_t dispatch = pm4.find("return COMMAND_PROCESSOR::ExecutePacketType3(packet);", read);
  Expect(fn != std::string::npos && read != std::string::npos && timer != std::string::npos && guard > read &&
             timer < dispatch,
         "every packet is timed right after it is read, on the console");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t arming = main_code.find("XELOGW(\"{}\", frame_cost_arming.Line(double(summary_samples)));");
  const size_t packets = main_code.find("XELOGW(\"{}\", frame_cost_packets.Line(double(summary_samples)));");
  Expect(arming != std::string::npos && packets != std::string::npos && arming < packets, "the summary gives the line after fix 74's");
  const std::string header = Read("include/xbox360ps5/frame_cost.hpp");
  Expect(header.find("DrawTimer() : active_(OnGpuThread()) {") != std::string::npos,
         "the draw timer tests the GPU thread without thread_local");
}

}  // namespace

int main() {
  CheckKinds();
  CheckTimer();
  CheckTree();
  if (failures) {
    std::printf("check-packet-cost: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-packet-cost: ok\n");
  return 0;
}
