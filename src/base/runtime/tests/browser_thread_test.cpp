#include "base/runtime/BrowserThread.h"
#include "base/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {

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

TEST(BrowserThreadTest, PostTaskIORoutesToWorkerPool) {
  pbr::AppRuntime::Initialize();
  pbr::BrowserThread::Initialize();

  std::atomic<bool> ran{false};
  pbr::BrowserThread::PostTask(pbr::BrowserThreadId::IO, [&]() { ran.store(true); });

  WaitUntil([&]() { return ran.load(); }, std::chrono::milliseconds(2000));
  EXPECT_TRUE(ran.load());

  pbr::BrowserThread::Shutdown();
  pbr::AppRuntime::Shutdown();
}

TEST(BrowserThreadTest, PostTaskFrontIOUsesCriticalLane) {
  pbr::AppRuntime::Initialize();
  pbr::BrowserThread::Initialize();

  std::mutex mu;
  std::condition_variable cv;
  bool gate_open = false;
  std::vector<std::string> order;

  pbr::BrowserThread::PostTask(pbr::BrowserThreadId::IO, [&]() {
    std::unique_lock lock(mu);
    cv.wait(lock, [&]() { return gate_open; });
    order.push_back("normal");
  });

  pbr::BrowserThread::PostTaskFront(pbr::BrowserThreadId::IO, [&]() { order.push_back("critical"); });

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

  pbr::BrowserThread::Shutdown();
  pbr::AppRuntime::Shutdown();
}

TEST(BrowserThreadTest, PauseIOPausesWorkerPool) {
  pbr::AppRuntime::Initialize();
  pbr::BrowserThread::Initialize();

  std::atomic<bool> ran{false};
  pbr::BrowserThread::PauseIO();
  pbr::BrowserThread::PostTask(pbr::BrowserThreadId::IO, [&]() { ran.store(true); });

  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_FALSE(ran.load());

  pbr::BrowserThread::ResumeIO();

  WaitUntil([&]() { return ran.load(); }, std::chrono::milliseconds(2000));
  EXPECT_TRUE(ran.load());

  pbr::BrowserThread::Shutdown();
  pbr::AppRuntime::Shutdown();
}
