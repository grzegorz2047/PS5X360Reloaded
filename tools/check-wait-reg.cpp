// SPDX-License-Identifier: MIT
// Fix 77 (tools/source-1.0.2/fixes/77-wait-reg-and-sampling.patch), from 1.0.5_candidate_50:
//  (a) include/xbox360ps5/frame_cost.hpp: draws (fix 73) and packets (fix 75) are timed one in n
//      at random (SampleOneIn, xorshift32), not every n-th. A game's ring repeats the same
//      packets every frame, so the counter took the same ones each time: Tekken Tag Tournament
//      2's WAIT_REG_MEM came out at 1,300 to 2,000 ms/s, Battlefield 3's packets at almost none
//      of its 424 ms/s. A ring that repeats every 4 packets is measured right now.
//  (b) WaitRegTimer and WaitRegReporter: every WAIT_REG_MEM counted; one that does not match at
//      once timed from its first miss to its match, its time in Xenia's sleeps apart from the
//      polling; by the packet's wait field (0, 1-FF, 100-3FF, 400+).
//  (c) the assembled tree: the WAIT_REG_MEM handler makes the timer before its loop, notes each
//      miss, times each sleep; the summary gives the line after fix 75's.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xbox360ps5/frame_cost.hpp"

#include <cstdio>
#include <cstdlib>
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

void CheckSampling() {
  int taken = 0;
  for (int n = 0; n < 160000; ++n) taken += fc::SampleOneIn(16) ? 1 : 0;
  Expect(taken > 9500 && taken < 10500, "one in 16 at random (" + std::to_string(taken) + " of 160000)");
  Expect(fc::SampleOneIn(1) && fc::SampleOneIn(0), "every one when n is 0 or 1");
  // A ring that repeats every 4 packets: one long (0.4 ms), three short. Every 4th by a counter
  // takes the long one always or never (4 times or 0 times its share); at random, about right.
  fc::packet_sample_every = 4;
  fc::PacketReporter reporter;
  (void)reporter.Line(1);
  const uint64_t begin = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    for (int frame = 0; frame < 200; ++frame) {
      {
        fc::PacketTimer wait(Type3(0x3C));
        Spin(400000);
      }
      for (int n = 0; n < 3; ++n) fc::PacketTimer registers(0x00002000);
    }
  }).join();
  const double span = double(fc::NowNs() - begin) / 1e9;
  const std::string line = reporter.Line(span);
  std::printf("   %s\n", line.c_str());
  const double waits = In(line, "WAIT_REG_MEM") * span;
  Expect(waits > 55 && waits < 110, "a ring repeating every 4 packets: WAIT_REG_MEM about 80 ms, not 0 or 320 (" +
                                        std::to_string(waits) + ")");
}

