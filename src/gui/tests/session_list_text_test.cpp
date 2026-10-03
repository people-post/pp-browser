#include "gui/chat/SessionListText.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

std::tm Day(const int year, const int month, const int day, const int yday, const int hour = 0, const int min = 0) {
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_yday = yday;
  tm.tm_hour = hour;
  tm.tm_min = min;
  return tm;
}

} // namespace

TEST(SessionListTextTest, ShortMessageBecomesTheTitleAsIs) {
  EXPECT_EQ(AiThreadTitleFromMessage("今天有什么新闻"), "今天有什么新闻");
  EXPECT_EQ(AiThreadTitleFromMessage("  hello \n\n world\t"), "hello world");
  EXPECT_EQ(AiThreadTitleFromMessage(" \n "), "");
}

TEST(SessionListTextTest, LongMessageIsCutOnACharacterBoundary) {
  const std::string title = AiThreadTitleFromMessage("请给我一条今天最新的政治新闻并且详细说明它的背景和各方的反应以及后续影响");
  EXPECT_EQ(title, "请给我一条今天最新的政治新闻并且详细说明它的背景…");

  const std::string latin = AiThreadTitleFromMessage("abcdefghijklmnopqrstuvwxyz0123456789");
  EXPECT_EQ(latin, "abcdefghijklmnopqrstuvwx…");

  // Exactly at the limit: no ellipsis.
  EXPECT_EQ(AiThreadTitleFromMessage("abcdefghijklmnopqrstuvwx"), "abcdefghijklmnopqrstuvwx");
}

TEST(SessionListTextTest, DateLabelShowsDateAndTimeThisYearAndFullDateBefore) {
  const std::tm now = Day(2026, 10, 3, 275, 18, 30);
  EXPECT_EQ(SessionDateLabel(Day(2026, 10, 3, 275, 9, 5), now), "10/3 09:05");
  EXPECT_EQ(SessionDateLabel(Day(2026, 10, 2, 274, 23, 59), now), "10/2 23:59");
  EXPECT_EQ(SessionDateLabel(Day(2026, 1, 15, 14), now), "1/15 00:00");
  EXPECT_EQ(SessionDateLabel(Day(2025, 12, 31, 364), now), "2025/12/31");
}

TEST(SessionListTextTest, UnsetTimestampHasNoLabel) {
  EXPECT_EQ(SessionDateLabel(int64_t{0}, int64_t{1700000000000}), "");
}
