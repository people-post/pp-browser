#include "feature/presenters/HomeToday.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ChatSessionRow Row(const std::string& id, const int unread, const bool stranger = false) {
  ChatSessionRow row;
  row.id = id;
  row.unread_count = unread;
  row.from_stranger = stranger;
  return row;
}

TEST(HomeTodayTest, NewMessagesShowAtMostThreeUnreadChatsInListOrder) {
  const HomeTodaySummary summary =
      SummarizeHomeToday({Row("a", 2), Row("read", 0), Row("b", 1), Row("c", 5), Row("d", 4), Row("e", 1)});
  ASSERT_EQ(summary.unread.size(), 3u);
  EXPECT_EQ(summary.unread[0].id, "a");
  EXPECT_EQ(summary.unread[1].id, "b");
  EXPECT_EQ(summary.unread[2].id, "c");
  // "See all" counts every unread message, like the Chats tab badge.
  EXPECT_EQ(summary.unread_total, 13);
}

TEST(HomeTodayTest, NothingUnreadHidesTheWidgets) {
  const HomeTodaySummary summary = SummarizeHomeToday({Row("a", 0), Row("b", 0, /*stranger=*/true)});
  EXPECT_TRUE(summary.unread.empty());
  EXPECT_EQ(summary.unread_total, 0);
  EXPECT_EQ(summary.new_contacts, 0);
  EXPECT_TRUE(SummarizeHomeToday({}).unread.empty());
}

TEST(HomeTodayTest, NewContactsCountUnreadChatsWithStrangersOnly) {
  const HomeTodaySummary summary =
      SummarizeHomeToday({Row("friend", 3), Row("stranger1", 1, true), Row("stranger2", 2, true),
                          Row("stranger-read", 0, true)});
  EXPECT_EQ(summary.new_contacts, 2);
}

TEST(HomeTodayTest, ContactsAndBlockedPeopleAreNotStrangers) {
  PeerDisplayLabel contact;
  contact.trust = PeerLabelTrust::Contact;
  contact.contact_id = "c1";
  // A blocked person is a saved contact; with no name yet the label falls back to the raw id but keeps the contact id.
  PeerDisplayLabel blocked_unnamed;
  blocked_unnamed.trust = PeerLabelTrust::RawId;
  blocked_unnamed.contact_id = "c2";
  PeerDisplayLabel raw;
  raw.trust = PeerLabelTrust::RawId;
  PeerDisplayLabel directory;
  directory.trust = PeerLabelTrust::DirectoryUnverified;

  EXPECT_FALSE(IsStrangerChat(ThreadKind::Direct, contact));
  EXPECT_FALSE(IsStrangerChat(ThreadKind::Direct, blocked_unnamed));
  EXPECT_TRUE(IsStrangerChat(ThreadKind::Direct, raw));
  EXPECT_TRUE(IsStrangerChat(ThreadKind::Direct, directory));
  EXPECT_FALSE(IsStrangerChat(ThreadKind::Group, raw));
  EXPECT_FALSE(IsStrangerChat(ThreadKind::Ai, raw));
}

class HomeDateTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }

  static std::tm Nov3() {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 10;
    tm.tm_mday = 3;
    tm.tm_wday = 2;  // Tuesday
    return tm;
  }
};

TEST_F(HomeDateTest, ChineseDateLine) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(HomeDateLabel(Nov3()), "11月3日 星期二");
}

TEST_F(HomeDateTest, EnglishDateLine) {
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(HomeDateLabel(Nov3()), "Tuesday, November 3");
}

} // namespace
} // namespace pbr
