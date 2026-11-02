#include "gui/chat/AttachmentPreparing.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using std::chrono::seconds;

TEST(AttachmentPreparing, BeginMakesItBusy) {
  AttachmentPreparing preparing;
  EXPECT_FALSE(preparing.Busy());
  EXPECT_TRUE(preparing.Begin());
  EXPECT_TRUE(preparing.Busy());
}

TEST(AttachmentPreparing, EndEndsItAndANewOneCanStart) {
  AttachmentPreparing preparing;
  ASSERT_TRUE(preparing.Begin());
  preparing.End(); // path delivered, empty path, camera failure and picker cancel all end here
  EXPECT_FALSE(preparing.Busy());
  EXPECT_TRUE(preparing.Begin());
}

TEST(AttachmentPreparing, ASecondEntryIsRefusedWhileBusy) {
  AttachmentPreparing preparing;
  ASSERT_TRUE(preparing.Begin());
  EXPECT_FALSE(preparing.Begin());
  EXPECT_TRUE(preparing.Busy()) << "the refused entry changes nothing";
}

TEST(AttachmentPreparing, EndingWhenNothingIsOnIsHarmless) {
  AttachmentPreparing preparing;
  preparing.End(); // a picker that was cancelled never began
  EXPECT_FALSE(preparing.Busy());
}

TEST(AttachmentPreparing, ItExpiresWhenNoExitEverReports) {
  AttachmentPreparing preparing;
  const auto start = AttachmentPreparing::Clock::now();
  ASSERT_TRUE(preparing.Begin(start));
  EXPECT_FALSE(preparing.Expire(start + seconds(59)));
  EXPECT_TRUE(preparing.Busy());
  EXPECT_TRUE(preparing.Expire(start + seconds(61)));
  EXPECT_FALSE(preparing.Busy());
  EXPECT_FALSE(preparing.Expire(start + seconds(62))) << "reported once";
}

TEST(AttachmentPreparing, TheChipBelongsToTheChatTheFileIsSentIn) {
  EXPECT_TRUE(AttachmentChipHere(true, "A", "A"));
  EXPECT_FALSE(AttachmentChipHere(true, "A", "B")) << "another chat: no chip, input stays typeable";
  EXPECT_FALSE(AttachmentChipHere(false, "A", "A")) << "nothing in flight";
}

} // namespace
} // namespace pbr
