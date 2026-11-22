// After Reset my profile the app restarts only after a success and only where a restart can be made; otherwise it clears
// in place (the fallback). The restart itself (os::RestartProcess) cannot be unit-tested: see os_restart_test.cpp.
#include "gui/ProfileResetFollowUp.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(ProfileResetFollowUpTest, ARestartIsAskedForOnlyAfterASuccessAndOnlyWhereItCanBeMade) {
  EXPECT_EQ(ChooseResetFollowUp(false, true, true), ResetFollowUp::None) << "a failed reset restarts nothing";
  EXPECT_EQ(ChooseResetFollowUp(false, false, false), ResetFollowUp::None);
  EXPECT_EQ(ChooseResetFollowUp(true, true, true), ResetFollowUp::RestartProcess);
  EXPECT_EQ(ChooseResetFollowUp(true, false, true), ResetFollowUp::ClearInPlace) << "phones clear in place";
  EXPECT_EQ(ChooseResetFollowUp(true, true, false), ResetFollowUp::ClearInPlace) << "no plan: the fallback";
  EXPECT_EQ(ChooseResetFollowUp(true, false, false), ResetFollowUp::ClearInPlace);
}

} // namespace
} // namespace pbr
