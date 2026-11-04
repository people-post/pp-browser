#include "feature/presenters/HomeToday.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ChatSessionRow Row(const std::string& id, const int unread, const bool stranger = false, const int64_t at = 0) {
  ChatSessionRow row;
  row.id = id;
  row.title = "T-" + id;
  row.unread_count = unread;
  row.from_stranger = stranger;
  row.updated_at = at;
  return row;
}

ChatSessionRow Direct(const std::string& id, const std::string& contact, const int64_t at, const bool blocked = false) {
  ChatSessionRow row = Row(id, 0, false, at);
  row.kind = "private";
  row.contact_id = contact;
  row.title = contact;
  row.blocked = blocked;
  return row;
}

const HomeWidgetLayout kAll = HomeWidgetLayoutFrom({}, {});

std::vector<std::string> Ids(const std::vector<ChatSessionRow>& rows) {
  std::vector<std::string> ids;
  for (const ChatSessionRow& row : rows) {
    ids.push_back(row.id);
  }
  return ids;
}

std::vector<std::string> ContactIds(const std::vector<HomeFavorite>& favorites) {
  std::vector<std::string> ids;
  for (const HomeFavorite& favorite : favorites) {
    ids.push_back(favorite.contact_id);
  }
  return ids;
}

TEST(HomeTodayTest, RecentChatsAreTheThreeLatestReadOrNotWhateverIsPinned) {
  // The list is pinned first; Home takes the newest by time.
  const std::vector<ChatSessionRow> rows = {Row("pinned-old", 0, false, 10), Row("a", 2, false, 50),
                                            Row("read", 0, false, 40), Row("b", 0, false, 30)};
  const HomeTodaySummary summary = SummarizeHomeToday(rows, kAll);
  EXPECT_EQ(Ids(summary.recent), (std::vector<std::string>{"a", "read", "b"}));

  // Reading every chat (unread 0 everywhere) leaves the widget full: it is not "new messages" any more.
  std::vector<ChatSessionRow> all_read = rows;
  for (ChatSessionRow& row : all_read) {
    row.unread_count = 0;
  }
  EXPECT_EQ(Ids(SummarizeHomeToday(all_read, kAll).recent), (std::vector<std::string>{"a", "read", "b"}));
  EXPECT_TRUE(SummarizeHomeToday({}, kAll).recent.empty());
}

TEST(HomeTodayTest, NewPeopleCountUnreadChatsWithStrangersAndOpenTheFirst) {
  const HomeTodaySummary summary = SummarizeHomeToday(
      {Row("friend", 3), Row("stranger-read", 0, true), Row("s1", 1, true), Row("s2", 4, true)}, kAll);
  EXPECT_EQ(summary.new_contacts, 2);
  EXPECT_EQ(summary.first_stranger_thread_id, "s1");
  EXPECT_TRUE(SummarizeHomeToday({Row("friend", 3)}, kAll).first_stranger_thread_id.empty());
}

TEST(HomeTodayTest, MentionsCountEveryUnreadMentionAndNameTheLatestChat) {
  ChatSessionRow hiking = Row("hiking", 3, false, 100);
  hiking.mention_count = 2;
  hiking.preview = "Fang: @you Saturday?";
  ChatSessionRow work = Row("work", 1, false, 200);
  work.mention_count = 1;
  work.preview = "Bob: @you the doc";
  const HomeTodaySummary summary = SummarizeHomeToday({hiking, Row("quiet", 5, false, 300), work}, kAll);
  EXPECT_EQ(summary.mentions.count, 3);
  EXPECT_EQ(summary.mentions.thread_id, "work");
  EXPECT_EQ(summary.mentions.title, "T-work");
  EXPECT_EQ(summary.mentions.preview, "Bob: @you the doc");
  EXPECT_EQ(SummarizeHomeToday({Row("quiet", 5)}, kAll).mentions.count, 0);
}

TEST(HomeTodayTest, MissedCallsCountTodaysUnansweredCallsAndShowTheLatest) {
  const int64_t midnight = 1'000'000'000;
  const int64_t now = midnight + 10LL * 60 * 60 * 1000;
  const auto call = [](const std::string& id, int64_t at, bool missed) {
    call_queries::RecentCall c;
    c.call_id = id;
    c.thread_id = "thread-" + id;
    c.incoming = true;
    c.missed = missed;
    c.at_ms = at;
    return c;
  };
  const HomeCalls calls = SummarizeHomeCalls(
      {call("answered", now - 1000, false), call("missed-2", now - 2000, true), call("missed-1", midnight + 5, true),
       call("yesterday", midnight - 5, true)},
      midnight, now);
  EXPECT_EQ(calls.missed_today, 2) << "yesterday's missed call is not today's";
  ASSERT_TRUE(calls.latest.has_value());
  EXPECT_EQ(calls.latest->call_id, "answered");

  EXPECT_FALSE(SummarizeHomeCalls({}, midnight, now).latest.has_value()) << "no calls: the widget hides";
  const HomeCalls old = SummarizeHomeCalls({call("old", now - kHomeCallsRecentMs - 1, false)}, midnight, now);
  EXPECT_FALSE(old.latest.has_value()) << "nothing in the last week";
}

