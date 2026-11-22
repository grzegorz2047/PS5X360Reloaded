// SPDX-License-Identifier: MIT
// Fix 134 (F2, xenia/gpu/vulkan/async_submit.h, vulkan_command_processor.cc): chunks of a frame
// handed to a submit worker. Fix 130 measured the replay of the deferred command buffer on the
// GPU thread: Grand Theft Auto IV 145-147 ms/s and Red Dead Redemption 92-140 ms/s, with the GPU
// thread saturated and about one submission a frame, the swap's, which must be submitted before
// the presenter gets the image back.
//  (a) async-submit.txt: mode and chunk lines, comments, bounds, lines not understood;
//  (b) the decisions: mode 0 never ends a chunk nor hands one off (today's command stream); a
//      chunk only from the chunk size on; never the swap's submission, nor one with wait
//      semaphores or query resolves;
//  (c) the worker: jobs run in order, at most two pending (Enqueue waits for room), Drain waits
//      until all are submitted;
//  (d) a model of the GPU thread with the worker: frames of chunks, a readback wait and the
//      swap; the queue sees the submissions in their order; every wait drains first, so nothing
//      waited for is still unsubmitted; at the swap's submit the worker is idle, and no submission
//      comes between the swap's and the presenter's;
//  (e) the assembled tree: the cvars (0 and 256 KiB), the chunk check before a draw, the hand-off
//      branch with the drain before any other submission and the original sequence untouched
//      for it, every wait for a submission through AwaitSubmissionDrained, the worker's thread
//      and name, the stop before shutdown, the reporter's line after fix 130's.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "xenia/gpu/vulkan/async_submit.h"

