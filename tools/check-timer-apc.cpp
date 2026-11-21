// SPDX-License-Identifier: MIT
// Fix 97 (tools/source-1.0.2/fixes/97-timer-apc.patch): a guest timer's APC routine queued from
// the host. Brian Lara International Cricket 2007 (434D07D8, issue 16) crashed about five seconds
// after the start on 1.0.4 and 1.0.4.1: a read fault at 0x20 in XThread::EnqueueApc, called from
// the timer's completion routine on the POSIX TimerQueue thread. That host thread has no guest
// ThreadState, and EnqueueApc queued with cpu::ThreadState::Get()->context(). The XTimer callback
// also kept a raw pointer to the thread that set the timer, which may end before it fires.
// The assembled tree:
//  (a) xtimer.cc: the callback keeps the thread's handle and the kernel, and queues through
//      KernelState::QueueApcFromHost; no raw thread pointer is used in it;
//  (b) xthread.cc: EnqueueApc on a thread without a ThreadState hands the APC to the relay, and
//      otherwise queues with the current thread's context, read once;
//  (c) kernel_state.cc: QueueApcFromHost takes the relay's own lock and never the global one
//      (closing a timer under the global lock waits in Disarm for a callback in progress), and
//      drops requests once the relay stopped;
//  (d) the relay: a kernel thread with a guest context, waiting on its own condition, finding the
//      thread by handle and skipping one that ended, started once by the first timer with an APC
//      routine (EnsureApcRelay, on the guest thread, before the timer is armed; not at boot: the
//      c37 smoke run had it in every game), stopped and waited for in ~KernelState.
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
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
// The text from `start` to the end of its function (the next line that is just "}").
std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
const std::string K = ".deps/xenia-canary/src/xenia/kernel/";

void CheckTimer() {
  const std::string timer = Read(K + "xtimer.cc");
  const std::string set = Body(timer, "X_STATUS XTimer::SetTimer(");
  Expect(Has(timer, "#include \"xenia/kernel/kernel_state.h\""), "xtimer.cc sees KernelState");
  Expect(Has(set, "const uint32_t cb_thread_handle =\n        callback_thread_ ? callback_thread_->handle() : 0;") &&
             Has(set, "callback = [kernel, cb_thread_handle, cb_routine, cb_routine_arg]() {"),
         "the timer callback keeps the thread's handle and the kernel");
  Expect(Has(set, "kernel->QueueApcFromHost(cb_thread_handle, cb_routine, cb_routine_arg,\n"
                  "                               time_low, time_high);"),
         "the timer callback queues through the kernel's relay");
  const size_t ensure = set.find("    auto* kernel = kernel_state();\n    kernel->EnsureApcRelay();");
  const size_t armed = set.find("result = timer_->SetOnceAt(");
  Expect(ensure != std::string::npos && armed != std::string::npos && ensure < armed,
         "a timer with an APC routine starts the relay on the guest thread, before it is armed");
  Expect(!Has(set, "cb_thread->") && !Has(set, "auto cb_thread = callback_thread_;"),
         "no raw thread pointer in the timer callback");
}

void CheckThread() {
  const std::string thread = Read(K + "xthread.cc");
  const std::string enqueue = Body(thread, "void XThread::EnqueueApc(");
  const size_t state = enqueue.find("auto* current_state = cpu::ThreadState::Get();");
  const size_t fallback = enqueue.find(
      "  if (!current_state) {\n    kernel_state()->QueueApcFromHost(this->handle(), normal_routine,\n"
      "                                     normal_context, arg1, arg2);\n    return;\n  }");
  const size_t queue = enqueue.find("xboxkrnl::xeNtQueueApcThread(");
  Expect(state != std::string::npos && fallback != std::string::npos && queue != std::string::npos && state < fallback &&
             fallback < queue,
         "EnqueueApc without a ThreadState hands the APC to the relay before queueing");
  Expect(Has(enqueue, "      current_state->context());") && !Has(enqueue, "cpu::ThreadState::Get()->context()"),
         "otherwise it queues with the current thread's context, read once");
}

