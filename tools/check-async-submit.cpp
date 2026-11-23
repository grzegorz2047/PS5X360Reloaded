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
// Fix 134b: 1.0.5-work-fix134 in Grand Theft Auto IV drained the worker at every chunk (4.00
// "wait" drains per frame, 160 ms/s), so the worker's replay came back as waiting: BeginSubmission
// waits for submission 0 at every new submission. Now only a wait for a submission still with the
// worker drains it (NeedsDrain), and the "wait" drains are logged by line.
//  (f) NeedsDrain; the model's BeginSubmission waits for 0 after every chunk and drain nothing,
//      while readback waits still find their submission submitted; the tree's wiring.
// Fix 134c: Red Dead Redemption in mode 1 handed off 30 of 180-191 submissions/s (5.0-5.36 chunks
// per frame): a submission with query resolves stayed on the GPU thread, which still replayed
// 120-131 ms/s. Its resolves now go to the end of its commands, where the GPU thread recorded
// them after the replay, and the line counts why a submission stays.
//  (g) "resolves 0|1"; Kept: the swap, then sparse binds, then query resolves only with
//      "resolves 0"; a model of Red Dead Redemption's frame (5 chunks with occlusion queries and
//      the swap): 1 hand-off a frame as in 134, 5 with 134c; the resolves at the end of the
//      handed-off commands, the same place as on the GPU thread;
//  (h) the tree: the query pool's recorder for both kinds of command buffer (the Vulkan one
//      unchanged), the resolves recorded into the deferred commands before the hand-off
//      decision, only when it hands off, the reasons counted, the 30 s line and the settings.
// Fix 134e: mode 1 by default for Grand Theft Auto IV (545407F2) and Red Dead Redemption
// (5454082B), confirmed on the console in mode 1 (1.0.5-work-fix134b to 134d).
//  (j) PresetMode and ModeFor: the two games mode 1, every other title and no title (0) 0; a
//      cvar other than 0 wins over the preset (and is clamped), the file's "mode" line over
//      both; the tree: RefreshAsyncSubmitSettings takes the mode from ModeFor with the running
//      title, the file's lines decide when it exists, the settings line names the preset.
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

// Fix 134c.
void CheckKept() {
  as::Settings s;
  Expect(s.resolves == 1, "query resolves handed off by default");
  Expect(as::Parse("resolves 0\n", s) == 0 && s.resolves == 0, "resolves 0: kept on the GPU thread");
  as::Settings t;
  Expect(as::Parse("resolves 2\nresolves\n", t) == 2 && t.resolves == 1, "resolves 2 or nothing: not understood");
  as::Settings u;
  u.resolves = 0;
  Expect(!(u == as::Settings()), "a change of resolves is a change of the settings (logged, applied)");
  Expect(as::Kept(true, true, true, false) == as::kKeptSwap, "the swap first");
  Expect(as::Kept(false, true, true, false) == as::kKeptSparse, "then sparse binds");
  Expect(as::Kept(false, false, true, false) == as::kKeptResolves, "query resolves with resolves 0");
  Expect(as::Kept(false, false, true, true) == as::kKeptReasons && as::Kept(false, false, false, false) == as::kKeptReasons,
         "handed off: with resolves 1, or none to record");
  // Red Dead Redemption's frame: 5 chunks, each with occlusion queries ended in it but the first,
  // then the swap. The commands of a submission: its draws, then (GPU thread or worker) the
  // resolves; the worker replays the deferred commands, so resolves recorded there must be last.
  for (uint32_t resolves : {0u, 1u}) {
    uint32_t handed_off = 0, kept_resolves = 0, kept_swap = 0;
    bool resolves_last = true;
    for (int frame = 0; frame < 30; ++frame) {
      for (int chunk = 0; chunk < 6; ++chunk) {
        const bool is_swap = chunk == 5;
        const bool query_resolves = chunk != 0;
        std::vector<char> commands(4, 'd');  // the draws, replayed
        bool recorded = false;
        if (query_resolves && resolves && as::HandOff(as::kWorker, is_swap, false, false)) {
          commands.push_back('r');  // RecordResolveBatch(deferred_command_buffer_)
          recorded = true;
        }
        const uint32_t kept = as::Kept(is_swap, false, query_resolves, resolves != 0);
        kept_resolves += kept == as::kKeptResolves;
        kept_swap += kept == as::kKeptSwap;
        if (as::HandOff(as::kWorker, is_swap, false, query_resolves && !recorded)) {
          ++handed_off;
        } else if (query_resolves) {
          commands.push_back('r');  // after the replay, on the GPU thread
        }
        resolves_last = resolves_last && (!query_resolves || commands.back() == 'r');
      }
    }
    std::printf("model 134c (resolves %u): %u hand-offs in 30 frames, kept for query resolves %u, swap %u\n",
                resolves, handed_off, kept_resolves, kept_swap);
    Expect(handed_off == (resolves ? 150u : 30u) && kept_swap == 30 && kept_resolves == (resolves ? 0u : 120u),
           "Red Dead Redemption's frame: 1 hand-off a frame as in 134, 5 with 134c");
    Expect(resolves_last, "the resolves after the submission's draws either way");
  }
}

