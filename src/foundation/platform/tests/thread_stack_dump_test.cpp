#include "foundation/platform/os/OsThreadStackDump.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

// B79: the first-present watchdog calls this while the UI thread may be stuck; it must come back and
// leave the other threads running. Output goes to stderr and is not checked here.
TEST(ThreadStackDumpTest, DumpsAndReturnsWithOtherThreadsAlive) {
  std::atomic<bool> stop{false};
  std::atomic<int> ticks{0};
  std::thread worker([&] {
    while (!stop.load()) {
      ++ticks;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  pbr::os::DumpAllThreadStacks();
  const int before = ticks.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_GT(ticks.load(), before) << "the worker must keep running after being signalled";
  stop = true;
  worker.join();
}
