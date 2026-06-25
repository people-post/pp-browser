#include "feature/chat/CalendarHelper.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  const pbr::TodayYmd today = pbr::TodayLocalDate();
  assert(today.month >= 1 && today.month <= 12);
  assert(today.day >= 1 && today.day <= 31);

  pbr::CalendarConfig config;
  config.month = 6;
  config.year = 2026;
  config.min_date = "2026-06-01";
  config.max_date = "2026-06-30";
  config.available_days = {"2026-06-15", "2026-06-16"};

  pbr::CalendarWidgetState state = pbr::BuildCalendarState(config);
  assert(!state.weeks.empty());
  assert(std::string(state.month_label.c_str()).find("June") != std::string::npos);

  int available_count = 0;
  int cell_count = 0;
  for (const pbr::CalendarWeekRow& week : state.weeks) {
    assert(week.days.size() == 7);
    cell_count += static_cast<int>(week.days.size());
    for (const pbr::CalendarDayRow& day : week.days) {
      if (day.available) {
        ++available_count;
        const std::string iso(day.iso_date.c_str());
        assert(iso == "2026-06-15" || iso == "2026-06-16");
      }
    }
  }
  assert(cell_count >= 35);
  assert(available_count == 2);

  pbr::ShiftCalendarMonth(state, 1);
  assert(state.month == 7);
  assert(state.year == 2026);

  const pbr::CalendarConfig today_config = pbr::TodayCalendarConfig();
  assert(today_config.month == today.month);
  assert(today_config.year == today.year);

  const pbr::CalendarWidgetState today_state = pbr::BuildCalendarState(today_config);
  assert(today_state.month == today.month);
  assert(today_state.year == today.year);

  const std::string mock_json = pbr::MockCalendarReplyJson();
  assert(mock_json.find("calendar") != std::string::npos);
  assert(mock_json.find(std::to_string(today.year)) != std::string::npos);

  std::cout << "calendar_helper_test passed\n";
  return 0;
}