// Fix 134e.
void CheckPreset() {
  Expect(as::PresetMode(0x545407F2) == as::kWorker && as::PresetMode(0x5454082B) == as::kWorker,
         "fix 134e: Grand Theft Auto IV and Red Dead Redemption get mode 1");
  Expect(as::PresetMode(0x534507E1) == as::kOff && as::PresetMode(0x41560817) == as::kOff &&
             as::PresetMode(0x4D5308C9) == as::kOff && as::PresetMode(0) == as::kOff,
         "fix 134e: every other title (Virtua Tennis 3, Modern Warfare 2, Kinect Sports) and none: 0");
  bool from_preset = true;
  Expect(as::ModeFor(0, 0x545407F2, &from_preset) == as::kWorker && from_preset,
         "fix 134e: the cvar at 0: the game's preset");
  Expect(as::ModeFor(0, 0x534507E1, &from_preset) == as::kOff && !from_preset,
         "fix 134e: the cvar at 0, no preset: off");
  Expect(as::ModeFor(2, 0x545407F2, &from_preset) == as::kChunkOnly && !from_preset,
         "fix 134e: a cvar other than 0 wins over the preset");
  Expect(as::ModeFor(7, 0x534507E1, &from_preset) == as::kModes - 1 && !from_preset,
         "fix 134e: a cvar out of range clamped as before");
  as::Settings s;
  s.mode = as::ModeFor(0, 0x5454082B, &from_preset);
  Expect(as::Parse("mode 0\n", s) == 0 && s.mode == as::kOff, "fix 134e: the file's mode line wins over the preset");
  as::Settings t;
  t.mode = as::ModeFor(0, 0x5454082B, &from_preset);
  Expect(as::Parse("chunk 128\n", t) == 0 && t.mode == as::kWorker && t.chunk_kib == 128,
         "fix 134e: a file without a mode line keeps the preset's mode");
}

