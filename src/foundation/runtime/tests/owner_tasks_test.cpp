#include "foundation/runtime/OwnerOutbox.h"
#include "foundation/runtime/OwnerTasks.h"
#include "foundation/runtime/tests/queue_owner_executor.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <functional>
#include <memory>

namespace pbr {
namespace {

void Drain(std::deque<std::function<void()>>& queue) {
  while (!queue.empty()) {
    auto task = std::move(queue.front());
    queue.pop_front();
    task();
  }
}

// Outbox copies live in I/O completions that may fire after the runner is gone: they must reach the
// runner's tasks only through its shared handle, and do nothing once the tasks closed.
TEST(OwnerTasksTest, OutboxCopiesAreQuietAfterTheTasksAreGone) {
  std::deque<std::function<void()>> queue;
  test::QueueOwnerExecutor executor(queue);
  int handled = 0;
  OwnerOutbox<int> outbox;
  {
    OwnerTasks tasks(executor);
    outbox = MakeOwnerOutbox<int>(tasks, [&handled](int& value) { handled += value; });
    outbox.Emit(1);
    Drain(queue);
    EXPECT_EQ(handled, 1);
    outbox.Emit(10);  // queued, then the runner goes
  }
  Drain(queue);
  EXPECT_EQ(handled, 1) << "an event queued before teardown ran after it";
  outbox.Emit(100);
  OwnerExecutor::TimerId timer = outbox.After(std::chrono::milliseconds(1), 1000);
  EXPECT_EQ(timer, 0u) << "no timer after teardown";
  outbox.Cancel(timer);
  EXPECT_TRUE(queue.empty()) << "a late emit reached the executor";
  EXPECT_EQ(handled, 1);
}

TEST(OwnerTasksTest, CloseRefusesLaterWorkThroughHandles) {
  std::deque<std::function<void()>> queue;
  test::QueueOwnerExecutor executor(queue);
  OwnerTasks tasks(executor);
  auto handle = tasks.Share();
  int ran = 0;
  handle->Post([&ran] { ++ran; });
  tasks.Close();
  handle->Post([&ran] { ++ran; });
  Drain(queue);
  EXPECT_EQ(ran, 0);
}

// A cancelled timer whose task the executor already queued must not run.
TEST(OwnerTasksTest, CancelledTimerAlreadyQueuedDoesNotRun) {
  std::deque<std::function<void()>> queue;
  test::QueueOwnerExecutor executor(queue);
  OwnerTasks tasks(executor);
  int ran = 0;
  OwnerExecutor::TimerId id = tasks.After(std::chrono::milliseconds(1), [&ran] { ++ran; });
  ASSERT_NE(id, 0u);
  tasks.Cancel(id);
  Drain(queue);
  EXPECT_EQ(ran, 0);
}

} // namespace
} // namespace pbr