void CheckWaitReg() {
  fc::WaitRegReporter reporter;
  (void)reporter.Line(1);
  { fc::WaitRegTimer not_gpu(true, 0x200); not_gpu.Missed(); }
  Expect(fc::wait_reg_packets == 0 && fc::wait_reg_waits == 0, "counted on the GPU thread only");
  const uint64_t begin = fc::NowNs();
  std::thread([] {
    fc::MarkGpuThread();
    { fc::WaitRegTimer at_once(false, 0); }  // matched at the first poll
    {
      fc::WaitRegTimer polled(true, 0);  // three misses, 3 ms of polling
      for (int n = 0; n < 3; ++n) {
        polled.Missed();
        Spin(1000000);
      }
    }
    {
      fc::WaitRegTimer slept(true, 0x200);  // two misses, a 4 ms sleep after each
      for (int n = 0; n < 2; ++n) {
        slept.Missed();
        const uint64_t before = slept.BeforeSleep();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        slept.AfterSleep(before);
      }
    }
  }).join();
  const double span = double(fc::NowNs() - begin) / 1e9;
  const std::string line = reporter.Line(span);
  std::printf("   %s\n", line.c_str());
  Expect(fc::wait_reg_packets == 3 && fc::wait_reg_waits == 2 && fc::wait_reg_memory_waits == 2,
         "three packets, two waited, both on memory");
  Expect(fc::wait_reg_polls == 5 && fc::wait_reg_sleeps == 2, "five missed polls, two sleeps");
  Expect(fc::wait_reg_by_field[0] == 1 && fc::wait_reg_by_field[2] == 1, "by wait field: one 0, one 100-3FF");
  const auto value = [&](const std::string& after) {
    const size_t at = line.find(after);
    return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + after.size());
  };
  const double polling = value("polling ") * span, sleeping = value("sleeping ") * span;
  Expect(polling > 2.5 && polling < 5, "polling: about 3 ms (" + std::to_string(polling) + ")");
  Expect(sleeping > 7.5 && sleeping < 20, "sleeping: about 8 ms (" + std::to_string(sleeping) + ")");
  Expect(line.find("Frame time WAIT_REG_MEM (GPU thread): ") == 0 && line.find("; waits by wait field: 0 ") != std::string::npos,
         "the line");
  Expect(reporter.Line(1).find("(GPU thread): 0/s, 0/s waited") != std::string::npos, "only since the previous line");
}

void CheckTree() {
  const std::string pm4 = Read(".deps/xenia-canary/src/xenia/gpu/pm4_command_processor_implement.h");
  const size_t fn = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_WAIT_REG_MEM(");
  const size_t timer = pm4.find("xbox360ps5::frame_cost::WaitRegTimer frame_cost_wait(is_memory, wait);", fn);
  const size_t loop = pm4.find("  do {\n    uint32_t value = value_ref;", fn);
  const size_t loop_crlf = pm4.find("  do {\r\n    uint32_t value = value_ref;", fn);
  const size_t do_at = loop != std::string::npos ? loop : loop_crlf;
  const size_t missed = pm4.find("frame_cost_wait.Missed();  // fix 77", fn);
  const size_t before = pm4.find("const uint64_t frame_cost_sleep = frame_cost_wait.BeforeSleep();  // fix 77", fn);
  const size_t sleep = pm4.find("xe::threading::Sleep(std::chrono::milliseconds(wait / 0x100));", before);
  const size_t after = pm4.find("frame_cost_wait.AfterSleep(frame_cost_sleep);", sleep);
  const size_t next = pm4.find("bool COMMAND_PROCESSOR::ExecutePacketType3_REG_RMW(", fn);
  Expect(fn != std::string::npos && timer != std::string::npos && do_at != std::string::npos && timer < do_at &&
             missed > do_at && before > missed && sleep > before && after > sleep && after < next,
         "WAIT_REG_MEM: the timer before the loop, each miss noted, each sleep timed");
  const std::string main_code = Read("src/canary_game_main.cpp");
  const size_t packets = main_code.find("XELOGW(\"{}\", frame_cost_packets.Line(double(summary_samples)));");
  const size_t waits = main_code.find("XELOGW(\"{}\", frame_cost_wait_reg.Line(double(summary_samples)));");
  Expect(packets != std::string::npos && waits != std::string::npos && packets < waits, "the summary gives the line after fix 75's");
  const std::string header = Read("include/xbox360ps5/frame_cost.hpp");
  Expect(header.find("_sample_counter") == std::string::npos &&
             header.find("active_ = SampleOneIn(draw_sample_every.load(std::memory_order_relaxed));") != std::string::npos &&
             header.find("packet_sampling = SampleOneIn(packet_sample_every.load(std::memory_order_relaxed));") != std::string::npos,
         "draws and packets drawn at random, no every-n-th counter left");
}

}  // namespace

int main() {
  CheckSampling();
  CheckWaitReg();
  CheckTree();
  if (failures) {
    std::printf("check-wait-reg: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-wait-reg: ok\n");
  return 0;
}
