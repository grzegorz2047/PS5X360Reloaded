// SPDX-License-Identifier: MIT
// Fix 156b (tools/source-1.0.2/fixes/156b-kernel-timers.patch; Kinect Sports Season Two
// 4D5309D6 and Zumba Fitness Core 4D4A07F0, issue 63): KeSetTimer, KeSetTimerEx and
// KeCancelTimer on a KTIMER the title initialised itself (dispatch header type 8 or 9). They
// were undefined externs ("undefined extern call to ... KeSetTimer"), the type-9 header had no
// object ("dispatch header type 09 has no object to create"), KeWaitForMultipleObjects failed
// at once and Season Two's helper thread spun on it, about 550k calls a second, while tennis
// never finished loading.
//
//   check-kernel-timer        run from an assembled tree's root
//
//  (a) xbox360ps5/kernel_timer.hpp, the guest-visible state: KeSetTimer(Ex) returns whether
//      the timer was set before and clears the signal state; a firing of an older arming is
//      dropped; a one-shot timer leaves the queue when it fires, a periodic one stays; cancel
//      returns whether it was set and keeps the signal state; a satisfied wait resets a
//      synchronization timer only; types 8 and 9 only;
//  (b) waits through the core's own wait objects (xenia/base/posix_conditions.h): a host timer
//      with the three members of threading_posix.cc's PosixCondition<Timer> (checked against
//      the tree), fired by a thread as the timer queue fires it (signal, then the callback's
//      Fire): one-shot notification and synchronization, cancel before the due time, a
//      re-arm before the first due time, periodic, and WaitAny / WaitAll together with an
//      event;
//  (c) the tree: types 8 and 9 get an XTimer in XObject::GetNativeObject; the three exports
//      (xboxkrnl ordinals 0x54, 0xA6, 0xA7, matched by the import as every export is);
//      KeSetTimer is KeSetTimerEx with no period; the host timer's callback takes no lock,
//      fires through the state and hands a DPC to the kernel's relay, which runs it as a DPC;
//      the relay is started on the guest thread before the timer is armed; NtSetTimerEx's
//      XTimer::SetTimer keeps its due time conversion (shared).
#undef NDEBUG  // assert_true and xe::AssertFailed as in a debug build
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// (b) needs the POSIX wait objects: a Windows host build (zig, for a quick local run) leaves it out.
#if !defined(_WIN32)
#define CHECK_WAITS 1
#include <pthread.h>
#include <unistd.h>

namespace xe {
namespace threading {
// threading_posix.cc blocks the suspend signal here; this check has no suspension.
class SuspensionDeferred {
 public:
  SuspensionDeferred() {}
};
}  // namespace threading
}  // namespace xe

#include "xenia/base/posix_conditions.h"
#include "xbox360ps5/kernel_timer.hpp"

// assert_true's failure path in debug builds (xenia/base/assert.h).
void xe::AssertFailed(const char* expression, const char* file, int line) {
  std::printf("FAIL: assertion %s at %s:%d\n", expression, file, line);
  std::fflush(stdout);
  std::abort();
}
#else
#define CHECK_WAITS 0
#include "xbox360ps5/kernel_timer.hpp"
#endif

namespace {

namespace kt = xbox360ps5::kernel_timer;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

std::string Read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  std::string out;
  for (char c : text.str())
    if (c != '\r') out += c;
  return out;
}
std::string Body(const std::string& text, const std::string& start) {
  const size_t at = text.find(start);
  if (at == std::string::npos) return "";
  const size_t end = text.find("\n}\n", at);
  return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}
bool Has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}

