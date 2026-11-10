#include "gui/chat/ReadWhenLooking.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(ReadWhenLookingTest, ReadsOnlyWhenLookingAtAnOpenShownChatWithUnread) {
  EXPECT_TRUE(ShouldMarkOpenChatRead(true, true, true, true));
}

TEST(ReadWhenLookingTest, EveryOtherCombinationLeavesItUnread) {
  for (int bits = 0; bits < 15; ++bits) { // 15 = all four true, the one case above
    const bool attentive = (bits & 1) != 0;
    const bool open = (bits & 2) != 0;
    const bool shows = (bits & 4) != 0;
    const bool unread = (bits & 8) != 0;
    EXPECT_FALSE(ShouldMarkOpenChatRead(attentive, open, shows, unread)) << "bits=" << bits;
  }
}

} // namespace
} // namespace pbr
