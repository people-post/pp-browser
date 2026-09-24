#include "foundation/runtime/TaskGate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using pbr::TaskGate;
using namespace std::chrono_literals;

TEST(TaskGateTest, GuardedRunsWhileOpenAndNoOpsAfterClose) {
  TaskGate gate;
  int runs = 0;
  auto task = gate.Guard([&] { ++runs; });
  task();
  EXPECT_EQ(runs, 1);
  EXPECT_TRUE(gate.CloseAndWait(10ms));
  EXPECT_TRUE(gate.IsClosed());
  task();
  EXPECT_EQ(runs, 1) << "queued work must no-op once closed";
}

TEST(TaskGateTest, CloseWaitsForRunningTask) {
  TaskGate gate;
  std::atomic<bool> started{false};
  std::atomic<bool> finished{false};
  std::thread worker(gate.Guard([&] {
    started = true;
    std::this_thread::sleep_for(50ms);
    finished = true;
  }));
  while (!started) {
    std::this_thread::yield();
  }
  EXPECT_TRUE(gate.CloseAndWait(2s));
  EXPECT_TRUE(finished) << "CloseAndWait must not return while guarded work runs";
  worker.join();
}

TEST(TaskGateTest, CloseReportsTimeoutWhileTaskStillRuns) {
  TaskGate gate;
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  std::thread worker(gate.Guard([&] {
    started = true;
    while (!release) {
      std::this_thread::sleep_for(1ms);
    }
  }));
  while (!started) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(gate.CloseAndWait(20ms));
  release = true;
  worker.join();
}

TEST(TaskGateTest, GuardForwardsArguments) {
  TaskGate gate;
  int got = 0;
  auto cb = gate.Guard([&](int value) { got = value; });
  cb(7);
  EXPECT_EQ(got, 7);
}

} // namespace