namespace {

namespace as = xe::gpu::async_submit;
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

void CheckParse() {
  as::Settings s;
  Expect(as::Parse("mode 1\nchunk 128\n", s) == 0 && s.mode == 1 && s.chunk_kib == 128, "mode and chunk");
  as::Settings t;
  t.mode = 2;
  t.chunk_kib = 512;
  Expect(as::Parse("# only a comment\n\n", t) == 0 && t.mode == 2 && t.chunk_kib == 512,
         "what the file does not set stays as the cvars set it");
  as::Settings u;
  Expect(as::Parse("mode 3\nchunk 4\nchunk 70000\nmode\nmode 1 2\nspeed 5\nmode 2 # chunks only\r\n", u) == 6 &&
             u.mode == 2 && u.chunk_kib == 256,
         "out of range, missing or extra values and unknown keys are not understood; CRLF and comments are");
}

void CheckDecisions() {
  for (size_t bytes : {size_t(0), size_t(1) << 20, size_t(64) << 20}) {
    Expect(!as::ChunkDue(as::kOff, bytes, 256), "mode 0 never ends a chunk");
  }
  Expect(!as::ChunkDue(as::kWorker, 255 * 1024, 256) && as::ChunkDue(as::kWorker, 256 * 1024, 256) &&
             as::ChunkDue(as::kChunkOnly, 300 * 1024, 256),
         "a chunk from the chunk size on, in modes 1 and 2");
  Expect(!as::HandOff(as::kOff, false, false, false) && !as::HandOff(as::kChunkOnly, false, false, false),
         "only mode 1 hands off");
  Expect(as::HandOff(as::kWorker, false, false, false) && !as::HandOff(as::kWorker, true, false, false) &&
             !as::HandOff(as::kWorker, false, true, false) && !as::HandOff(as::kWorker, false, false, true),
         "never the swap's submission, nor one with wait semaphores or query resolves");
}

// The model: a queue that records the order submissions reach it and which side made them.
struct Queue {
  std::mutex mutex;
  std::vector<uint64_t> order;
  std::vector<char> who;  // 'w' worker, 'g' GPU thread, 'p' presenter
  void Submit(uint64_t index, char by) {
    std::lock_guard<std::mutex> lock(mutex);
    order.push_back(index);
    who.push_back(by);
  }
};
struct Job {
  uint64_t index;
};

void CheckWorker() {
  as::Worker<Job> worker;
  Queue queue;
  std::atomic<int> running{0}, most{0};
  std::thread thread([&] {
    worker.Loop([&](Job& job) {
      const int now = ++running;
      if (now > most) most = now;
      std::this_thread::sleep_for(std::chrono::microseconds(300));  // the replay
      queue.Submit(job.index, 'w');
      --running;
    });
  });
  bool waited_for_room = false;
  for (uint64_t n = 1; n <= 20; ++n) waited_for_room |= worker.Enqueue(Job{n});
  Expect(worker.Drain(), "Drain waits while jobs are pending");
  Expect(!worker.Busy() && queue.order.size() == 20, "after Drain every job is submitted");
  bool in_order = true;
  for (size_t n = 0; n < queue.order.size(); ++n) in_order &= queue.order[n] == n + 1;
  Expect(in_order && most.load() == 1, "one at a time, in order");
  Expect(waited_for_room, "Enqueue waits for room (at most two pending)");
  Expect(!worker.Drain(), "Drain on an idle worker does not wait");
  worker.Stop();
  thread.join();
}

void CheckModel() {
  as::Worker<Job> worker;
  Queue queue;
  std::atomic<uint64_t> worker_submitted{0};
  std::thread thread([&] {
    worker.Loop([&](Job& job) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      queue.Submit(job.index, 'w');
      worker_submitted = job.index;
    });
  });
  uint64_t next = 1;          // GetCurrentSubmission
  uint64_t submitted_max = 0;  // what the timeline may be asked to wait for
  bool waited_unsubmitted = false, busy_at_swap = false;
  std::vector<std::pair<uint64_t, uint64_t>> swap_then_present;  // (swap's index, queue position)
  for (int frame = 0; frame < 40; ++frame) {
    for (int chunk = 0; chunk < 4; ++chunk) {
      worker.Enqueue(Job{next});  // HandOffSubmission
      submitted_max = next++;
      if (chunk == 1 && frame % 5 == 0) {
        // A readback: AwaitSubmissionDrained drains, then waits for the submission.
        worker.Drain();
        waited_unsubmitted |= worker_submitted.load() < submitted_max;
      }
    }
    // The swap: EndSubmission(true) drains, then submits on the GPU thread.
    worker.Drain();
    busy_at_swap |= worker.Busy();
    queue.Submit(next, 'g');
    const uint64_t swap_index = next++;
    // The presenter's own commands, after EndSubmission(true) returned.
    queue.Submit(0, 'p');
    swap_then_present.push_back({swap_index, queue.order.size() - 1});
  }
  worker.Drain();
  worker.Stop();
  thread.join();
  bool in_order = true;
  uint64_t last = 0;
  for (size_t n = 0; n < queue.order.size(); ++n) {
    if (queue.who[n] == 'p') continue;
    in_order &= queue.order[n] > last;
    last = queue.order[n];
  }
  bool present_right_after_swap = true;
  for (const auto& [swap_index, position] : swap_then_present) {
    present_right_after_swap &= position > 0 && queue.order[position - 1] == swap_index && queue.who[position - 1] == 'g';
  }
  std::printf("model: %zu submissions in 40 frames (4 chunks each, a readback every 5th frame)\n", queue.order.size());
  Expect(in_order, "the queue sees the submissions in their order");
  Expect(!waited_unsubmitted, "a wait never finds its submission still with the worker");
  Expect(!busy_at_swap, "the worker is idle at the swap's submit");
  Expect(present_right_after_swap, "no submission between the swap's and the presenter's");
}

