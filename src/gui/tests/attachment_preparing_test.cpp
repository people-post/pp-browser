#include "gui/chat/AttachmentPreparing.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AttachmentPreparing, BeginMakesItBusy) {
  AttachmentPreparing preparing;
  EXPECT_FALSE(preparing.Busy());
  EXPECT_TRUE(preparing.Begin());
  EXPECT_TRUE(preparing.Busy());
}

TEST(AttachmentPreparing, EveryExitEndsIt) {
  for (const auto outcome :
       {AttachmentPrepareOutcome::Delivered, AttachmentPrepareOutcome::NoFile, AttachmentPrepareOutcome::Cancelled,
        AttachmentPrepareOutcome::Unavailable, AttachmentPrepareOutcome::Unprocessable}) {
    AttachmentPreparing preparing;
    ASSERT_TRUE(preparing.Begin());
    preparing.End(outcome);
    EXPECT_FALSE(preparing.Busy()) << static_cast<int>(outcome);
    EXPECT_TRUE(preparing.Begin()) << "a new attachment can start again";
  }
}

TEST(AttachmentPreparing, ASecondEntryIsRefusedWhileBusy) {
  AttachmentPreparing preparing;
  ASSERT_TRUE(preparing.Begin());
  EXPECT_FALSE(preparing.Begin());
  EXPECT_TRUE(preparing.Busy()) << "the refused entry changes nothing";
}

TEST(AttachmentPreparing, EndingWhenNothingIsOnIsHarmless) {
  AttachmentPreparing preparing;
  preparing.End(AttachmentPrepareOutcome::Cancelled); // a picker that was cancelled never began
  EXPECT_FALSE(preparing.Busy());
}

} // namespace
} // namespace pbr
