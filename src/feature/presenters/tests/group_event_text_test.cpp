#include "feature/presenters/GroupEventText.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupMembershipCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The words of a group's membership lines are one function for the transcript and the chat list's preview.

const auto kNames = [](const std::string& account) { return account == "acct-ann" ? std::string("Ann") : account; };

ThreadMessage Event(const GroupMembershipControlType type, const Roe<std::string>& detail, const std::string& text,
                    const std::string& sender) {
  EXPECT_TRUE(detail);
  auto message = GroupMembershipCodec::BuildSystemMessage("t", type, text, *detail, sender);
  EXPECT_TRUE(message);
  return *message;
}

class GroupEventTextTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(GroupEventTextTest, TheChatListPreviewAndTheTranscriptUseTheSameChineseWords) {
  const auto text =
      GroupEventText(Event(GroupMembershipControlType::MemberJoined,
                           GroupMembershipCodec::EncodeMemberJoined("g", "acct-ann", MemberRole::Member, 2),
                           "Member joined the group", "acct-ann"),
                     kNames, "acct-me");
  ASSERT_TRUE(text);
  EXPECT_EQ(*text, "Ann \xE5\x8A\xA0\xE5\x85\xA5\xE4\xBA\x86\xE7\xBE\xA4\xE8\x81\x8A");
}

TEST_F(GroupEventTextTest, TheStoredEnglishTextsAreRecognisedAsMembershipPreviews) {
  for (const char* stored :
       {"Member joined the group", "You joined the group", "Owner created the group", "Member removed", "Member left",
        "Group ownership transferred", "Ownership transferred; previous owner left", "Group renamed to Sunday hike",
        "The group is full: you were not added", "Group forked", "Forked from group", "Invitation declined",
        "Group dissolved"}) {
    EXPECT_TRUE(IsStoredGroupEventText(stored)) << stored;
  }
  EXPECT_FALSE(IsStoredGroupEventText("see you at 7"));
  EXPECT_FALSE(IsStoredGroupEventText("Member joined the group chat later"));
}

TEST_F(GroupEventTextTest, ADissolvedGroupPreviewNamesTheOwnerOrYou) {
  const auto dissolved = [](const std::string& sender) {
    return Event(GroupMembershipControlType::GroupDissolved,
                 GroupMembershipCodec::EncodeGroupDissolved("group:hike", "acct-ann"), "Group dissolved", sender);
  };
  EXPECT_TRUE(IsStoredGroupEventText("Group dissolved"));
  const auto by_ann = GroupEventText(dissolved("acct-ann"), kNames, "acct-me");
  ASSERT_TRUE(by_ann);
  EXPECT_EQ(*by_ann, "Ann \xE8\xA7\xA3\xE6\x95\xA3\xE4\xBA\x86\xE8\xBF\x99\xE4\xB8\xAA\xE7\xBE\xA4");
  const auto by_me = GroupEventText(dissolved("acct-me"), kNames, "acct-me");
  ASSERT_TRUE(by_me);
  EXPECT_EQ(*by_me, "\xE4\xBD\xA0\xE8\xA7\xA3\xE6\x95\xA3\xE4\xBA\x86\xE8\xBF\x99\xE4\xB8\xAA\xE7\xBE\xA4");
}

TEST_F(GroupEventTextTest, ForkedLinesTellTheTwoSidesApart) {
  const auto source = GroupEventText(
      Event(GroupMembershipControlType::GroupForked, Roe<std::string>(std::string("{}")), "Group forked", "acct-ann"),
      kNames, "acct-me");
  const auto fresh = GroupEventText(Event(GroupMembershipControlType::GroupForked, Roe<std::string>(std::string("{}")),
                                          "Forked from group", "acct-ann"),
                                    kNames, "acct-me");
  ASSERT_TRUE(source && fresh);
  EXPECT_EQ(
      *source,
      "Ann "
      "\xE4\xBB\x8E\xE6\x9C\xAC\xE7\xBE\xA4\xE5\x88\x9B\xE5\xBB\xBA\xE4\xBA\x86\xE6\x96\xB0\xE7\xBE\xA4\xE8\x81\x8A");
  EXPECT_NE(*source, *fresh);
}

