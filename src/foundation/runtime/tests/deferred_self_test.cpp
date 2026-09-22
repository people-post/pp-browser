#include "foundation/runtime/DeferredSelf.h"

#include <gtest/gtest.h>

#include <functional>
#include <vector>

namespace {

using pbr::DeferredSelf;

TEST(DeferredSelfTest, BindNoOpsAfterInvalidate) {
  DeferredSelf deferred;
  int runs = 0;
  auto fn = deferred.Bind([&]() { ++runs; });

  fn();
  EXPECT_EQ(runs, 1);

  deferred.Invalidate();
  fn();
  EXPECT_EQ(runs, 1);
}

TEST(DeferredSelfTest, PostNoOpsAfterInvalidate) {
  DeferredSelf deferred;
  std::vector<std::function<void()>> queue;
  auto post = [&](std::function<void()> task) { queue.push_back(std::move(task)); };

  int runs = 0;
  deferred.Post(post, [&]() { ++runs; });
  ASSERT_EQ(queue.size(), 1u);
  queue[0]();
  EXPECT_EQ(runs, 1);

  deferred.Post(post, [&]() { ++runs; });
  ASSERT_EQ(queue.size(), 2u);
  deferred.Invalidate();
  queue[1]();
  EXPECT_EQ(runs, 1);
}

TEST(DeferredSelfTest, NewBindWorksAfterInvalidate) {
  DeferredSelf deferred;
  int runs = 0;
  auto first = deferred.Bind([&]() { ++runs; });
  deferred.Invalidate();
  auto second = deferred.Bind([&]() { ++runs; });

  first();
  EXPECT_EQ(runs, 0);
  second();
  EXPECT_EQ(runs, 1);
}

TEST(DeferredSelfTest, AliveMatchesSnapshot) {
  DeferredSelf deferred;
  const auto tok = deferred.token();
  const uint64_t snap = deferred.Snapshot();
  EXPECT_TRUE(DeferredSelf::Alive(tok, snap));
  deferred.Invalidate();
  EXPECT_FALSE(DeferredSelf::Alive(tok, snap));
  EXPECT_TRUE(deferred.Alive(deferred.Snapshot()));
}

TEST(DeferredSelfTest, AbortTicketIndependentOfLifetime) {
  // Amp L4 pattern: AbortInflight Invalidates PostIo ticket only; IoTick lifetime survives.
  DeferredSelf abort_ticket;
  DeferredSelf lifetime;
  int abort_runs = 0;
  int life_runs = 0;
  auto abort_fn = abort_ticket.Bind([&]() { ++abort_runs; });
  auto life_fn = lifetime.Bind([&]() { ++life_runs; });

  abort_ticket.Invalidate();
  abort_fn();
  life_fn();
  EXPECT_EQ(abort_runs, 0);
  EXPECT_EQ(life_runs, 1);

  std::vector<std::function<void()>> queue;
  auto post = [&](std::function<void()> task) { queue.push_back(std::move(task)); };
  abort_ticket.Post(post, [&]() { ++abort_runs; });
  lifetime.Post(post, [&]() { ++life_runs; });
  ASSERT_EQ(queue.size(), 2u);
  abort_ticket.Invalidate();
  queue[0]();
  queue[1]();
  EXPECT_EQ(abort_runs, 0);
  EXPECT_EQ(life_runs, 2);
}

} // namespace