// (a) the state alone.
void CheckState() {
  std::printf("guest-visible state\n");
  kt::State s;
  kt::Armed first = kt::Arm(s, 0);
  CHECK(!first.was_inserted && s.inserted && !s.signaled && !s.periodic);
  kt::Armed again = kt::Arm(s, 0);
  CHECK(again.was_inserted && again.generation != first.generation);
  CHECK(!kt::Fire(s, first.generation));  // the older arming
  CHECK(!s.signaled && s.inserted && s.dropped == 1);
  CHECK(kt::Fire(s, again.generation));
  CHECK(s.signaled && !s.inserted && s.fired == 1);  // a one-shot timer left the queue
  CHECK(!kt::Fire(s, again.generation));             // fired once
  CHECK(!kt::Cancel(s));                             // not set any more
  CHECK(s.signaled);                                 // cancel keeps the signal state
  kt::Consumed(s, false);
  CHECK(s.signaled);  // a notification timer stays signalled
  kt::Consumed(s, true);
  CHECK(!s.signaled);  // a synchronization timer is reset by the wait

  kt::Armed periodic = kt::Arm(s, 16);
  CHECK(!periodic.was_inserted && s.periodic && !s.signaled);
  CHECK(kt::Fire(s, periodic.generation) && kt::Fire(s, periodic.generation));
  CHECK(s.inserted && s.signaled);  // a periodic timer stays in the queue
  CHECK(kt::Cancel(s));
  CHECK(!s.inserted && !kt::Fire(s, periodic.generation));
  kt::Armed set = kt::Arm(s, 0);
  CHECK(!s.signaled && !set.was_inserted);  // setting clears the signal state

  CHECK(kt::IsTimerType(8) && kt::IsTimerType(9));
  CHECK(!kt::IsTimerType(0) && !kt::IsTimerType(1) && !kt::IsTimerType(5) && !kt::IsTimerType(0x13));
}

#if CHECK_WAITS
using xe::threading::PosixCondition;
using xe::threading::PosixConditionBase;
using xe::threading::WaitResult;

// threading_posix.cc's PosixCondition<Timer>: Signal, signaled and post_execution (the tree
// is checked to still have them), plus the reset SetOnce/SetRepeating do.
class HostTimer final : public PosixConditionBase {
 public:
  explicit HostTimer(bool manual_reset) : manual_reset_(manual_reset) {}
  bool Signal() override {
    xe::threading::SuspensionDeferred condition_suspend_guard;
    std::lock_guard lock(mutex_);
    signal_ = true;
    cond_.notify_all();
    NotifyMultiWaiters();
    NotifyAnyLegacy();
    return true;
  }
  void ResetForArm() {
    std::lock_guard lock(mutex_);
    signal_ = false;
  }

 protected:
  [[nodiscard]] bool signaled() const override { return signal_; }
  void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }

 private:
  bool signal_ = false;
  const bool manual_reset_;
};

// A KTIMER as XTimer backs it: the host timer, the shared state, and a thread per arming
// standing in for the timer queue (a cancelled or replaced arming is disarmed: no signal).
class GuestTimer {
 public:
  explicit GuestTimer(bool synchronization)
      : synchronization_(synchronization), host_(!synchronization) {}
  ~GuestTimer() {
    kt::Cancel(state_);
    for (auto& t : threads_) t.join();
  }
  bool Set(std::chrono::milliseconds due, std::chrono::milliseconds period = 0ms) {
    const kt::Armed armed = kt::Arm(state_, uint32_t(period.count()));
    host_.ResetForArm();
    const uint32_t generation = armed.generation;
    threads_.emplace_back([this, due, period, generation] {
      auto at = Clock::now() + due;
      for (;;) {
        std::this_thread::sleep_until(at);
        if (state_.generation.load() != generation) return;  // disarmed
        host_.Signal();
        kt::Fire(state_, generation);
        if (period == 0ms) return;
        at += period;
      }
    });
    return armed.was_inserted;
  }
  bool Cancel() { return kt::Cancel(state_); }
  // A wait that took this timer (XTimer::WaitCallback).
  void Took() { kt::Consumed(state_, synchronization_); }
  HostTimer& host() { return host_; }
  kt::State& state() { return state_; }

 private:
  const bool synchronization_;
  HostTimer host_;
  kt::State state_;
  std::vector<std::thread> threads_;
};

using Event = PosixCondition<xe::threading::Event>;

std::pair<WaitResult, size_t> Wait(std::vector<PosixConditionBase*> handles, bool all,
                                   std::chrono::milliseconds timeout) {
  return PosixConditionBase::WaitMultiple(std::move(handles), all, timeout);
}

