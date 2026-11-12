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
  const ComposerLocks bubble = ComposerLocksOf(false, /*attachment_here=*/true, /*picture_in_transcript=*/true,
                                               /*attachment_busy=*/true, false, false);
  EXPECT_FALSE(bubble.input);
  EXPECT_TRUE(bubble.attach);
  const ComposerLocks card =
      ComposerLocksOf(false, true, /*picture_in_transcript=*/false, /*attachment_busy=*/true, false, false);
  EXPECT_TRUE(card.input);
  EXPECT_TRUE(card.attach);
  const ComposerLocks idle = ComposerLocksOf(false, false, false, /*attachment_busy=*/false, false, false);
  EXPECT_FALSE(idle.input);
  EXPECT_FALSE(idle.attach);
  for (const ComposerLocks other : {ComposerLocksOf(true, true, true, /*attachment_busy=*/true, false, false),
                                    ComposerLocksOf(false, true, true, /*attachment_busy=*/true, true, false),
                                    ComposerLocksOf(false, true, true, /*attachment_busy=*/true, false, true)}) {
    EXPECT_TRUE(other.input) << "compose off, an AI picture preparing, or recording still lock the input";
    EXPECT_TRUE(other.attach);
  }
}

// DEV/PLAN-attachment-bubbles-multi-pick.md goal 4: the plus, the camera and the microphone are greyed only while an
// attachment is preparing or uploading (any chat); beside a failed bubble alone they work (a new attachment then makes
// the bubble give way); a card (a send left from before a restart) still locks the whole composer.
TEST(ComposerLocksTest, TheAttachEntriesAreGreyedOnlyWhileAnAttachmentPreparesOrUploads) {
  struct Row {
    const char* name;
    bool here, bubble, busy;
    bool input, attach;
  };
  const Row rows[] = {
      {"preparing or uploading here, bubble", true, true, true, false, true},
      {"uploading in another chat", false, false, true, false, true},
      {"only a failed bubble here", true, true, false, false, false},
      {"a failed card (restart)", true, false, false, true, true},
      {"nothing", false, false, false, false, false},
  };
  for (const Row& row : rows) {
    SCOPED_TRACE(row.name);
    const ComposerLocks locks = ComposerLocksOf(false, row.here, row.bubble, row.busy, false, false);
    EXPECT_EQ(locks.input, row.input);
    EXPECT_EQ(locks.attach, row.attach);
  }
}

// Editing a sent message (DEV/PLAN-message-edit.md): the words stay typeable; the plus and the microphone grey out.
TEST(ComposerLocksTest, EditingAMessageGreysTheAttachEntriesAndKeepsTheInput) {
  const ComposerLocks locks = ComposerLocksOf(false, false, false, false, false, false, /*editing_message=*/true);
  EXPECT_FALSE(locks.input);
  EXPECT_TRUE(locks.attach);
  EXPECT_FALSE(ComposerLocksOf(false, false, false, false, false, false, false).attach);
}

} // namespace
} // namespace pbr