// G022: a group made during a call starts with one line saying so, on the maker's side and on the joiner's.
TEST_F(GroupEventTextTest, AGroupMadeInACallSaysSoOnItsFirstLine) {
  GroupMembershipCodec::MemberJoinedPayload made;
  made.group_id = "g";
  made.member_identity = "acct-me";
  made.role = MemberRole::Owner;
  made.roster_epoch = 1;
  made.context = kGroupInviteContextCall;
  const auto maker =
      GroupEventText(Event(GroupMembershipControlType::MemberJoined, GroupMembershipCodec::EncodeMemberJoined(made),
                           kGroupMadeInCallLine, "acct-me"),
                     kNames, "acct-me");
  ASSERT_TRUE(maker);
  EXPECT_EQ(*maker, Tr("chat.group_event.made_in_call"));
  EXPECT_NE(*maker, kGroupMadeInCallLine) << "in the reader's language";

  GroupMembershipCodec::MemberJoinedPayload joined = made;
  joined.member_identity = "acct-bo";
  joined.role = MemberRole::Member;
  const auto joiner =
      GroupEventText(Event(GroupMembershipControlType::MemberJoined, GroupMembershipCodec::EncodeMemberJoined(joined),
                           kGroupMadeInCallLine, "acct-bo"),
                     kNames, "acct-bo");
  ASSERT_TRUE(joiner);
  EXPECT_EQ(*joiner, Tr("chat.group_event.made_in_call"));
  EXPECT_TRUE(IsStoredGroupEventText(kGroupMadeInCallLine)) << "the chat list words the preview too";
}

// G020: a removal says who removed whom — "Bob removed Ann", "Bob removed you", "You removed Ann".
TEST_F(GroupEventTextTest, ARemovalNamesWhoRemovedWhom) {
  LocalizationService::Instance().SetPreferredLanguage("en");
  const auto names = [](const std::string& account) {
    return account == "acct-ann" ? std::string("Ann") : account == "acct-bob" ? std::string("Bob") : account;
  };
  const auto removal = [](const std::string& member, const std::string& by) {
    return Event(GroupMembershipControlType::MemberRemoved,
                 GroupMembershipCodec::EncodeMemberRemoved("g", member, 4, by), "Member removed", by);
  };
  EXPECT_EQ(GroupEventText(removal("acct-ann", "acct-bob"), names, "acct-me").value_or(""),
            "Bob removed Ann from the group");
  EXPECT_EQ(GroupEventText(removal("acct-me", "acct-bob"), names, "acct-me").value_or(""),
            "Bob removed you from the group");
  EXPECT_EQ(GroupEventText(removal("acct-ann", "acct-me"), names, "acct-me").value_or(""),
            "You removed Ann from the group");
  EXPECT_EQ(GroupEventText(removal("acct-ann", ""), names, "acct-me").value_or(""), "Ann was removed from the group")
      << "an event that does not say who";
}

// G021: a join by link says whose link let them in — "Erin joined by Bob's link", for Erin and for Bob too.
TEST_F(GroupEventTextTest, AJoinByLinkNamesWhoseLinkItWas) {
  LocalizationService::Instance().SetPreferredLanguage("en");
  const auto names = [](const std::string& account) {
    return account == "acct-erin" ? std::string("Erin") : account == "acct-bob" ? std::string("Bob") : account;
  };
  const auto joined = [](const std::string& member, const std::string& by, const bool by_link) {
    GroupMembershipCodec::MemberJoinedPayload payload;
    payload.group_id = "g";
    payload.member_identity = member;
    payload.roster_epoch = 4;
    payload.added_by = by;
    payload.by_link = by_link;
    return Event(GroupMembershipControlType::MemberJoined, GroupMembershipCodec::EncodeMemberJoined(payload),
                 "Member joined the group", by);
  };
  EXPECT_EQ(GroupEventText(joined("acct-erin", "acct-bob", true), names, "acct-me").value_or(""),
            "Erin joined by Bob's link");
  EXPECT_EQ(GroupEventText(joined("acct-me", "acct-bob", true), names, "acct-me").value_or(""),
            "You joined by Bob's link");
  EXPECT_EQ(GroupEventText(joined("acct-erin", "acct-me", true), names, "acct-me").value_or(""),
            "Erin joined by your link");
  EXPECT_EQ(GroupEventText(joined("acct-erin", "acct-bob", false), names, "acct-me").value_or(""), "Bob added Erin")
      << "an invite's add reads as before";
}

} // namespace
} // namespace pbr
