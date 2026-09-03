#include "domain/ui/PeoplePickerLogic.h"

#include <gtest/gtest.h>

using pbr::ComputePeoplePickerCta;
using pbr::PeoplePickerCta;
using pbr::PeoplePickerMode;

TEST(PeoplePickerLogicTest, FreeModeThresholds) {
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 0), PeoplePickerCta::Disabled);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 1), PeoplePickerCta::Message);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 2), PeoplePickerCta::CreateGroup);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::Free, 5), PeoplePickerCta::CreateGroup);
}

TEST(PeoplePickerLogicTest, CallInviteModesRequireSelection) {
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupCall, 0), PeoplePickerCta::Disabled);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::GroupCall, 1), PeoplePickerCta::StartCall);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::CallAddGuest, 0), PeoplePickerCta::Disabled);
  EXPECT_EQ(ComputePeoplePickerCta(PeoplePickerMode::CallAddGuest, 2), PeoplePickerCta::StartCall);
}
