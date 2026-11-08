// G021 (o5): who sees "Invite link…" and the group's live links, the links' rows, the preview sheet's words and how a
// join by link ends, in plain words.
#include "feature/presenters/ChatPresenter.h"
#include "feature/presenters/GroupLinkText.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

constexpr int64_t kNow = 1'800'000'000'000;
constexpr int64_t kDay = 86'400'000;

class GroupLinkTextTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");  // not the machine's language
  }
};

GroupInfoFacts Facts(const MemberRole role, const GroupPreset preset) {
  GroupInfoFacts facts;
  facts.thread_id = "t1";
  facts.group_id = "g1";
  facts.title = "Weekend hike";
  facts.is_owner = role == MemberRole::Owner;
  facts.viewer_role = role;
  facts.preset = preset;
  facts.self_identity = "acct:me";
  GroupMemberRow ann;
  ann.identity = "acct:a";
  ann.title = "Ann";
  facts.members = {ann};
  facts.now_ms = kNow;
  facts.links = {{.link_id = "l1", .maker = "acct:a", .expires_at = kNow + 7 * kDay, .can_revoke = true},
                 {.link_id = "l2", .maker = "acct:me", .expires_at = kNow + kDay / 2, .max_uses = 5, .uses = 2}};
  return facts;
}

} // namespace

TEST_F(GroupLinkTextTest, WhoSeesInviteLinkFollowsThePreset) {
  for (const MemberRole role : {MemberRole::Owner, MemberRole::Admin, MemberRole::Member}) {
    EXPECT_TRUE(BuildGroupInfoScreen(Facts(role, GroupPreset::Open)).show_invite_link) << MemberRoleToString(role);
  }
  EXPECT_TRUE(BuildGroupInfoScreen(Facts(MemberRole::Owner, GroupPreset::Managed)).show_invite_link);
  EXPECT_TRUE(BuildGroupInfoScreen(Facts(MemberRole::Admin, GroupPreset::Managed)).show_invite_link);
  EXPECT_FALSE(BuildGroupInfoScreen(Facts(MemberRole::Member, GroupPreset::Managed)).show_invite_link)
      << "a managed group's member asks an admin";
}

TEST_F(GroupLinkTextTest, TheOwnerAndAdminsSeeTheGroupsLiveLinks) {
  const GroupInfoScreen owner = BuildGroupInfoScreen(Facts(MemberRole::Owner, GroupPreset::Open));
  ASSERT_TRUE(owner.show_links);
  ASSERT_EQ(owner.links.size(), 2u);
  EXPECT_EQ(owner.links[0].label, Tr("group_link.someones", {{"name", "Ann"}}));
  EXPECT_EQ(owner.links[0].detail, Tr("group_link.expires_days", {{"count", "7"}}));
  EXPECT_TRUE(owner.links[0].can_revoke);
  EXPECT_EQ(owner.links[1].label, Tr("group_link.yours"));
  EXPECT_EQ(owner.links[1].detail, Tr("group_link.expires_hours", {{"count", "12"}}) + " · " +
                                       Tr("group_link.used_of", {{"count", "2"}, {"max", "5"}}));
  EXPECT_TRUE(BuildGroupInfoScreen(Facts(MemberRole::Admin, GroupPreset::Managed)).show_links);
  EXPECT_FALSE(BuildGroupInfoScreen(Facts(MemberRole::Member, GroupPreset::Open)).show_links);

  GroupInfoFacts none = Facts(MemberRole::Owner, GroupPreset::Open);
  none.links.clear();
  EXPECT_FALSE(BuildGroupInfoScreen(none).show_links) << "no list without links";
  none.dissolved = true;
  EXPECT_FALSE(BuildGroupInfoScreen(none).show_invite_link) << "a dissolved group has no links";
}

