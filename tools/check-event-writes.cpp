// SPDX-License-Identifier: MIT
// Fix 96 (tools/source-1.0.2/fixes/96-event-writes.patch): what the EVENT_WRITE* packets are.
// Red Dead Redemption on the 1.0.5 perf builds sends 233,490 a second (about 1.4 per draw), 64.5 ms/s
// of the GPU command thread's own time, 0.28 us each against 0.08 for a register write; Modern
// Warfare 2 sends 2,400. Fix 75's summary has them as one kind.
//  (a) include/xbox360ps5/frame_cost.hpp: every event packet the GPU thread runs is counted by
//      opcode and VGT_EVENT_INITIATOR type, other threads' not; the packets fix 75 times give each
//      opcode its own time, other packets nothing; EventReporter gives the opcodes sent most first,
//      each with its time per packet and its commonest types by name, since its previous line;
//  (b) the assembled tree: each of the four handlers counts its packet after reading the initiator
//      (on the console), and the summary gives the line after fix 75's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
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
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
namespace fc = xbox360ps5::frame_cost;
constexpr uint32_t Type3(uint32_t opcode, uint32_t count) { return 0xC0000000u | ((count - 1) << 16) | (opcode << 8); }

void CheckCounting() {
  Expect(fc::EventOpcodeOf(Type3(0x46, 1)) == fc::kEventWrite && fc::EventOpcodeOf(Type3(0x58, 3)) == fc::kEventWriteShd &&
             fc::EventOpcodeOf(Type3(0x5A, 2)) == fc::kEventWriteExt && fc::EventOpcodeOf(Type3(0x5B, 1)) == fc::kEventWriteZpd,
         "the four EVENT_WRITE opcodes");
  Expect(fc::EventOpcodeOf(Type3(0x3C, 6)) == fc::kEventOpcodes && fc::EventOpcodeOf(0x00004600u) == fc::kEventOpcodes &&
             fc::EventOpcodeOf(Type3(0x59, 1)) == fc::kEventOpcodes,
         "other packets (WAIT_REG_MEM, a type 0 write, EVENT_WRITE_CFL) are not events here");
  Expect(std::string(fc::EventTypeName(22)) == "CACHE_FLUSH_AND_INV_EVENT" && std::string(fc::EventTypeName(21)) == "ZPASS_DONE" &&
             std::string(fc::EventTypeName(7)) == "VIZQUERY_START" && std::string(fc::EventTypeName(27)) == "VS_FETCH_DONE_TS" &&
             fc::EventTypeName(17) == nullptr && fc::EventTypeName(40) == nullptr,
         "the event types by xenos::Event's names");

  fc::EventReporter reporter;
  (void)reporter.Line(1);
  // Not the GPU thread: nothing counted.
  std::thread other([] {
    for (int n = 0; n < 50; ++n) fc::CountEvent(fc::kEventWrite, 22);
  });
  other.join();
  Expect(reporter.Line(1) == "Performance events (GPU thread, every EVENT_WRITE* packet): none", "other threads are not counted");

  fc::MarkGpuThread();
  for (int n = 0; n < 1000; ++n) fc::CountEvent(fc::kEventWrite, 0x80000000u | 22);  // the type is the low 6 bits
  for (int n = 0; n < 10; ++n) fc::CountEvent(fc::kEventWrite, 7);
  for (int n = 0; n < 3; ++n) fc::CountEvent(fc::kEventWrite, 40);
  for (int n = 0; n < 500; ++n) fc::CountEvent(fc::kEventWriteZpd, 21);
  // Timed: one EVENT_WRITE_ZPD packet, then one WAIT_REG_MEM (not an event).
  fc::packet_sample_every = 1;
  {
    fc::PacketTimer timer(Type3(0x5B, 1));
    const uint64_t start = fc::Ticks();
    while (fc::Ticks() - start < 20000) {
    }
  }
  {
    fc::PacketTimer timer(Type3(0x3C, 6));
  }
  Expect(fc::event_timed[fc::kEventWriteZpd] == 1 && fc::event_ticks[fc::kEventWriteZpd] >= 20000,
         "a timed event packet gives its opcode its own time");
  Expect(fc::event_timed[fc::kEventWrite] == 0 && fc::event_timed[fc::kEventWriteShd] == 0 &&
             fc::event_timed[fc::kEventWriteExt] == 0,
         "other packets give the event opcodes nothing");
  const std::string line = reporter.Line(1);
  std::printf("   %s\n", line.c_str());
  Expect(line.find("Performance events (GPU thread, every EVENT_WRITE* packet): EVENT_WRITE 1013/s, untimed "
                   "(CACHE_FLUSH_AND_INV_EVENT 1000/s, VIZQUERY_START 10/s, type 40 3/s); EVENT_WRITE_ZPD 500/s, ") == 0,
         "the opcodes sent most first, each with its commonest types");
  Expect(line.find(" us each (ZPASS_DONE 500/s)") != std::string::npos && line.find("EVENT_WRITE_SHD") == std::string::npos,
         "a timed opcode's time per packet; opcodes not sent left out");
  Expect(reporter.Line(1) == "Performance events (GPU thread, every EVENT_WRITE* packet): none", "only since the previous line");
}

void CheckTree() {
  const std::string pm4 = Read(".deps/xenia-canary/src/xenia/gpu/pm4_command_processor_implement.h");
  const char* const handlers[] = {"ExecutePacketType3_EVENT_WRITE(", "ExecutePacketType3_EVENT_WRITE_SHD(",
                                  "ExecutePacketType3_EVENT_WRITE_EXT(", "ExecutePacketType3_EVENT_WRITE_ZPD("};
  const char* const opcodes[] = {"kEventWrite", "kEventWriteShd", "kEventWriteExt", "kEventWriteZpd"};
  for (int i = 0; i < 4; ++i) {
    const size_t fn = pm4.find(std::string("bool COMMAND_PROCESSOR::") + handlers[i]);
    const size_t initiator = pm4.find("COMMAND_PROCESSOR::WriteEventInitiator(initiator & 0x3", fn);
    const size_t counted = pm4.find(std::string("#if XE_PLATFORM_PS5\n  xbox360ps5::frame_cost::CountEvent(xbox360ps5::frame_cost::") +
                                        opcodes[i] + ", initiator);  // fix 96\n#endif\n",
                                    initiator);
    const size_t next = pm4.find("\nbool COMMAND_PROCESSOR::", fn + 1);
    Expect(fn != std::string::npos && initiator != std::string::npos && counted != std::string::npos && counted < next &&
               counted - initiator < 80,
           std::string(handlers[i]) + " counts its packet after reading the initiator");
  }
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t packets = main_code.find("XELOGW(\"{}\", frame_cost_packets.Line(double(summary_samples)));");
  const size_t events = main_code.find("XELOGW(\"{}\", frame_cost_events.Line(double(summary_samples)));", packets);
  Expect(packets != std::string::npos && events != std::string::npos && events - packets < 240,
         "the summary gives the line after fix 75's");
}

}  // namespace

int main() {
  CheckCounting();
  CheckTree();
  if (failures) {
    std::printf("check-event-writes: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-event-writes: ok\n");
  return 0;
}
