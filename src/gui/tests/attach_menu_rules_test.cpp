#include "gui/chat/AttachMenuRules.h"

#include <gtest/gtest.h>

#include <tuple>

namespace pbr {
namespace {

// is_mobile, call_live, in_ai_context, attachment_busy
class ShowCaptureEntryTest : public ::testing::TestWithParam<std::tuple<bool, bool, bool, bool>> {};

TEST_P(ShowCaptureEntryTest, OnlyAPhoneWithNothingElseGoingOnShowsTheCamera) {
  const auto [is_mobile, call_live, in_ai_context, attachment_busy] = GetParam();
  const bool expected = is_mobile && !call_live && !in_ai_context && !attachment_busy;
  EXPECT_EQ(ShowCaptureEntry(is_mobile, call_live, in_ai_context, attachment_busy), expected);
}

INSTANTIATE_TEST_SUITE_P(AllCombinations, ShowCaptureEntryTest,
                         ::testing::Combine(::testing::Bool(), ::testing::Bool(), ::testing::Bool(),
                                            ::testing::Bool()));

TEST(ShowCaptureEntryNamedTest, TheReasonsThatTakeTheCameraAway) {
  EXPECT_TRUE(ShowCaptureEntry(true, false, false, false));
  EXPECT_FALSE(ShowCaptureEntry(false, false, false, false)) << "desktop";
  EXPECT_FALSE(ShowCaptureEntry(true, true, false, false)) << "a call has the camera";
  EXPECT_FALSE(ShowCaptureEntry(true, false, true, false)) << "AI composer";
  EXPECT_FALSE(ShowCaptureEntry(true, false, false, true)) << "uploading or waiting for Retry / Cancel";
}

// A picture being sent shows as a bubble (no card explains a locked composer): text stays typeable and sendable, only
// another attachment waits; a file / voice / video send (its card above the input) keeps the whole composer locked.
TEST(ComposerLocksTest, APictureBubbleLeavesTheTextInputOnAndLocksOnlyTheAttachEntries) {
  const ComposerLocks bubble =
      ComposerLocksOf(false, /*attachment_here=*/true, /*picture_in_transcript=*/true, false, false);
  EXPECT_FALSE(bubble.input);
  EXPECT_TRUE(bubble.attach);
  const ComposerLocks card = ComposerLocksOf(false, true, /*picture_in_transcript=*/false, false, false);
  EXPECT_TRUE(card.input);
  EXPECT_TRUE(card.attach);
  const ComposerLocks idle = ComposerLocksOf(false, false, false, false, false);
  EXPECT_FALSE(idle.input);
  EXPECT_FALSE(idle.attach);
  for (const ComposerLocks other :
       {ComposerLocksOf(true, true, true, false, false), ComposerLocksOf(false, true, true, true, false),
        ComposerLocksOf(false, true, true, false, true)}) {
    EXPECT_TRUE(other.input) << "compose off, an AI picture preparing, or recording still lock the input";
    EXPECT_TRUE(other.attach);
  }
}

} // namespace
} // namespace pbr
