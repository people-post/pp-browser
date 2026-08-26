#include "common/CivilTime.h"

#include <gtest/gtest.h>

#include <ctime>

TEST(CivilTimeTest, UtcTimeRoundTripNoon) {
  std::tm in{};
  in.tm_year = 2026 - 1900;
  in.tm_mon = 7; // August
  in.tm_mday = 17;
  in.tm_hour = 12;
  in.tm_min = 0;
  in.tm_sec = 0;
  in.tm_isdst = 0;
  const time_t unix_sec = pp::civil_time::TimeGm(&in);
  ASSERT_NE(unix_sec, static_cast<time_t>(-1));

  std::tm out{};
  ASSERT_TRUE(pp::civil_time::UtcTime(unix_sec, &out));
  EXPECT_EQ(out.tm_year, 2026 - 1900);
  EXPECT_EQ(out.tm_mon, 7);
  EXPECT_EQ(out.tm_mday, 17);
  EXPECT_EQ(out.tm_hour, 12);
  EXPECT_EQ(out.tm_min, 0);
  EXPECT_EQ(out.tm_sec, 0);
}

TEST(CivilTimeTest, LocalTimeSucceeds) {
  const time_t now = std::time(nullptr);
  std::tm local{};
  ASSERT_TRUE(pp::civil_time::LocalTime(now, &local));
  EXPECT_GE(local.tm_year, 125); // 2025+
}
