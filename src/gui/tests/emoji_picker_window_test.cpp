#include "gui/EmojiPickerController.h"

#include <gtest/gtest.h>

TEST(EmojiPickerWindowTest, GrowsAheadFromStart) {
  int begin = -1;
  int end = -1;
  pbr::EmojiPickerController::ComputeSectionWindow(/*center=*/0, /*n=*/8, /*span=*/4, /*prev_end=*/0, begin,
                                                   end);
  EXPECT_EQ(begin, 0);
  EXPECT_EQ(end, 4);
}

TEST(EmojiPickerWindowTest, GrowsEndOnScrollDownNeverShrinks) {
  int begin = -1;
  int end = -1;
  pbr::EmojiPickerController::ComputeSectionWindow(/*center=*/2, /*n=*/8, /*span=*/4, /*prev_end=*/4, begin,
                                                   end);
  EXPECT_EQ(begin, 0);
  EXPECT_EQ(end, 6) << "center+span should extend end for scroll-load";

  pbr::EmojiPickerController::ComputeSectionWindow(/*center=*/1, /*n=*/8, /*span=*/4, /*prev_end=*/6, begin,
                                                   end);
  EXPECT_EQ(end, 6) << "end must not shrink when scrolling back up";
  EXPECT_EQ(begin, 0);
}

TEST(EmojiPickerWindowTest, UnloadsBehindWhileKeepingAhead) {
  int begin = -1;
  int end = -1;
  pbr::EmojiPickerController::ComputeSectionWindow(/*center=*/7, /*n=*/8, /*span=*/4, /*prev_end=*/6, begin,
                                                   end);
  EXPECT_EQ(begin, 3);
  EXPECT_EQ(end, 8);
}

TEST(EmojiPickerWindowTest, JumpFarCategoryExpandsEnd) {
  int begin = -1;
  int end = -1;
  pbr::EmojiPickerController::ComputeSectionWindow(/*center=*/7, /*n=*/8, /*span=*/4, /*prev_end=*/0, begin,
                                                   end);
  EXPECT_EQ(begin, 3);
  EXPECT_EQ(end, 8);
}
