#include "gui/chat/TranscriptRml.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupMembershipCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// A group thread's membership lines are worded at display time, in the reader's language, with the person's public
// name: the stored text is English and names nobody (it stays for old rows and previews).

constexpr const char* kGroup = "group:hike";
constexpr const char* kMe = "acct-me";
constexpr const char* kAnn = "acct-ann";
constexpr const char* kHostile = "acct-hostile";
const std::string kHostileName = "<b>\"x\"</b> {title} &amp;";

TranscriptFacts Facts() {
  TranscriptFacts facts;
  facts.local_account = kMe;
  facts.member_name = [](const std::string& account) {
    return account == kHostile ? kHostileName : account == kAnn ? std::string("Ann") : account;
  };
  return facts;
}

std::string Line(const GroupMembershipControlType type, const Roe<std::string>& detail, const std::string& sender,
                 const TranscriptFacts& facts = Facts()) {
  EXPECT_TRUE(detail);
  auto message = GroupMembershipCodec::BuildSystemMessage("t", type, "Stored English text", *detail, sender);
  EXPECT_TRUE(message);
  TranscriptRow row;
  row.message = *message;
  const auto rows = RenderTranscript({row}, facts);
  return rows.empty() ? std::string() : std::string(rows[0].content_rml.c_str());
}

class GroupEventLineTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage(GetParam());
    zh_ = std::string(GetParam()) == "zh-Hans";
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
  bool zh_ = false;
};

INSTANTIATE_TEST_SUITE_P(Locales, GroupEventLineTest, ::testing::Values("en", "zh-Hans"));

TEST_P(GroupEventLineTest, AMemberJoinedNamesThem) {
  const std::string rml =
      Line(GroupMembershipControlType::MemberJoined,
           GroupMembershipCodec::EncodeMemberJoined(kGroup, kAnn, MemberRole::Member, 2), kAnn);
  EXPECT_NE(rml.find(zh_ ? "Ann \xE5\x8A\xA0\xE5\x85\xA5\xE4\xBA\x86\xE7\xBE\xA4\xE8\x81\x8A" : "Ann joined the group"),
            std::string::npos)
      << rml;
  EXPECT_EQ(rml.find("Stored English text"), std::string::npos);
}

TEST_P(GroupEventLineTest, YouJoinedAndYouCreatedReadAsYou) {
  const std::string joined = Line(GroupMembershipControlType::MemberJoined,
                                  GroupMembershipCodec::EncodeMemberJoined(kGroup, kMe, MemberRole::Member, 2), kMe);
  EXPECT_NE(joined.find(zh_ ? "\xE4\xBD\xA0\xE5\x8A\xA0\xE5\x85\xA5\xE4\xBA\x86\xE7\xBE\xA4\xE8\x81\x8A"
                            : "You joined the group"),
            std::string::npos)
      << joined;
  const std::string created = Line(GroupMembershipControlType::MemberJoined,
                                   GroupMembershipCodec::EncodeMemberJoined(kGroup, kMe, MemberRole::Owner, 1), kMe);
  EXPECT_NE(created.find(zh_ ? "\xE4\xBD\xA0\xE5\x88\x9B\xE5\xBB\xBA\xE4\xBA\x86\xE7\xBE\xA4\xE8\x81\x8A"
                             : "You created the group"),
            std::string::npos)
      << created;
}

TEST_P(GroupEventLineTest, ARemovalAndALeaveNameThePerson) {
  const std::string removed = Line(GroupMembershipControlType::MemberRemoved,
                                   GroupMembershipCodec::EncodeMemberRemoved(kGroup, kAnn, 3), kMe);
  EXPECT_NE(removed.find(zh_ ? "Ann \xE8\xA2\xAB\xE7\xA7\xBB\xE5\x87\xBA\xE4\xBA\x86\xE7\xBE\xA4\xE8\x81\x8A"
                             : "Ann was removed from the group"),
            std::string::npos)
      << removed;
  const std::string left =
      Line(GroupMembershipControlType::MemberLeft, GroupMembershipCodec::EncodeMemberLeft(kGroup, kAnn, 3), kAnn);
  EXPECT_NE(left.find(zh_ ? "Ann \xE9\x80\x80\xE5\x87\xBA\xE4\xBA\x86\xE7\xBE\xA4\xE8\x81\x8A" : "Ann left the group"),
            std::string::npos)
      << left;
  const std::string you_removed = Line(GroupMembershipControlType::MemberRemoved,
                                       GroupMembershipCodec::EncodeMemberRemoved(kGroup, kMe, 3), kAnn);
  EXPECT_NE(you_removed.find(zh_ ? "\xE4\xBD\xA0\xE8\xA2\xAB\xE7\xA7\xBB\xE5\x87\xBA\xE4\xBA\x86\xE7\xBE\xA4\xE8\x81\x8A"
                                 : "You were removed from the group"),
            std::string::npos)
      << you_removed;
}

TEST_P(GroupEventLineTest, ARenameNamesWhoDidItAndTheNewTitle) {
  const std::string rml = Line(GroupMembershipControlType::GroupRenamed,
                               GroupMembershipCodec::EncodeGroupRenamed(kGroup, "Sunday hike", 3, 2), kAnn);
  EXPECT_NE(rml.find(zh_ ? "Ann \xE5\xB0\x86\xE7\xBE\xA4\xE5\x90\x8D\xE6\x94\xB9\xE4\xB8\xBA Sunday hike"
                         : "Ann renamed the group to Sunday hike"),
            std::string::npos)
      << rml;
  const std::string mine = Line(GroupMembershipControlType::GroupRenamed,
                                GroupMembershipCodec::EncodeGroupRenamed(kGroup, "Sunday hike", 3, 2), kMe);
  EXPECT_NE(mine.find(zh_ ? "\xE4\xBD\xA0\xE5\xB0\x86\xE7\xBE\xA4\xE5\x90\x8D\xE6\x94\xB9\xE4\xB8\xBA Sunday hike"
                          : "You renamed the group to Sunday hike"),
            std::string::npos)
      << mine;
}