void CheckNeedsDrain() {
  Expect(!as::NeedsDrain(0, 0, 0, false), "a wait for 0 with nothing handed off: no drain");
  Expect(!as::NeedsDrain(0, 7, 9, false), "BeginSubmission's wait for 0: no drain");
  Expect(!as::NeedsDrain(7, 7, 9, false) && !as::NeedsDrain(5, 7, 9, false), "already submitted: no drain");
  Expect(as::NeedsDrain(8, 7, 9, false) && as::NeedsDrain(9, 7, 9, false), "still with the worker: drain");
  Expect(!as::NeedsDrain(10, 7, 9, false), "not handed off (the GPU thread's own): no drain");
  Expect(as::NeedsDrain(0, 7, 9, true), "a failure is surfaced by a drain");
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
  uint64_t begin_drains = 0;  // fix 134b: BeginSubmission's waits for 0 that drained
  std::vector<std::pair<uint64_t, uint64_t>> swap_then_present;  // (swap's index, queue position)
  for (int frame = 0; frame < 40; ++frame) {
    for (int chunk = 0; chunk < 4; ++chunk) {
      worker.Enqueue(Job{next});  // HandOffSubmission
      submitted_max = next++;
      // The next chunk's BeginSubmission: a wait for submission 0 (fix 134b: no drain).
      if (as::NeedsDrain(0, worker_submitted.load(), submitted_max, false)) {
        worker.Drain();
        ++begin_drains;
      }
      if (chunk == 1 && frame % 5 == 0) {
        // A readback: AwaitSubmissionDrained drains, then waits for the submission.
        if (as::NeedsDrain(submitted_max, worker_submitted.load(), submitted_max, false)) worker.Drain();
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
  Expect(begin_drains == 0, "fix 134b: no drain at the chunks' BeginSubmission");
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
  // Fix 134b.
  const size_t awaited = vk.find("bool VulkanCommandProcessor::AwaitSubmissionDrained(uint64_t submission) {");
  Expect(awaited != std::string::npos &&
             vk.find("async_submit::NeedsDrain(submission, async_submitted_.load(std::memory_order_acquire),", awaited) <
                 vk.find("DrainAsyncSubmit(", awaited),
         "fix 134b: a wait drains only for a submission still with the worker");
  Expect(vk.find("async_handed_off_ = job.submission;  // fix 134b") != std::string::npos &&
             vk.find("async_submitted_.store(job.submission, std::memory_order_release);  // fix 134b") != std::string::npos,
         "fix 134b: the hand-off and the worker's submit are tracked");
  // Fix 134c.
  const std::string qpc = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_query_pool.cc");
  Expect(qp.find("void RecordResolveBatch(DeferredCommandBuffer& deferred_command_buffer);") != std::string::npos &&
             qpc.find("struct VulkanQueryPool::ResolveRecorder {") != std::string::npos &&
             qpc.find("deferred->CmdVkCopyQueryPoolResults(pool, first, count, dst, offset, stride, flags);") !=
                 std::string::npos &&
             qpc.find("dfn->vkCmdCopyQueryPoolResults(command_buffer, pool, first, count, dst, offset, stride,") !=
                 std::string::npos,
         "fix 134c: the query resolves into either kind of command buffer");
  const size_t record_fn = qpc.find("void VulkanQueryPool::RecordResolveBatchTo(const ResolveRecorder& recorder) {");
  const size_t record_end = qpc.find("\n}\n", record_fn);
  Expect(record_fn != std::string::npos && qpc.find("dfn.vkCmd", record_fn) > record_end &&
             qpc.find("recorder.CopyBuffer(counter_buffer_, counter_readback_buffer_,", record_fn) < record_end,
         "fix 134c: the resolves' every command through the recorder");
  const size_t in_stream = vk.find("host_query_pool_->RecordResolveBatch(deferred_command_buffer_);", end);
  const size_t in_stream_if = vk.find("if (async_query_resolves && async_settings_.resolves &&", end);
  Expect(in_stream_if != std::string::npos && in_stream_if < in_stream && in_stream < hand &&
             vk.find("async_submit::HandOff(async_settings_.mode, is_swap,", in_stream_if) < in_stream,
         "fix 134c: the resolves go to the deferred commands before the hand-off, only when it hands off");
  Expect(vk.find("host_query_pool_->RecordResolveBatch(command_buffer.buffer);", execute) < submit,
         "fix 134c: else recorded after the replay as before");
  Expect(vk.find("xbox360ps5::frame_cost::async_kept[kept].fetch_add(1, std::memory_order_relaxed);", end) < hand &&
             vk.find("query resolves {} (fix 134c)") != std::string::npos,
         "fix 134c: why a submission stays, counted; the settings line");
  // Fix 134e.
  const size_t refresh = vk.find("void VulkanCommandProcessor::RefreshAsyncSubmitSettings() {");
  const size_t refresh_end = vk.find("\nbool VulkanCommandProcessor::", refresh + 1);
  const std::string refresh_body = vk.substr(refresh, refresh_end - refresh);
  Expect(refresh != std::string::npos &&
             refresh_body.find("next.mode = async_submit::ModeFor(cvars::gpu_async_submit, title_id, &from_preset);") <
                 refresh_body.find("not_understood = async_submit::Parse(text, next);") &&
             refresh_body.find("const uint32_t title_id = kernel_state_ ? kernel_state_->title_id() : 0;") !=
                 std::string::npos &&
             refresh_body.find("from_preset = false;  // fix 134e") != std::string::npos &&
             refresh_body.find("\"the preset for title {:08X} (fix 134e)\"") != std::string::npos &&
             vk.find("DEFINE_uint32(gpu_async_submit, 0,") != std::string::npos,
         "fix 134e: the preset taken while the cvar is 0, before the file, and named in the settings line");
  const std::string fc = Read("include/xbox360ps5/frame_cost.hpp");
  Expect(fc.find("\"; kept on the GPU thread per frame: swap %.2f, sparse binds %.2f, query resolves %.2f; \"") !=
             std::string::npos,
         "fix 134c: the 30 s line says why submissions stayed");
  Expect(fc.find("\"; wait drains by line: \"") != std::string::npos &&
             vk.find("xbox360ps5::frame_cost::NoteAsyncWaitLine(xbox360ps5::frame_cost::wait_line);") != std::string::npos,
         "fix 134b: the wait drains by line");
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
  CheckNeedsDrain();
  CheckPreset();
  CheckKept();
  CheckModel();
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-async-submit: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-async-submit: ok\n");
  return 0;
}
