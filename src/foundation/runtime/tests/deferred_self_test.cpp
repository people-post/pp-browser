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

} // namespace
