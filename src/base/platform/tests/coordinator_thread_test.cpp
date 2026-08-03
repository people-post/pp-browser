#include "base/platform/CoordinatorThread.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using pbr::CoordinatorPriority;
using pbr::CoordinatorThread;

void WaitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "Timed out waiting for condition";
}

} // namespace

TEST(CoordinatorThreadTest, CriticalRunsBeforeNormal) {
  CoordinatorThread coordinator;
  coordinator.Start();

  std::mutex mu;
  std::condition_variable cv;
  bool gate_open = false;
  std::vector<std::string> order;

  coordinator.Post(CoordinatorPriority::Normal, [&]() {
    std::unique_lock lock(mu);
    cv.wait(lock, [&]() { return gate_open; });
    order.push_back("normal");
  });

  coordinator.Post(CoordinatorPriority::Critical, [&]() { order.push_back("critical"); });

  WaitUntil([&]() { return order.size() == 1; }, std::chrono::milliseconds(2000));
  ASSERT_EQ(order.size(), 1u);
  EXPECT_EQ(order.front(), "critical");

  {
    std::lock_guard lock(mu);
    gate_open = true;
  }
  cv.notify_all();

  WaitUntil([&]() { return order.size() == 2; }, std::chrono::milliseconds(2000));
  EXPECT_EQ(order[0], "critical");
  EXPECT_EQ(order[1], "normal");

  coordinator.Shutdown();
}

TEST(CoordinatorThreadTest, RepeatingTimerFires) {
  CoordinatorThread coordinator;
  coordinator.Start();

  std::atomic<int> count{0};
  coordinator.ScheduleRepeating(std::chrono::milliseconds(30), [&]() { count.fetch_add(1); });

  WaitUntil([&]() { return count.load() >= 2; }, std::chrono::milliseconds(2000));
  EXPECT_GE(count.load(), 2);

  coordinator.Shutdown();
}

TEST(CoordinatorThreadTest, OneShotTimerFiresOnce) {
  CoordinatorThread coordinator;
  coordinator.Start();

  std::atomic<int> count{0};
  coordinator.ScheduleOneShot(std::chrono::milliseconds(30), [&]() { count.fetch_add(1); });

  WaitUntil([&]() { return count.load() >= 1; }, std::chrono::milliseconds(2000));
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_EQ(count.load(), 1);

  coordinator.Shutdown();
}

TEST(CoordinatorThreadTest, CancelTimerPreventsFire) {
  CoordinatorThread coordinator;
  coordinator.Start();

  std::atomic<int> count{0};
  const uint64_t id =
      coordinator.ScheduleOneShot(std::chrono::milliseconds(50), [&]() { count.fetch_add(1); });
  coordinator.CancelTimer(id);

  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_EQ(count.load(), 0);

  coordinator.Shutdown();
}

TEST(CoordinatorThreadTest, ShutdownJoinsCleanly) {
  CoordinatorThread coordinator;
  coordinator.Start();

  std::atomic<bool> ran{false};
  coordinator.Post(CoordinatorPriority::Normal, [&]() { ran.store(true); });

  WaitUntil([&]() { return ran.load(); }, std::chrono::milliseconds(2000));
  coordinator.Shutdown();
  SUCCEED();
}