void CheckTree() {
  const std::string vk = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string vh = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  const std::string dcb = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/deferred_command_buffer.h");
  const std::string qp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_query_pool.h");
  Expect(vk.find("DEFINE_uint32(gpu_async_submit, 0,") != std::string::npos &&
             vk.find("DEFINE_uint32(gpu_async_submit_chunk_kib, 256,") != std::string::npos,
         "the cvars: off, 256 KiB");
  Expect(vh.find("#include \"xenia/gpu/vulkan/async_submit.h\"  // fix 134") != std::string::npos &&
             dcb.find("void SwapContents(DeferredCommandBuffer& other) {") != std::string::npos &&
             qp.find("bool HasResolveBatch() const {") != std::string::npos,
         "the worker's job, the commands' exchange, the query resolves' test");
  // The chunk check opens IssueDraw.
  const size_t draw = vk.find("bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type,");
  const size_t chunk = vk.find("async_submit::ChunkDue(async_settings_.mode,", draw);
  const size_t inner = vk.find("IssueDrawInner(prim_type, index_count, index_buffer_info,", draw);
  Expect(draw != std::string::npos && chunk < inner && vk.find("CanEndSubmissionImmediately()) {", chunk) < inner,
         "a chunk ends before the draw, when the submission may end now");
  // EndSubmission: the hand-off, the drain before any other submission, the original sequence
  // in the other branch.
  const size_t end = vk.find("bool VulkanCommandProcessor::EndSubmission(bool is_swap) {");
  const size_t hand = vk.find("const bool async_hand_off = async_submit::HandOff(", end);
  const size_t drain = vk.find("DrainAsyncSubmit(is_swap ? async_submit::kDrainSwap : async_submit::kDrainSync);", end);
  const size_t sync_open = vk.find("    if (!async_hand_off) {\n#endif\n    assert_false(command_buffers_writable_.empty());", end);
  const size_t execute = vk.find("deferred_command_buffer_.Execute(command_buffer.buffer);", end);
  const size_t submit = vk.find("completion_timeline_.AcquireFenceAndSubmit(", end);
  const size_t sync_close = vk.find("    }  // fix 134: not handed off\n", end);
  Expect(end != std::string::npos && hand > end && drain > hand && sync_open > drain && execute > sync_open &&
             submit > execute && sync_close > submit,
         "EndSubmission: the hand-off, else a drain and the original sequence");
  Expect(vk.find("RefreshAsyncSubmitSettings();  // fix 134", end) < hand, "the settings read again before");
  // Every wait for a submission drains first (Poll waits for a completed one only).
  size_t direct = 0;
  for (size_t at = vk.find("completion_timeline_.AwaitSubmissionAndUpdateCompleted("); at != std::string::npos;
       at = vk.find("completion_timeline_.AwaitSubmissionAndUpdateCompleted(", at + 1)) {
    ++direct;
  }
  size_t drained = 0;
  for (size_t at = vk.find("AwaitSubmissionDrained("); at != std::string::npos; at = vk.find("AwaitSubmissionDrained(", at + 1)) {
    ++drained;
  }
  Expect(direct == 2 && vk.find("completion_timeline_.AwaitSubmissionAndUpdateCompleted(\n      GetCompletedSubmission());") !=
                            std::string::npos,
         "the timeline is awaited directly only by the poll and by AwaitSubmissionDrained (" + std::to_string(direct) + ")");
  Expect(drained >= 5, "the waits go through AwaitSubmissionDrained (" + std::to_string(drained) + ")");
  Expect(vk.find("async_worker_thread_->set_name(\"GPU Submit worker\");") != std::string::npos &&
             vk.find("new kernel::XHostThread(") != std::string::npos,
         "the worker is a named host thread, listed and placed like the others");
  Expect(vk.find("void VulkanCommandProcessor::ShutdownContext() {\n  StopAsyncSubmit();  // fix 134\n  "
                 "AwaitAllQueueOperationsCompletion();") != std::string::npos,
         "the worker stops before shutdown");
  const std::string game_main = Read("src/canary_game_main.cpp");
  const size_t parts = game_main.find("XELOGW(\"{}\", frame_cost_submit_parts.Line(double(summary_samples)));");
  const size_t async_line = game_main.find("static xbox360ps5::frame_cost::AsyncSubmitReporter frame_cost_async_submit;");
  Expect(parts != std::string::npos && async_line > parts, "the line after fix 130's");
}

}  // namespace

int main(int argc, char** argv) {
  CheckParse();
  CheckDecisions();
  CheckWorker();
  CheckModel();
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-async-submit: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-async-submit: ok\n");
  return 0;
}
