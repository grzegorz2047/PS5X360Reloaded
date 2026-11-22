// SPDX-License-Identifier: MIT
// Fix 129 (xenia/vfs/entry_io.h, xenia/vfs/entry.cc): a file system entry's host operation runs
// without the global critical region. Kinect Sports (4D5308C9) on 1.0.5-work-fix128: thread
// F8000078 held the region in Entry::CreateEntry and Entry::Delete (0xd07047, 0xd07171) for up to
// 961 ms while it created and deleted its replay files on the console's disk, 572-710 ms/s, and
// the GPU thread waited 540-667 ms/s for it in SharedMemory::RequestRange.
//  (a) OutsideLock: while a 300 ms host operation runs, another thread takes the region at once;
//      the caller holds it again afterwards, also when the operation throws;
//  (b) a caller that already holds the (recursive) region keeps it across the operation, as
//      before: no other thread gets it in between;
//  (c) the counters' line, and none when nothing happened;
//  (d) the assembled tree: CreateEntry and Delete run the host operation through OutsideLock on
//      every platform (so check-content-create's real VFS create runs it too), check the
//      children again after creating, mark an entry being deleted, and the summary logs the line.
// The tree part runs from the assembled 1.0.2 tree's root (run-fix-checks.sh).
#undef NDEBUG
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "xenia/vfs/entry_io.h"

namespace {

namespace io = xe::vfs::entry_io;
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
using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::recursive_mutex region;

// How long another thread waits for the region while `body` runs on this one.
template <class Body>
double OtherWaitMs(Body body) {
  std::atomic<bool> started{false};
  double waited = 0;
  std::thread other([&] {
    while (!started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the host operation is under way
    const auto t = Clock::now();
    region.lock();
    waited = MsSince(t);
    region.unlock();
  });
  started = true;
  body();
  other.join();
  return waited;
}

void CheckOutsideLock() {
  io::Reporter reporter;
  Expect(reporter.Line().empty(), "no line without host operations");
  bool held_after = false;
  int result = 0;
  const double waited = OtherWaitMs([&] {
    std::unique_lock<std::recursive_mutex> lock(region);
    result = io::OutsideLock(lock, io::kCreate, [] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      return 42;
    });
    held_after = lock.owns_lock();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  });
  std::printf("a 300 ms host operation: another thread waited %.1f ms for the region\n", waited);
  Expect(result == 42 && held_after, "the operation's result, and the region held again after it");
  Expect(waited < 100, "another thread takes the region during the operation");

  // Already held by the caller: kept, as before.
  const double waited_held = OtherWaitMs([&] {
    std::unique_lock<std::recursive_mutex> outer(region);
    std::unique_lock<std::recursive_mutex> lock(region);
    io::OutsideLock(lock, io::kDelete, [] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      return true;
    });
  });
  std::printf("the same with the region held by the caller: %.1f ms\n", waited_held);
  Expect(waited_held >= 200, "a caller's own hold is kept across the operation");

  // A throwing operation still takes the lock back.
  bool caught = false, held = false;
  {
    std::unique_lock<std::recursive_mutex> lock(region);
    try {
      io::OutsideLock(lock, io::kDelete, []() -> bool { throw std::runtime_error("host"); });
    } catch (const std::runtime_error&) {
      caught = true;
      held = lock.owns_lock();
    }
  }
  Expect(caught && held, "the region is taken back when the operation throws");

  const std::string line = reporter.Line();
  std::printf("%s\n", line.c_str());
  Expect(line.find("VFS host operations (fix 129, without the global lock): 1 creates (") == 0 &&
             line.find("1 deletes (") != std::string::npos && line.find("longest 30") != std::string::npos,
         "the line counts the creates and deletes (not one that threw) with the longest");
  Expect(reporter.Line().empty(), "the next line only when there were more");
}

void CheckTree() {
  const std::string vfs = ".deps/xenia-canary/src/xenia/vfs/";
  const std::string cc = Read(vfs + "entry.cc"), h = Read(vfs + "entry.h");
  Expect(cc.find("#include \"xenia/vfs/entry_io.h\"  // fix 129") != std::string::npos &&
             h.find("bool deleting_ = false;") != std::string::npos,
         "entry.cc uses entry_io.h; an entry can be marked while it is deleted");
  const size_t create = cc.find("Entry* Entry::CreateEntry(");
  const size_t del = cc.find("bool Entry::Delete(Entry* entry) {");
  const size_t del_end = cc.find("bool Entry::Delete() {", del);
  Expect(create != std::string::npos && del != std::string::npos && del_end != std::string::npos, "the functions");
  const std::string create_body = cc.substr(create, del - create), delete_body = cc.substr(del, del_end - del);
  Expect(create_body.find("entry_io::OutsideLock(global_lock, entry_io::kCreate,") != std::string::npos &&
             create_body.find("if (!entry || GetChild(name)) {") != std::string::npos,
         "CreateEntry creates without the lock and checks the children again");
  Expect(delete_body.find("if (entry->deleting_) {") < delete_body.find("entry->deleting_ = true;") &&
             delete_body.find("entry_io::OutsideLock(") != std::string::npos &&
             delete_body.find("entry->deleting_ = false;") != std::string::npos &&
             delete_body.find("children_.erase(it);") > delete_body.find("entry_io::OutsideLock("),
         "Delete marks the entry, deletes without the lock, and erases it under the lock after");
  Expect(create_body.find("#else") == std::string::npos && delete_body.find("#else") == std::string::npos,
         "on every platform, so check-content-create's real VFS create runs it too");
  const std::string game_main = Read("src/canary_game_main.cpp");
  Expect(game_main.find("static xe::vfs::entry_io::Reporter vfs_host_operations;") != std::string::npos,
         "the summary logs the line");
}

}  // namespace

int main(int argc, char** argv) {
  CheckOutsideLock();
  if (!(argc > 1 && std::string(argv[1]) == "--model-only")) CheckTree();
  if (failures) {
    std::printf("check-vfs-host-ops: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-vfs-host-ops: ok\n");
  return 0;
}