TEST_P(GroupEventLineTest, AHostileNameAndTitleAreEscapedAndNeverFilledIntoEachOther) {
  const std::string title = "{name}<i>t</i>";
  const std::string rml = Line(GroupMembershipControlType::GroupRenamed,
                               GroupMembershipCodec::EncodeGroupRenamed(kGroup, title, 3, 2), kHostile);
  EXPECT_EQ(rml.find("<b>"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("<i>"), std::string::npos) << rml;
  EXPECT_NE(rml.find("&lt;b&gt;&quot;x&quot;&lt;/b&gt; &#123;title&#125; &amp;amp;"), std::string::npos)
      << "the name shows as typed, its {title} left alone: " << rml;
  EXPECT_NE(rml.find("&#123;name&#125;&lt;i&gt;t&lt;/i&gt;"), std::string::npos)
      << "the title shows as typed, its {name} left alone: " << rml;
  const std::string joined = Line(GroupMembershipControlType::MemberJoined,
                                  GroupMembershipCodec::EncodeMemberJoined(kGroup, kHostile, MemberRole::Member, 2),
                                  kHostile);
  EXPECT_EQ(joined.find("<b>"), std::string::npos) << joined;
  EXPECT_NE(joined.find("&lt;b&gt;"), std::string::npos) << joined;
}

TEST_P(GroupEventLineTest, DeclinedOwnershipGroupFullAndForkedAreWordedToo) {
  const std::string declined = Line(GroupMembershipControlType::GroupInviteDecline,
                                    GroupMembershipCodec::EncodeInviteResponse("nonce-1", kGroup), kAnn);
  EXPECT_NE(declined.find(zh_ ? "Ann \xE6\x8B\x92\xE7\xBB\x9D\xE4\xBA\x86\xE9\x82\x80\xE8\xAF\xB7"
                              : "Ann declined the invitation"),
            std::string::npos)
      << declined;
  const std::string owner = Line(GroupMembershipControlType::OwnerTransferred,
                                 GroupMembershipCodec::EncodeOwnerTransferred(kGroup, kAnn, 3), kMe);
  EXPECT_NE(owner.find(zh_ ? "Ann \xE6\x88\x90\xE4\xB8\xBA\xE4\xBA\x86\xE6\x96\xB0\xE7\xBE\xA4\xE4\xB8\xBB"
                           : "Ann is now the group owner"),
            std::string::npos)
      << owner;
  const std::string you_owner = Line(GroupMembershipControlType::OwnerTransferred,
                                     GroupMembershipCodec::EncodeOwnerTransferred(kGroup, kMe, 3, true), kAnn);
  EXPECT_NE(you_owner.find(zh_ ? "\xE4\xBD\xA0\xE6\x88\x90\xE4\xB8\xBA\xE4\xBA\x86\xE6\x96\xB0\xE7\xBE\xA4\xE4\xB8\xBB"
                               : "You are now the group owner; the previous owner left"),
            std::string::npos)
      << you_owner;
  const std::string full = Line(GroupMembershipControlType::GroupInviteRefused,
                                GroupMembershipCodec::EncodeInviteRefused("nonce-1", kGroup, "group_full"), kAnn);
  EXPECT_NE(full.find(zh_ ? "\xE7\xBE\xA4\xE8\x81\x8A\xE5\xB7\xB2\xE6\xBB\xA1" : "The group is full"),
            std::string::npos)
      << full;
}

TEST(GroupEventLineFallbackTest, ANameIsLookedUpOncePerBuild) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  int lookups = 0;
  TranscriptFacts facts = Facts();
  facts.member_name = [&lookups](const std::string& account) {
    ++lookups;
    return account;
  };
  std::vector<TranscriptRow> rows;
  for (int i = 0; i < 20; ++i) {
    auto message = GroupMembershipCodec::BuildSystemMessage(
        "t", GroupMembershipControlType::MemberLeft, "Member left",
        *GroupMembershipCodec::EncodeMemberLeft(kGroup, kAnn, 3), kAnn);
    ASSERT_TRUE(message);
    message->id = "m" + std::to_string(i);
    TranscriptRow row;
    row.message = *message;
    rows.push_back(row);
  }
  EXPECT_EQ(RenderTranscript(rows, facts).size(), 20u);
  EXPECT_EQ(lookups, 1) << "twenty lines about one person: one lookup";
}

TEST(GroupEventLineFallbackTest, WithoutNamesOrADecodableDetailTheStoredTextShows) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  TranscriptFacts bare;  // no member_name: an old caller
  EXPECT_NE(Line(GroupMembershipControlType::MemberJoined,
                 GroupMembershipCodec::EncodeMemberJoined(kGroup, kAnn, MemberRole::Member, 2), kAnn, bare)
                .find("Stored English text"),
            std::string::npos);
  EXPECT_NE(Line(GroupMembershipControlType::MemberLeft, Roe<std::string>(std::string("not json")), kAnn)
                .find("Stored English text"),
            std::string::npos);
  // A kind with no wording here keeps its stored text.
  EXPECT_NE(Line(GroupMembershipControlType::GroupRosterRequest, Roe<std::string>(std::string("{}")), kMe)
                .find("Stored English text"),
            std::string::npos);
}

} // namespace
} // namespace pbr
