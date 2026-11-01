// The time above a chat message: local time, relative to now, in the user's language.
#include "gui/chat/MessageTimeLabel.h"

#include "foundation/i18n/LocalizationService.h"

#include <ctime>
#include <gtest/gtest.h>

namespace pbr {
namespace {

// Unix ms of a local wall-clock time, so the tests do not depend on the machine's time zone.
int64_t LocalMs(const int year, const int month, const int day, const int hour, const int minute) {
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&tm)) * 1000;
}

class MessageTimeLabelTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
  const int64_t now = LocalMs(2026, 10, 29, 20, 0);
};

TEST_F(MessageTimeLabelTest, EnglishRelativeToNow) {
  EXPECT_EQ(MessageTimeLabel(LocalMs(2026, 10, 29, 9, 5), now), "09:05");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2026, 10, 28, 18, 32), now), "Yesterday 18:32");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2026, 10, 27, 18, 32), now), "Oct 27, 18:32");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2026, 1, 3, 7, 0), now), "Jan 3, 07:00");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2025, 10, 28, 18, 32), now), "Oct 28 2025, 18:32");
}

TEST_F(MessageTimeLabelTest, ChineseRelativeToNow) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2026, 10, 29, 18, 32), now), "18:32");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2026, 10, 28, 18, 32), now), "昨天 18:32");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2026, 10, 27, 18, 32), now), "10月27日 18:32");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2025, 10, 28, 18, 32), now), "2025年10月28日 18:32");
}

TEST_F(MessageTimeLabelTest, YesterdayAcrossAMonthAndYearBoundary) {
  EXPECT_EQ(MessageTimeLabel(LocalMs(2026, 9, 30, 23, 59), LocalMs(2026, 10, 1, 0, 1)), "Yesterday 23:59");
  EXPECT_EQ(MessageTimeLabel(LocalMs(2025, 12, 31, 12, 0), LocalMs(2026, 1, 1, 8, 0)), "Yesterday 12:00");
}

TEST_F(MessageTimeLabelTest, UnknownTimeGivesNothing) {
  EXPECT_TRUE(MessageTimeLabel(0, now).empty());
  EXPECT_TRUE(MessageTimeLabel(-5, now).empty());
}

} // namespace
} // namespace pbr