// (b) waits through the core's wait objects.
void CheckWaits() {
  std::printf("one-shot notification timer\n");
  {
    GuestTimer timer(false);
    const auto start = Clock::now();
    CHECK(!timer.Set(30ms));
    CHECK(Wait({&timer.host()}, false, 2000ms).first == WaitResult::kSuccess);
    CHECK(Clock::now() - start >= 25ms);
    timer.Took();
    CHECK(timer.state().signaled && !timer.state().inserted);
    CHECK(Wait({&timer.host()}, false, 0ms).first == WaitResult::kSuccess);  // stays signalled
    CHECK(!timer.Cancel());
  }
  std::printf("one-shot synchronization timer\n");
  {
    GuestTimer timer(true);
    timer.Set(20ms);
    CHECK(Wait({&timer.host()}, false, 2000ms).first == WaitResult::kSuccess);
    timer.Took();
    CHECK(!timer.state().signaled);
    CHECK(Wait({&timer.host()}, false, 40ms).first == WaitResult::kTimeout);  // reset by the wait
  }
  std::printf("cancelled before its due time\n");
  {
    GuestTimer timer(true);
    timer.Set(60ms);
    CHECK(timer.Cancel());
    CHECK(Wait({&timer.host()}, false, 150ms).first == WaitResult::kTimeout);
    CHECK(timer.state().fired == 0 && !timer.Cancel());
  }
  std::printf("re-armed before its first due time\n");
  {
    GuestTimer timer(false);
    timer.Set(30ms);
    CHECK(timer.Set(250ms));  // it was set
    CHECK(Wait({&timer.host()}, false, 120ms).first == WaitResult::kTimeout);  // 30 ms dropped
    CHECK(Wait({&timer.host()}, false, 2000ms).first == WaitResult::kSuccess);
    CHECK(timer.state().fired == 1);
  }
  std::printf("periodic synchronization timer\n");
  {
    GuestTimer timer(true);
    timer.Set(10ms, 10ms);
    int taken = 0;
    const auto until = Clock::now() + 300ms;
    while (Clock::now() < until) {
      if (Wait({&timer.host()}, false, 100ms).first == WaitResult::kSuccess) {
        timer.Took();
        ++taken;
      }
    }
    CHECK(taken >= 5);
    CHECK(timer.state().inserted);  // still set
    CHECK(timer.Cancel());
    std::this_thread::sleep_for(30ms);  // a firing already in its sleep sees the cancel
    const uint64_t fired = timer.state().fired;
    std::this_thread::sleep_for(60ms);
    CHECK(timer.state().fired == fired);
  }
  std::printf("WaitAny with an event\n");
  {
    Event event(false, false);
    GuestTimer timer(true);
    timer.Set(30ms);
    auto r = Wait({&event, &timer.host()}, false, 2000ms);
    CHECK(r.first == WaitResult::kSuccess && r.second == 1);  // the timer
    timer.Set(400ms);
    std::thread setter([&] {
      std::this_thread::sleep_for(20ms);
      event.Signal();
    });
    r = Wait({&event, &timer.host()}, false, 2000ms);
    setter.join();
    CHECK(r.first == WaitResult::kSuccess && r.second == 0);  // the event, before the timer
    CHECK(timer.Cancel());
  }
  std::printf("WaitAll with an event\n");
  {
    Event event(true, false);
    GuestTimer timer(false);
    timer.Set(20ms);
    const auto start = Clock::now();
    std::thread setter([&] {
      std::this_thread::sleep_for(80ms);
      event.Signal();
    });
    CHECK(Wait({&event, &timer.host()}, true, 2000ms).first == WaitResult::kSuccess);
    setter.join();
    CHECK(Clock::now() - start >= 70ms);  // both: the timer first, then the event
    event.Reset();
    event.Signal();
    GuestTimer cancelled(false);
    cancelled.Set(30ms);
    CHECK(cancelled.Cancel());
    CHECK(Wait({&event, &cancelled.host()}, true, 120ms).first == WaitResult::kTimeout);
  }
}
#endif  // CHECK_WAITS

const std::string K = ".deps/xenia-canary/src/xenia/kernel/";

