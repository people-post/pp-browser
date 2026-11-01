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
                         ::testing::Combine(::testing::Bool(), ::testing::Bool(), ::testing::Bool(), ::testing::Bool()));

TEST(ShowCaptureEntryNamedTest, TheReasonsThatTakeTheCameraAway) {
  EXPECT_TRUE(ShowCaptureEntry(true, false, false, false));
  EXPECT_FALSE(ShowCaptureEntry(false, false, false, false)) << "desktop";
  EXPECT_FALSE(ShowCaptureEntry(true, true, false, false)) << "a call has the camera";
  EXPECT_FALSE(ShowCaptureEntry(true, false, true, false)) << "AI composer";
  EXPECT_FALSE(ShowCaptureEntry(true, false, false, true)) << "uploading or waiting for Retry / Cancel";
}

} // namespace
} // namespace pbr
