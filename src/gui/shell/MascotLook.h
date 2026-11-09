#pragma once

#include "gui/shell/AiDotModel.h"

#include <span>
#include <string_view>

namespace pbr {

/**
 * The mascot's looks by day (Kenneth 2026-11-09: one look setting for the floating AI dot and the AI Home's logo;
 * "auto" changes it with the weekday). A look is one letter: `a`-`d` are the four looks of Me -> AI assistant (one
 * eye, two eyes, cool, plain), `e`-`g` the weekend looks.
 */
struct MascotDayLook {
  /** A date range inside one month, both ends included; a holiday look shows on these days instead of the weekday's. */
  int month = 0;
  int first_day = 0;
  int last_day = 0;
  char look = 'b';
};

/**
 * Date overrides (holiday looks, like a Google doodle): empty for now. Add `{month, first_day, last_day, look}` rows
 * here; `LookForDate` consults them before the weekday.
 */
inline std::span<const MascotDayLook> HolidayLooks() { return {}; }

/**
 * The look for a date: a row of `overrides` that holds the day first, else the weekday's. `weekday` counts from Sunday
 * (0 Sunday, 1 Monday ... 6 Saturday, std::tm::tm_wday). Mon a, Tue b, Wed c, Thu d, Fri e, Sat f, Sun g. `year` is not
 * used yet (the table has no moving holidays); it is here so one can be added without changing callers. Pure.
 */
inline char LookForDate(const int year, const int month, const int day, const int weekday,
                        const std::span<const MascotDayLook> overrides = HolidayLooks()) {
  (void)year;
  for (const MascotDayLook& row : overrides) {
    if (row.month == month && day >= row.first_day && day <= row.last_day) {
      return row.look;
    }
  }
  static constexpr char kWeek[7] = {'g', 'a', 'b', 'c', 'd', 'e', 'f'};
  return kWeek[((weekday % 7) + 7) % 7];
}

/** The weekday (0 Sunday ... 6 Saturday) of a Gregorian date (Sakamoto's method). */
inline int WeekdayOf(const int year, const int month, const int day) {
  static constexpr int kOffset[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  const int y = month < 3 ? year - 1 : year;
  return (y + y / 4 - y / 100 + y / 400 + kOffset[((month - 1) % 12 + 12) % 12] + day) % 7;
}

/**
 * The artwork a look is drawn with: every look has its own parts (`a`-`d` the four fixed looks, `e`-`g` the weekend
 * looks with their accessories, see MascotFigure.cpp); anything else falls back to `b`. The one place to change if a
 * look ever borrows another's artwork again.
 */
inline char PartsFor(const char look) {
  return look >= 'a' && look <= 'g' ? look : 'b';
}

inline AiDotLook DotLookOf(const char parts) {
  switch (parts) {
  case 'a':
    return AiDotLook::OneEye;
  case 'c':
    return AiDotLook::Cool;
  case 'd':
    return AiDotLook::Plain;
  case 'e':
    return AiDotLook::Evening;
  case 'f':
    return AiDotLook::Outing;
  case 'g':
    return AiDotLook::Lazy;
  default:
    return AiDotLook::TwoEyes;
  }
}

/** Me -> AI assistant -> Look: `auto` (the default) is the day's look, `a`-`g` a fixed one (anything else is auto). */
inline bool IsLookPref(const std::string_view pref) {
  return pref == "auto" || (pref.size() == 1 && pref[0] >= 'a' && pref[0] <= 'g');
}

/** The look to draw for the pref on a date (the dot and the home mascot both use this). */
inline char ResolveLook(const std::string_view pref, const int year, const int month, const int day,
                        const std::span<const MascotDayLook> overrides = HolidayLooks()) {
  if (pref.size() == 1 && pref != "auto" && IsLookPref(pref)) {
    return pref[0];
  }
  return LookForDate(year, month, day, WeekdayOf(year, month, day), overrides);
}

} // namespace pbr
