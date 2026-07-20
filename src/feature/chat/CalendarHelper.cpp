#include "feature/chat/CalendarHelper.h"

#include "base/platform/os/OsTime.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <optional>
#include <sstream>

namespace pbr {

namespace {

constexpr std::array<const char*, 12> kMonthNames = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

bool IsLeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int DaysInMonth(int month, int year) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 30;
  }
  if (month == 2 && IsLeapYear(year)) {
    return 29;
  }
  return kDays[static_cast<size_t>(month - 1)];
}

int WeekdayOfFirst(int month, int year) {
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = 1;
  std::mktime(&tm);
  return tm.tm_wday;
}

std::optional<int> ParseIsoDateParts(const std::string& iso) {
  if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') {
    return std::nullopt;
  }
  try {
    const int year = std::stoi(iso.substr(0, 4));
    const int month = std::stoi(iso.substr(5, 2));
    const int day = std::stoi(iso.substr(8, 2));
    if (month < 1 || month > 12 || day < 1 || day > 31) {
      return std::nullopt;
    }
    return year * 10000 + month * 100 + day;
  } catch (...) {
    return std::nullopt;
  }
}

bool IsDateInRange(int year, int month, int day, const std::string& min_date, const std::string& max_date) {
  const std::optional<int> value = ParseIsoDateParts(FormatIsoDate(year, month, day));
  if (!value) {
    return false;
  }
  if (!min_date.empty()) {
    const std::optional<int> min_value = ParseIsoDateParts(min_date);
    if (min_value && *value < *min_value) {
      return false;
    }
  }
  if (!max_date.empty()) {
    const std::optional<int> max_value = ParseIsoDateParts(max_date);
    if (max_value && *value > *max_value) {
      return false;
    }
  }
  return true;
}

Rml::String MonthLabel(int month, int year) {
  if (month < 1 || month > 12) {
    return Rml::String("Calendar");
  }
  std::ostringstream out;
  out << kMonthNames[static_cast<size_t>(month - 1)] << " " << year;
  return Rml::String(out.str().c_str());
}

void AppendWeeksFromFlatDays(CalendarWidgetState& state, std::vector<CalendarDayRow>& flat_days) {
  while (flat_days.size() % 7 != 0) {
    CalendarDayRow pad;
    pad.label = Rml::String("");
    pad.available = false;
    flat_days.push_back(std::move(pad));
  }

  state.weeks.clear();
  state.weeks.reserve(flat_days.size() / 7);
  for (size_t i = 0; i < flat_days.size(); i += 7) {
    CalendarWeekRow week;
    week.days.assign(flat_days.begin() + static_cast<std::ptrdiff_t>(i),
                     flat_days.begin() + static_cast<std::ptrdiff_t>(i + 7));
    state.weeks.push_back(std::move(week));
  }
}

} // namespace

TodayYmd TodayLocalDate() {
  TodayYmd today;
  const std::time_t now = std::time(nullptr);
  std::tm local_tm{};
  if (!os::LocalTime(now, &local_tm)) {
    return today;
  }
  today.year = local_tm.tm_year + 1900;
  today.month = local_tm.tm_mon + 1;
  today.day = local_tm.tm_mday;
  return today;
}

CalendarConfig TodayCalendarConfig() {
  const TodayYmd today = TodayLocalDate();
  CalendarConfig config;
  config.month = today.month;
  config.year = today.year;
  return config;
}

std::string FormatIsoDate(int year, int month, int day) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
  return buffer;
}

CalendarWidgetState BuildCalendarState(const CalendarConfig& config) {
  CalendarWidgetState state;
  state.month = config.month;
  state.year = config.year;
  state.min_date = Rml::String(config.min_date.c_str());
  state.max_date = Rml::String(config.max_date.c_str());
  state.month_label = MonthLabel(config.month, config.year);
  for (const std::string& day : config.available_days) {
    state.available_days.push_back(Rml::String(day.c_str()));
  }

  std::vector<CalendarDayRow> flat_days;
  const int leading = WeekdayOfFirst(config.month, config.year);
  const int days_in_month = DaysInMonth(config.month, config.year);

  for (int i = 0; i < leading; ++i) {
    CalendarDayRow pad;
    pad.label = Rml::String("");
    pad.available = false;
    flat_days.push_back(std::move(pad));
  }

  for (int day = 1; day <= days_in_month; ++day) {
    CalendarDayRow row;
    row.day = day;
    row.label = Rml::String(std::to_string(day).c_str());
    const std::string iso = FormatIsoDate(config.year, config.month, day);
    row.iso_date = Rml::String(iso.c_str());

    bool available = IsDateInRange(config.year, config.month, day, config.min_date, config.max_date);
    if (!config.available_days.empty()) {
      available = available && std::find(config.available_days.begin(), config.available_days.end(), iso) !=
                                    config.available_days.end();
    }
    row.available = available;
    flat_days.push_back(std::move(row));
  }

  AppendWeeksFromFlatDays(state, flat_days);
  return state;
}

void ShiftCalendarMonth(CalendarWidgetState& calendar, int delta_months) {
  int month = calendar.month + delta_months;
  int year = calendar.year;
  while (month < 1) {
    month += 12;
    --year;
  }
  while (month > 12) {
    month -= 12;
    ++year;
  }

  CalendarConfig config;
  config.month = month;
  config.year = year;
  config.min_date = std::string(calendar.min_date.c_str());
  config.max_date = std::string(calendar.max_date.c_str());
  for (const Rml::String& day : calendar.available_days) {
    config.available_days.push_back(std::string(day.c_str()));
  }
  calendar = BuildCalendarState(config);
}

std::string MockCalendarReplyJson() {
  const TodayYmd today = TodayLocalDate();
  const std::string today_iso = FormatIsoDate(today.year, today.month, today.day);
  const int month_days = DaysInMonth(today.month, today.year);
  const std::string month_start = FormatIsoDate(today.year, today.month, 1);
  const std::string month_end = FormatIsoDate(today.year, today.month, month_days);

  std::ostringstream available;
  available << "\"" << today_iso << "\"";
  for (int offset : {1, 2, 5}) {
    int day = today.day + offset;
    if (day <= month_days) {
      available << ", \"" << FormatIsoDate(today.year, today.month, day) << "\"";
    }
  }

  std::ostringstream out;
  out << R"JSON({
    "blocks": [
      { "type": "paragraph", "text": "Pick an available date:" },
      { "type": "calendar",
        "min_date": ")JSON" << month_start << R"JSON(",
        "max_date": ")JSON" << month_end << R"JSON(",
        "available_days": [)JSON" << available.str() << R"JSON(]
      }
    ]
  })JSON";
  return out.str();
}

} // namespace pbr
