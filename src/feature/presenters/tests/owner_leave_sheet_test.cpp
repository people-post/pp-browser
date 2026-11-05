// The owner leaving a group (DEV/PLAN-ui-batch8-group-pages.md 3): who can take over (from the close plan: reachable
// members, never this device), the one confirm naming the person, and the check made again right before handing over.
#include "feature/presenters/ChatPresenter.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

CloseChatPlan Plan(const CloseChatPlan::Kind kind, std::vector<std::string> successors) {
  CloseChatPlan plan;
  plan.kind = kind;
  plan.group_id = "g1";
  plan.successors = std::move(successors);
  return plan;
}

std::string NameOf(const std::string& identity) {
  if (identity == "acct:b") return "Bea";
  if (identity == "acct:a") return "Ann";
  return "";
}

class OwnerLeaveSheetTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
};

} // namespace

TEST_F(OwnerLeaveSheetTest, ListsTheSuccessorsOfTheCloseByName) {
  const OwnerLeaveSheet sheet =
      BuildOwnerLeaveSheet("t1", Plan(CloseChatPlan::Kind::LeaveAsOwner, {"acct:b", "acct:a"}), NameOf);
  EXPECT_EQ(sheet.thread_id, "t1");
  EXPECT_EQ(sheet.group_id, "g1");
  ASSERT_EQ(sheet.candidates.size(), 2u);
  EXPECT_EQ(sheet.candidates[0], (OwnerLeaveCandidate{"acct:a", "Ann"}));
  EXPECT_EQ(sheet.candidates[1], (OwnerLeaveCandidate{"acct:b", "Bea"}));
}

TEST_F(OwnerLeaveSheetTest, SomeoneWithoutANameShowsTheirShortId) {
  const OwnerLeaveSheet sheet =
      BuildOwnerLeaveSheet("t1", Plan(CloseChatPlan::Kind::LeaveAsOwner, {"acct:0123456789abcdef"}), NameOf);
  ASSERT_EQ(sheet.candidates.size(), 1u);
  EXPECT_FALSE(sheet.candidates[0].name.empty());
}

TEST_F(OwnerLeaveSheetTest, NobodyToTakeOverLeavesOnlyDissolve) {
  const OwnerLeaveSheet sheet = BuildOwnerLeaveSheet("t1", Plan(CloseChatPlan::Kind::Dissolve, {}), NameOf);
  EXPECT_EQ(sheet.thread_id, "t1");
  EXPECT_TRUE(sheet.candidates.empty());
}

TEST_F(OwnerLeaveSheetTest, NotForAnyoneButTheOwnerLeaving) {
  for (const CloseChatPlan::Kind kind : {CloseChatPlan::Kind::Leave, CloseChatPlan::Kind::Dismiss,
                                         CloseChatPlan::Kind::Delete, CloseChatPlan::Kind::DeleteAi}) {
    EXPECT_TRUE(BuildOwnerLeaveSheet("t1", Plan(kind, {"acct:a"}), NameOf).thread_id.empty());
  }
}

TEST_F(OwnerLeaveSheetTest, TheConfirmAndTheGoneLineNameThePerson) {
  EXPECT_EQ(HandOverConfirmText("Ann"), Tr("group_leave.confirm", {{"name", "Ann"}}));
  EXPECT_NE(HandOverConfirmText("Ann").find("Ann"), std::string::npos);
  EXPECT_NE(HandOverGoneText("Ann").find("Ann"), std::string::npos);
}

TEST_F(OwnerLeaveSheetTest, HandingOverIsCheckedAgainstAPlanReadNow) {
  EXPECT_TRUE(HandOverStillPossible(Plan(CloseChatPlan::Kind::LeaveAsOwner, {"acct:a", "acct:b"}), "acct:a"));
  // Ann left or went out of reach meanwhile.
  EXPECT_FALSE(HandOverStillPossible(Plan(CloseChatPlan::Kind::LeaveAsOwner, {"acct:b"}), "acct:a"));
  // Nobody left to take over, or no longer the owner.
  EXPECT_FALSE(HandOverStillPossible(Plan(CloseChatPlan::Kind::Dissolve, {}), "acct:a"));
  EXPECT_FALSE(HandOverStillPossible(Plan(CloseChatPlan::Kind::Leave, {"acct:a"}), "acct:a"));
}
