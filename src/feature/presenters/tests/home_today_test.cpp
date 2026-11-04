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

TEST(HomeTodayTest, NewPeopleOpensTheFirstStrangerChatInListOrder) {
  const HomeTodaySummary summary =
      SummarizeHomeToday({Row("friend", 3), Row("stranger-read", 0, true), Row("s1", 1, true), Row("s2", 4, true)});
  EXPECT_EQ(summary.new_contacts, 2);
  EXPECT_EQ(summary.first_stranger_thread_id, "s1");
  EXPECT_TRUE(SummarizeHomeToday({Row("friend", 3)}).first_stranger_thread_id.empty());
}

TEST(HomeTodayTest, AWidgetWithNothingToShowIsHidden) {
  const HomeWidgetsShown none = ShownHomeWidgets(SummarizeHomeToday({Row("a", 0)}), 0);
  EXPECT_FALSE(none.messages);
  EXPECT_FALSE(none.new_contacts);
  EXPECT_FALSE(none.brief);

  const HomeWidgetsShown friends_only = ShownHomeWidgets(SummarizeHomeToday({Row("friend", 2)}), 0);
  EXPECT_TRUE(friends_only.messages);
  EXPECT_FALSE(friends_only.new_contacts) << "no unread chat from a stranger";
  EXPECT_FALSE(friends_only.brief) << "no headlines";

  const HomeWidgetsShown all = ShownHomeWidgets(SummarizeHomeToday({Row("s", 1, true)}), 3);
  EXPECT_TRUE(all.messages);
  EXPECT_TRUE(all.new_contacts);
  EXPECT_TRUE(all.brief);
}

TEST(HomeTodayTest, AWidgetTheUserRemovedIsNotShownEvenWithData) {
  const HomeTodaySummary busy = SummarizeHomeToday({Row("friend", 2), Row("s", 1, true)});
  HomeWidgetChoices choices;
  choices.brief = false;
  const HomeWidgetsShown shown = ShownHomeWidgets(busy, 3, choices);
  EXPECT_TRUE(shown.messages);
  EXPECT_TRUE(shown.new_contacts);
  EXPECT_FALSE(shown.brief);

  choices = {.messages = false, .new_contacts = false, .brief = true};
  const HomeWidgetsShown only_brief = ShownHomeWidgets(busy, 3, choices);
  EXPECT_FALSE(only_brief.messages);
  EXPECT_FALSE(only_brief.new_contacts);
  EXPECT_TRUE(only_brief.brief);
}

TEST(HomeTodayTest, EditHomeChoicesAreStoredAsTheIdsSwitchedOff) {
  EXPECT_EQ(HomeWidgetChoicesFromHidden({}), HomeWidgetChoices{}) << "nothing stored: every widget on";
  EXPECT_TRUE(HiddenHomeWidgets(HomeWidgetChoices{}).empty());

  const HomeWidgetChoices no_brief = ToggleHomeWidget(HomeWidgetChoices{}, "brief");
  EXPECT_FALSE(no_brief.brief);
  EXPECT_TRUE(no_brief.messages);
  EXPECT_EQ(HiddenHomeWidgets(no_brief), (std::vector<std::string>{"brief"}));
  EXPECT_EQ(HomeWidgetChoicesFromHidden(HiddenHomeWidgets(no_brief)), no_brief) << "round trip";
  EXPECT_TRUE(ToggleHomeWidget(no_brief, "brief").brief) << "switched back on";

  const HomeWidgetChoices all_off =
      ToggleHomeWidget(ToggleHomeWidget(no_brief, "messages"), "new_people");
  EXPECT_EQ(HiddenHomeWidgets(all_off), (std::vector<std::string>{"messages", "new_people", "brief"}));
  EXPECT_EQ(ToggleHomeWidget(all_off, "weather"), all_off) << "an unknown id changes nothing";
  EXPECT_EQ(HomeWidgetChoicesFromHidden({"weather", "messages"}),
            (HomeWidgetChoices{.messages = false, .new_contacts = true, .brief = true}));
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
