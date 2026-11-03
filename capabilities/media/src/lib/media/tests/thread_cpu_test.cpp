#include "lib/media/ThreadCpu.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Prev = std::unordered_map<uint32_t, double>;

TEST(ThreadCpuTest, FirstCallHasNoBaseline) {
  Prev prev;
  EXPECT_TRUE(TopThreadCpuDeltas({{1, "main", 5.0}}, prev, 8).empty());
  EXPECT_EQ(prev.size(), 1u);
}

TEST(ThreadCpuTest, DeltasMergeByNameAndSortLargestFirst) {
  Prev prev;
  TopThreadCpuDeltas({{1, "main", 1.0}, {2, "pp.video", 1.0}, {3, "pp.video", 2.0}, {4, "x1", 0.0}}, prev, 8);
  const auto top = TopThreadCpuDeltas(
      {{1, "main", 1.2}, {2, "pp.video", 1.3}, {3, "pp.video", 2.1}, {4, "x1", 0.05}, {5, "x2", 0.1}}, prev, 8);
  ASSERT_EQ(top.size(), 3u);
  EXPECT_EQ(top[0].name, "pp.video");
  EXPECT_NEAR(top[0].ms, 400.0, 1e-6);
  EXPECT_EQ(top[1].name, "main");
  EXPECT_NEAR(top[1].ms, 200.0, 1e-6);
  EXPECT_EQ(top[2].name, "other"); // unknown names (new thread 5 counts fully) merge
  EXPECT_NEAR(top[2].ms, 150.0, 1e-6);
}

TEST(ThreadCpuTest, EndedThreadsDropOutAndTopNCuts) {
  Prev prev;
  TopThreadCpuDeltas({{1, "main", 1.0}, {2, "pp.capture", 1.0}, {3, "pp.playout", 1.0}}, prev, 8);
  const auto top = TopThreadCpuDeltas({{1, "main", 1.3}, {3, "pp.playout", 1.1}}, prev, 1);
  ASSERT_EQ(top.size(), 1u);
  EXPECT_EQ(top[0].name, "main");
  EXPECT_EQ(prev.count(2), 0u);
}

TEST(ThreadCpuTest, CanonicalNames) {
  Prev prev;
  TopThreadCpuDeltas({{1, "main", 10.0}, {2, "pp.video", 4.0}}, prev, 8);
  const auto top = TopThreadCpuDeltas({{1, "main", 10.45}, {2, "pp.video", 4.3}}, prev, 8);
  ASSERT_EQ(top.size(), 2u);
  EXPECT_EQ(CanonicalThreadName("pp.video"), "pp.video");
  EXPECT_EQ(CanonicalThreadName("anything else"), "other");
}

} // namespace
} // namespace pbr
