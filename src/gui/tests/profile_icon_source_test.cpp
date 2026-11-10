// Which sources Me → Profile's photo menu offers, and when it is skipped for the only one (PLAN-avatar-edit 做法 1).
#include "gui/ProfileIconSource.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Action = ProfileIconSourceAction;
using Actions = std::vector<Action>;

TEST(ProfileIconSourceTest, APhoneWithAPhotoOffersCameraLibraryAndRemoveInThatOrder) {
  const auto plan = PlanProfileIconSources(/*has_camera=*/true, /*has_icon=*/true);
  EXPECT_EQ(plan.actions, (Actions{Action::Camera, Action::Library, Action::Remove}));
  EXPECT_FALSE(plan.run_directly);
}

TEST(ProfileIconSourceTest, APhoneWithoutAPhotoHasNothingToRemove) {
  const auto plan = PlanProfileIconSources(true, false);
  EXPECT_EQ(plan.actions, (Actions{Action::Camera, Action::Library}));
  EXPECT_FALSE(plan.run_directly);
}

TEST(ProfileIconSourceTest, TheDesktopWithAPhotoHasNoCamera) {
  const auto plan = PlanProfileIconSources(false, true);
  EXPECT_EQ(plan.actions, (Actions{Action::Library, Action::Remove}));
  EXPECT_FALSE(plan.run_directly);
}

TEST(ProfileIconSourceTest, TheDesktopWithoutAPhotoGoesStraightToTheFileDialog) {
  const auto plan = PlanProfileIconSources(false, false);
  EXPECT_EQ(plan.actions, (Actions{Action::Library}));
  EXPECT_TRUE(plan.run_directly);
}

} // namespace
} // namespace pbr
