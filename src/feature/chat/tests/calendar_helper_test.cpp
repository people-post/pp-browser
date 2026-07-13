#include "feature/chat/CalendarHelper.h"

#include <gtest/gtest.h>

#include <string>

TEST(CalendarHelperTest, TodayLocalDateIsValid) {
  const pbr::TodayYmd today = pbr::TodayLocalDate();
  EXPECT_GE(today.month, 1);
  EXPECT_LE(today.month, 12);
  EXPECT_GE(today.day, 1);
  EXPECT_LE(today.day, 31);
}

TEST(CalendarHelperTest, BuildsJune2026Calendar) {
  pbr::CalendarConfig config;
  config.month = 6;
  config.year = 2026;
  config.min_date = "2026-06-01";
  config.max_date = "2026-06-30";
  config.available_days = {"2026-06-15", "2026-06-16"};

  pbr::CalendarWidgetState state = pbr::BuildCalendarState(config);
  ASSERT_FALSE(state.weeks.empty());
  EXPECT_NE(std::string(state.month_label.c_str()).find("June"), std::string::npos);

  int available_count = 0;
  int cell_count = 0;
  for (const pbr::CalendarWeekRow& week : state.weeks) {
    EXPECT_EQ(week.days.size(), 7u);
    cell_count += static_cast<int>(week.days.size());
    for (const pbr::CalendarDayRow& day : week.days) {
      if (day.available) {
        ++available_count;
        const std::string iso(day.iso_date.c_str());
        EXPECT_TRUE(iso == "2026-06-15" || iso == "2026-06-16");
      }
    }
  }
  EXPECT_GE(cell_count, 35);
  EXPECT_EQ(available_count, 2);
}

TEST(CalendarHelperTest, ShiftsMonth) {
  pbr::CalendarConfig config;
  config.month = 6;
  config.year = 2026;
  pbr::CalendarWidgetState state = pbr::BuildCalendarState(config);

  pbr::ShiftCalendarMonth(state, 1);
  EXPECT_EQ(state.month, 7);
  EXPECT_EQ(state.year, 2026);
}

TEST(CalendarHelperTest, TodayConfigAndMockJson) {
  const pbr::TodayYmd today = pbr::TodayLocalDate();
  const pbr::CalendarConfig today_config = pbr::TodayCalendarConfig();
  EXPECT_EQ(today_config.month, today.month);
  EXPECT_EQ(today_config.year, today.year);

  const pbr::CalendarWidgetState today_state = pbr::BuildCalendarState(today_config);
  EXPECT_EQ(today_state.month, today.month);
  EXPECT_EQ(today_state.year, today.year);

  const std::string mock_json = pbr::MockCalendarReplyJson();
  EXPECT_NE(mock_json.find("calendar"), std::string::npos);
  EXPECT_NE(mock_json.find(std::to_string(today.year)), std::string::npos);
}