void CheckKernel() {
  const std::string header = Read(K + "kernel_state.h");
  Expect(Has(header, "#include <mutex>  // fix 97") &&
             Has(header, "  void QueueApcFromHost(uint32_t thread_handle, uint32_t routine,") &&
             Has(header, "  std::condition_variable host_apc_cond_;") &&
             Has(header, "  bool host_apc_running_ = false;   // guarded by host_apc_mutex_") &&
             Has(header, "  object_ref<XHostThread> host_apc_thread_;"),
         "kernel_state.h declares the relay");

  const std::string kernel = Read(K + "kernel_state.cc");
  const std::string queue = Body(kernel, "void KernelState::QueueApcFromHost(");
  Expect(Has(queue, "std::lock_guard<std::mutex> lock(host_apc_mutex_);") && !Has(queue, "global_critical_region") &&
             !Has(queue, "global_lock"),
         "QueueApcFromHost takes the relay's lock, never the global one");
  const size_t stopped = queue.find("      return;\n    }\n    host_apcs_.push_back(", queue.find("    if (!host_apc_running_) {"));
  const size_t push = queue.find("host_apcs_.push_back({thread_handle, routine, routine_context, arg1, arg2});");
  Expect(stopped != std::string::npos && push != std::string::npos && stopped < push &&
             queue.find("host_apc_cond_.notify_one();", push) != std::string::npos,
         "requests are dropped once the relay stopped, otherwise queued and the relay woken");

  const std::string start = Body(kernel, "void KernelState::EnsureApcRelay() {");
  const size_t relay = kernel.find("  if (!host_apc_thread_) {", kernel.find("void KernelState::EnsureApcRelay() {"));
  const size_t named = kernel.find("    host_apc_thread_->set_name(\"Kernel APC relay\");", relay);
  Expect(Has(start, "  std::lock_guard<std::mutex> start(host_apc_start_mutex_);\n  if (!host_apc_thread_) {") &&
             Has(start, "    host_apc_thread_->Create();") && named != std::string::npos,
         "the relay is started once, by EnsureApcRelay, under its own lock");
  Expect(!Has(kernel.substr(0, kernel.find("void KernelState::EnsureApcRelay() {")), "host_apc_thread_ = object_ref<XHostThread>"),
         "not at boot: a game with no APC timer has no relay thread");
  Expect(Has(header, "  void EnsureApcRelay();") && Has(header, "  std::mutex host_apc_start_mutex_;   // EnsureApcRelay") &&
             Has(queue, "Timer APC (fix 97): routine {:08X} for thread {:08X} asked for \"") &&
             queue.find("host_apcs_unrelayed_++ == 0") < queue.find("host_apcs_.push_back("),
         "before the relay starts, a host thread's APC is dropped and logged once");
  const std::string loop = relay == std::string::npos ? "" : kernel.substr(relay, named - relay);
  Expect(Has(loop, "new XHostThread(") && Has(loop, "GetSystemProcess()));"), "the relay has a guest context (XHostThread)");
  Expect(Has(loop, "host_apc_cond_.wait(lock, [this] {\n                return !host_apcs_.empty() || !host_apc_running_;") &&
             Has(loop, "batch.swap(host_apcs_);") && !Has(loop, "global_critical_region") && !Has(loop, "dispatch_cond_"),
         "the relay waits on its own condition and takes the requests in one swap");
  const size_t lookup = loop.find("object_table()->LookupObject<XThread>(apc.thread_handle);");
  const size_t skip = loop.find("if (!thread) {", lookup);
  const size_t enqueue = loop.find("thread->EnqueueApc(apc.routine, apc.routine_context, apc.arg1,", skip);
  Expect(lookup != std::string::npos && skip != std::string::npos && enqueue != std::string::npos &&
             loop.find("continue;", skip) < enqueue,
         "the relay finds the thread by handle and skips one that ended");
  Expect(Has(loop, "Timer APC (fix 97): thread {:08X} ended before its") &&
             Has(loop, "Timer APC (fix 97): routine {:08X} for thread {:08X} "),
         "the first skipped and the first relayed APC are logged");

  const std::string destructor = Body(kernel, "KernelState::~KernelState() {");
  const size_t stop = destructor.find("      host_apc_running_ = false;");
  Expect(stop != std::string::npos && destructor.find("    host_apc_cond_.notify_all();", stop) != std::string::npos &&
             destructor.find("    host_apc_thread_->Wait(0, 0, 0, nullptr);", stop) != std::string::npos,
         "~KernelState stops the relay and waits for it");
}

}  // namespace

int main() {
  CheckTimer();
  CheckThread();
  CheckKernel();
  if (failures) {
    std::printf("check-timer-apc: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-timer-apc: ok\n");
  return 0;
}