TEST_F(GroupLinkTextTest, ExpiryReadsInTheLargestUnitRoundedUp) {
  EXPECT_EQ(GroupLinkExpiryText(kNow + 7 * kDay, kNow), Tr("group_link.expires_days", {{"count", "7"}}));
  EXPECT_EQ(GroupLinkExpiryText(kNow + 6 * kDay + 1, kNow), Tr("group_link.expires_days", {{"count", "7"}}));
  EXPECT_EQ(GroupLinkExpiryText(kNow + kDay + 1, kNow), Tr("group_link.expires_days", {{"count", "2"}}));
  EXPECT_EQ(GroupLinkExpiryText(kNow + kDay, kNow), Tr("group_link.expires_hours", {{"count", "24"}}));
  EXPECT_EQ(GroupLinkExpiryText(kNow + 3'600'000, kNow), Tr("group_link.expires_minutes", {{"count", "60"}}));
  EXPECT_EQ(GroupLinkExpiryText(kNow + 30'000, kNow), Tr("group_link.expires_minutes_one"));
  EXPECT_EQ(GroupLinkExpiryText(kNow, kNow), Tr("group_link.expired"));
}

TEST_F(GroupLinkTextTest, ThePreviewSaysTheNameTheCountAndWhatJoiningShares) {
  const std::string text = GroupJoinPreviewText("Hikers", 4);
  EXPECT_EQ(text, "Join \"Hikers\"? 4 members. Joining shares your profile with 4 people.");
}

TEST_F(GroupLinkTextTest, AJoinEndsInASheetOrInPlainWords) {
  GroupJoinState state{.request_id = "r1", .maker = "acct:a"};
  EXPECT_EQ(NextGroupJoinStep(state, "Ann").kind, GroupJoinStep::Kind::Wait) << "still asking";

  state.phase = GroupJoinState::Phase::Preview;
  state.preview = GroupJoinPreview{.request_id = "r1", .group_title = "Hikers", .member_count = 4};
  GroupJoinStep step = NextGroupJoinStep(state, "Ann");
  EXPECT_EQ(step.kind, GroupJoinStep::Kind::Preview);
  EXPECT_EQ(step.title, Tr("group_join.title"));
  EXPECT_EQ(step.text, GroupJoinPreviewText("Hikers", 4));

  state.phase = GroupJoinState::Phase::NoAnswer;
  step = NextGroupJoinStep(state, "Ann");
  EXPECT_EQ(step.kind, GroupJoinStep::Kind::Tell);
  EXPECT_EQ(step.text, Tr("group_join.no_answer", {{"name", "Ann"}}));
  EXPECT_NE(step.text.find("mistyped"), std::string::npos) << "no answer also covers a link the maker does not know";

  state.phase = GroupJoinState::Phase::Refused;
  for (const auto& [reason, key] : std::vector<std::pair<GroupJoinRefusal, std::string>>{
           {GroupJoinRefusal::Expired, "group_join.refused.expired"},
           {GroupJoinRefusal::Revoked, "group_join.refused.revoked"},
           {GroupJoinRefusal::UsedUp, "group_join.refused.used_up"},
           {GroupJoinRefusal::Removed, "group_join.refused.removed"},
           {GroupJoinRefusal::GroupFull, "group_join.refused.group_full"},
           {GroupJoinRefusal::Rate, "group_join.refused.rate"}}) {
    state.reason = reason;
    step = NextGroupJoinStep(state, "Ann");
    EXPECT_EQ(step.kind, GroupJoinStep::Kind::Tell);
    EXPECT_EQ(step.text, Tr(key));
    EXPECT_NE(step.text, key) << "worded: " << key;
  }

  state.phase = GroupJoinState::Phase::Joined;
  EXPECT_EQ(NextGroupJoinStep(state, "Ann").kind, GroupJoinStep::Kind::Wait);
}

TEST_F(GroupLinkTextTest, AnAskThatCannotStartSaysWhy) {
  EXPECT_EQ(GroupJoinAskFailureText(Error("group_join.err.own_link")), Tr("group_join.err.own_link"));
  EXPECT_EQ(GroupJoinAskFailureText(Error("Invite link does not decode")), Tr("group_join.err.bad_link"));
}