// (c) the tree.
void CheckTree() {
  std::printf("the assembled tree\n");
  const std::string posix = Read(".deps/xenia-canary/src/xenia/base/threading_posix.cc");
  const std::string object = Read(K + "xobject.cc");
  const std::string threading = Read(K + "xboxkrnl/xboxkrnl_threading.cc");
  const std::string table = Read(K + "xboxkrnl/xboxkrnl_table.inc");
  const std::string timer_h = Read(K + "xtimer.h");
  const std::string timer = Read(K + "xtimer.cc");
  const std::string kernel = Read(K + "kernel_state.cc");
  const std::string kernel_h = Read(K + "kernel_state.h");
  if (posix.empty() || object.empty() || threading.empty() || timer.empty() || kernel.empty()) {
    std::printf("FAIL: no assembled tree here\n");
    ++failures;
    return;
  }
  // The host timer used in (b) is the core's.
  const size_t cls = posix.find("class PosixCondition<Timer> final : public PosixConditionBase {");
  const size_t cls_end = posix.find("\n};\n", cls);
  const std::string host = cls == std::string::npos ? "" : posix.substr(cls, cls_end - cls);
  CHECK(Has(host, "    std::lock_guard lock(mutex_);\n    signal_ = true;\n    cond_.notify_all();\n"
                  "    NotifyMultiWaiters();"));
  CHECK(Has(host, "  [[nodiscard]] bool signaled() const override { return signal_; }\n"
                  "  void post_execution() override {\n    if (!manual_reset_) {\n      signal_ = false;"));
  CHECK(Has(host, "    callback_ = std::move(opt_callback);\n    signal_ = false;"));  // arming resets
  CHECK(Has(host, "    timer->Signal();\n"));  // CompletionRoutine signals before the callback
  CHECK(host.find("    timer->Signal();\n") < host.find("      callback();"));

  const std::string create = Body(object, "object_ref<XObject> XObject::GetNativeObject(");
  CHECK(Has(create, "        case X_OBJECT_TYPES::TimerNotificationObject:\n"
                    "        case X_OBJECT_TYPES::TimerSynchronizationObject: {\n"));
  CHECK(Has(create, "          auto timer = new XTimer(kernel_state);\n"
                    "          timer->InitializeNative(native_ptr, header);\n          return timer;"));
  CHECK(Has(object, "#include \"xenia/kernel/xtimer.h\""));

  CHECK(Has(table, "XE_EXPORT(xboxkrnl, 0x00000054, KeCancelTimer,"));
  CHECK(Has(table, "XE_EXPORT(xboxkrnl, 0x000000A6, KeSetTimer,"));
  CHECK(Has(table, "XE_EXPORT(xboxkrnl, 0x000000A7, KeSetTimerEx,"));
  CHECK(Has(threading, "DECLARE_XBOXKRNL_EXPORT1(KeSetTimerEx, kThreading, kImplemented);"));
  CHECK(Has(threading, "DECLARE_XBOXKRNL_EXPORT1(KeSetTimer, kThreading, kImplemented);"));
  CHECK(Has(threading, "DECLARE_XBOXKRNL_EXPORT1(KeCancelTimer, kThreading, kImplemented);"));
  const std::string set_ex = Body(threading, "dword_result_t KeSetTimerEx_entry(");
  CHECK(Has(set_ex, "dword_t period, lpvoid_t dpc)") &&
        Has(set_ex, "SetNativeTimer(timer, uint64_t(due_time), uint32_t(period), dpc.guest_address(),"));
  const std::string set = Body(threading, "dword_result_t KeSetTimer_entry(");
  CHECK(Has(set, "SetNativeTimer(timer, uint64_t(due_time), 0, dpc.guest_address(), \"KeSetTimer\")"));
  const std::string cancel = Body(threading, "dword_result_t KeCancelTimer_entry(");
  CHECK(Has(cancel, "object->CancelNative();") && Has(cancel, "return was_set ? 1 : 0;"));
  const std::string lookup = Body(threading, "object_ref<XTimer> NativeTimer(");
  CHECK(Has(lookup, "xbox360ps5::kernel_timer::IsTimerType(uint32_t(timer->header.type))"));
  CHECK(Has(lookup, "object->type() != XObject::Type::Timer") && Has(lookup, "is_native()"));
  CHECK(Has(threading, "Kernel timer (fix 156b): {}({:08X}, due {:016X}, period {} ms, DPC {:08X}) -> "));

  CHECK(Has(timer_h, "#include \"xbox360ps5/kernel_timer.hpp\"") &&
        Has(timer_h, "  void WaitCallback() override;") &&
        Has(timer_h, "  std::shared_ptr<xbox360ps5::kernel_timer::State> native_state_;"));
  const std::string init = Body(timer, "void XTimer::InitializeNative(");
  CHECK(Has(init, "header->type == X_OBJECT_TYPES::TimerSynchronizationObject;") &&
        Has(init, "xe::threading::Timer::CreateSynchronizationTimer()") &&
        Has(init, "xe::threading::Timer::CreateManualResetTimer()") &&
        Has(init, "SetNativePointer(memory()->HostToGuestVirtual(native_ptr), true);"));
  const std::string native = Body(timer, "bool XTimer::SetNative(");
  CHECK(Has(native, "const kt::Armed armed = kt::Arm(*native_state_, period_ms);") &&
        Has(native, "ktimer->header.signal_state = 0;\n  ktimer->header.inserted = 1;") &&
        Has(native, "ktimer->dpc = dpc_ptr;") && Has(native, "return armed.was_inserted;"));
  const size_t relay = native.find("kernel->EnsureApcRelay();");
  const size_t armed = native.find("timer_->SetOnceAt(due_tp, std::move(callback));");
  CHECK(relay != std::string::npos && armed != std::string::npos && relay < armed);
  CHECK(Has(native, "Clock::ScaleGuestDurationMillis(period_ms)"));
  const size_t cb = native.find("std::function<void()> callback = [");
  const size_t cb_end = native.find("\n  };\n", cb);
  const std::string callback = cb == std::string::npos ? "" : native.substr(cb, cb_end - cb);
  CHECK(!callback.empty() && !Has(callback, "lock_guard") && !Has(callback, "unique_lock") &&
        !Has(callback, "_lock") && !Has(callback, "mutex") && !Has(callback, "global") &&
        !Has(callback, "this"));
  CHECK(callback.find("xbox360ps5::kernel_timer::Fire(*state, generation)") <
        callback.find("fired->header.signal_state = 1;"));
  CHECK(Has(callback, "if (!state->periodic.load()) fired->header.inserted = 0;") &&
        Has(callback, "kernel->QueueDpcFromHost(dpc_ptr, uint32_t(time), uint32_t(time >> 32));"));
  const std::string cancel_native = Body(timer, "bool XTimer::CancelNative(");
  CHECK(Has(cancel_native, "xbox360ps5::kernel_timer::Cancel(*native_state_);") &&
        Has(cancel_native, "timer_->Cancel();") && Has(cancel_native, "header.inserted = 0;"));
  const std::string wait = Body(timer, "void XTimer::WaitCallback(");
  CHECK(Has(wait, "!synchronization_") && Has(wait, "header.signal_state = 0;"));
  const std::string nt_set = Body(timer, "X_STATUS XTimer::SetTimer(");
  CHECK(Has(nt_set, "const WinSystemClock::time_point due_tp = GuestDueTime(due_time);"));
  CHECK(Has(Body(timer, "xe::chrono::WinSystemClock::time_point GuestDueTime("),
            "auto after = xe::chrono::hundrednanoseconds(-due_time);"));

  CHECK(Has(kernel_h, "  void QueueDpcFromHost(uint32_t dpc_ptr, uint32_t arg1, uint32_t arg2);") &&
        Has(kernel_h, "    uint32_t dpc = 0;  // fix 156b"));
  const std::string queue = Body(kernel, "void KernelState::QueueDpcFromHost(");
  CHECK(Has(queue, "std::lock_guard<std::mutex> lock(host_apc_mutex_);") &&
        !Has(queue, "global") && Has(queue, "host_apcs_.push_back(dpc);") &&
        Has(queue, "host_apc_cond_.notify_one();"));
  const size_t loop = kernel.find("for (const HostApc& apc : batch) {");
  const size_t dpc = kernel.find("if (apc.dpc) {  // fix 156b", loop);
  const size_t thread = kernel.find("object_table()->LookupObject<XThread>(apc.thread_handle);", loop);
  CHECK(loop != std::string::npos && dpc != std::string::npos && thread != std::string::npos &&
        dpc < thread);
  const std::string run = Body(kernel, "void KernelState::RunTimerDpc(");
  const size_t begin = run.find("BeginDPCImpersonation(context, dpc_scope);");
  const size_t exec = run.find("processor_->Execute(thread->thread_state(), routine, args,");
  const size_t end = run.find("EndDPCImpersonation(context, dpc_scope);");
  CHECK(begin != std::string::npos && exec != std::string::npos && end != std::string::npos &&
        begin < exec && exec < end);
  CHECK(Has(run, "uint64_t args[] = {dpc_ptr, uint32_t(dpc->context), arg1, arg2};"));
}

}  // namespace

int main() {
  // A hang fails the check instead of the job's time limit.
  std::thread([] {
    std::this_thread::sleep_for(60s);
    std::printf("FAIL: check-kernel-timer did not finish within 60 s\n");
    std::fflush(stdout);
    std::_Exit(1);
  }).detach();
  CheckState();
#if CHECK_WAITS
  CheckWaits();
#else
  std::printf("skip the waits (no POSIX wait objects on this host)\n");
#endif
  CheckTree();
  if (failures) {
    std::printf("check-kernel-timer: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-kernel-timer: ok\n");
  return 0;
}
