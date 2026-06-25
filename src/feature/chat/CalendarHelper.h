#pragma once

#include "feature/chat/ChatWidgetTypes.h"

#include <string>
#include <vector>

namespace pbr {

struct CalendarConfig {
  int month = 1;
  int year = 2000;
  std::string min_date;
  std::string max_date;
  std::vector<std::string> available_days;
};

struct TodayYmd {
  int year = 2000;
  int month = 1;
  int day = 1;
};

TodayYmd TodayLocalDate();
CalendarConfig TodayCalendarConfig();
std::string FormatIsoDate(int year, int month, int day);

CalendarWidgetState BuildCalendarState(const CalendarConfig& config);

void ShiftCalendarMonth(CalendarWidgetState& calendar, int delta_months);

std::string MockCalendarReplyJson();

} // namespace pbr