TEST(HomeTodayTest, FavoritesAreStarredFirstThenRecentContactsAndFollowTheStar) {
  const std::vector<ChatSessionRow> rows = {
      Direct("t-ann", "ann", 500), Direct("t-bob", "bob", 400), Direct("t-cy", "cy", 300),
      Direct("t-cy-secure", "cy", 600),  // a contact's newer chat is the one opened
      Direct("t-dee", "dee", 200, /*blocked=*/true), Row("group", 0, false, 900), Row("stranger", 1, true, 800)};
  // Nobody starred: the most recent contacts (a blocked one, a group and a stranger never show).
  std::vector<HomeFavorite> favorites = HomeFavorites(rows, {}, {});
  EXPECT_EQ(ContactIds(favorites), (std::vector<std::string>{"cy", "ann", "bob"}));
  EXPECT_EQ(favorites[0].thread_id, "t-cy-secure");

  // Bob starred: first, before newer chats. Eve starred too, with no chat yet (looked up by the caller).
  const std::vector<std::string> starred = {"bob", "eve"};
  EXPECT_EQ(StarredWithoutChat(rows, starred), std::vector<std::string>{"eve"});
  const HomeFavorite eve{.contact_id = "eve", .thread_id = "", .title = "Eve", .avatar_letter = "E"};
  favorites = HomeFavorites(rows, starred, {eve});
  EXPECT_EQ(ContactIds(favorites), (std::vector<std::string>{"bob", "eve", "cy", "ann"}));
  EXPECT_TRUE(favorites[1].thread_id.empty()) << "a tap starts the chat";

  // Unstarred: Bob is back in his place by time.
  EXPECT_EQ(ContactIds(HomeFavorites(rows, {"eve"}, {eve})), (std::vector<std::string>{"eve", "cy", "ann", "bob"}));
}

TEST(HomeTodayTest, AWidgetWithNothingToShowIsHiddenButFavoritesAlwaysShows) {
  const HomeTodaySummary empty = SummarizeHomeToday({}, kAll);
  EXPECT_EQ(ShownHomeWidgets(kAll, HomeWidgetDataOf(empty, {}, 0)), std::vector<std::string>{kHomeWidgetFavorites});

  ChatSessionRow mention = Row("g", 1, false, 5);
  mention.mention_count = 1;
  const HomeTodaySummary busy = SummarizeHomeToday({mention, Row("s", 1, true, 4)}, kAll);
  HomeCalls calls;
  calls.latest = call_queries::RecentCall{};
  EXPECT_EQ(ShownHomeWidgets(kAll, HomeWidgetDataOf(busy, calls, 3)),
            (std::vector<std::string>{kHomeWidgetFavorites, kHomeWidgetCalls, kHomeWidgetMentions,
                                      kHomeWidgetRecentChats, kHomeWidgetBrief, kHomeWidgetNewPeople}))
      << "every widget, in the default order";
}

TEST(HomeTodayTest, ARemovedWidgetIsNeitherShownNorComputed) {
  ChatSessionRow mention = Row("g", 1, false, 5);
  mention.mention_count = 2;
  const std::vector<ChatSessionRow> rows = {mention, Row("s", 1, true, 4)};
  const HomeWidgetLayout layout =
      RemoveHomeWidget(RemoveHomeWidget(RemoveHomeWidget(kAll, kHomeWidgetRecentChats), kHomeWidgetMentions),
                       kHomeWidgetNewPeople);
  const HomeTodaySummary summary = SummarizeHomeToday(rows, layout);
  EXPECT_TRUE(summary.recent.empty()) << "not computed";
  EXPECT_EQ(summary.mentions.count, 0) << "not computed";
  EXPECT_EQ(summary.new_contacts, 0) << "not computed";
  HomeCalls calls;
  calls.latest = call_queries::RecentCall{};
  EXPECT_EQ(ShownHomeWidgets(RemoveHomeWidget(layout, kHomeWidgetCalls), HomeWidgetDataOf(summary, calls, 3)),
            (std::vector<std::string>{kHomeWidgetFavorites, kHomeWidgetBrief}));
}

TEST(HomeTodayTest, TheLayoutIsStoredAsTheRemovedIdsAndAWidgetPutBackGoesLast) {
  EXPECT_EQ(kAll.order, HomeWidgetIds()) << "nothing stored: every widget, default order";
  EXPECT_TRUE(HiddenHomeWidgets(kAll).empty());

  const HomeWidgetLayout no_calls = RemoveHomeWidget(kAll, kHomeWidgetCalls);
  EXPECT_FALSE(no_calls.Kept(kHomeWidgetCalls));
  EXPECT_EQ(HiddenHomeWidgets(no_calls), std::vector<std::string>{kHomeWidgetCalls});
  EXPECT_EQ(no_calls.Addable(), std::vector<std::string>{kHomeWidgetCalls});
  EXPECT_EQ(HomeWidgetLayoutFrom(no_calls.order, HiddenHomeWidgets(no_calls)), no_calls) << "round trip";

  const HomeWidgetLayout back = AddHomeWidget(no_calls, kHomeWidgetCalls);
  EXPECT_TRUE(back.Kept(kHomeWidgetCalls));
  EXPECT_EQ(back.OnHome().back(), kHomeWidgetCalls) << "back at the end of Home";
  EXPECT_EQ(RemoveHomeWidget(kAll, "weather"), kAll) << "an unknown id changes nothing";
  EXPECT_EQ(HomeWidgetLayoutFrom({}, {"weather", "messages", "brief"}).hidden, std::set<std::string>{"brief"})
      << "unknown and retired ids are ignored";
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
