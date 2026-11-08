#include "lib/calls/RecentCallsOver.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

using Ids = std::vector<std::string>;

// P017: the phone's call screen learns of each call that is over once, in order, from the published state.
TEST(RecentCallsOverTest, AReaderTakesEachCallOnce) {
  RecentCallsOver over;
  EXPECT_TRUE(over.NewSince(0).empty());
  over.Note("c1");
  over.Note("c2");
  EXPECT_EQ(over.NewSince(0), (Ids{"c1", "c2"}));
  const uint64_t seen = over.seq;
  EXPECT_TRUE(over.NewSince(seen).empty());
  over.Note("c3");
  EXPECT_EQ(over.NewSince(seen), (Ids{"c3"}));
}

TEST(RecentCallsOverTest, AReaderFarBehindGetsTheNewestKept) {
  RecentCallsOver over;
  for (int i = 0; i < 20; ++i) {
    over.Note("c" + std::to_string(i));
  }
  const Ids fresh = over.NewSince(0);
  ASSERT_EQ(fresh.size(), RecentCallsOver::kKept);
  EXPECT_EQ(fresh.front(), "c12");
  EXPECT_EQ(fresh.back(), "c19");
}

TEST(RecentCallsOverTest, NoIdIsNoNews) {
  RecentCallsOver over;
  over.Note("");
  EXPECT_EQ(over.seq, 0u);
  EXPECT_TRUE(over.NewSince(0).empty());
}

} // namespace
} // namespace pbr
