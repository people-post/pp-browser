// A peer whose app is newer than ours (INVENTORY U1): marked, and told about once a day, not once a message.
#include "lib/messaging/PeerUpdateMarks.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'800'000'000'000;

TEST(PeerUpdateMarksTest, ThePeerIsToldOnceADay) {
  PeerUpdateMarks marks;
  EXPECT_FALSE(marks.Marked("bob"));
  EXPECT_TRUE(marks.Note("bob", kNow)) << "the first time";
  EXPECT_TRUE(marks.Marked("bob"));
  EXPECT_FALSE(marks.Note("bob", kNow + 1)) << "the next message: no second notice";
  EXPECT_FALSE(marks.Note("bob", kNow + kNeedsUpdateNoticeGapMs - 1));
  EXPECT_TRUE(marks.Note("bob", kNow + kNeedsUpdateNoticeGapMs)) << "a day on, told again";
  EXPECT_TRUE(marks.Note("carol", kNow + 2)) << "each peer apart";
}

TEST(PeerUpdateMarksTest, NoSenderIsNoMark) {
  PeerUpdateMarks marks;
  EXPECT_FALSE(marks.Note("", kNow));
  EXPECT_EQ(marks.Size(), 0u);
}

TEST(PeerUpdateMarksTest, TheMarksAreBounded) {
  PeerUpdateMarks marks;
  for (size_t i = 0; i < kPeerUpdateMarksMax + 3; ++i) {
    marks.Note("acct-" + std::to_string(i), kNow + static_cast<int64_t>(i));
  }
  EXPECT_EQ(marks.Size(), kPeerUpdateMarksMax);
  EXPECT_FALSE(marks.Marked("acct-0")) << "the longest unheard went first";
  EXPECT_TRUE(marks.Marked("acct-" + std::to_string(kPeerUpdateMarksMax + 2)));
}

} // namespace
} // namespace pbr
