#include "foundation/platform/os/OsLocale.h"

#include <gtest/gtest.h>

#include <cctype>

// Kenneth (2026-10-13): a fresh install on the iPhone came up in English for a Chinese phone. Apple
// apps launched from the home screen carry no LANG / LC_* variables, so the POSIX reader found
// nothing; the Darwin reader asks the OS. Run with LANG / LC_ALL / LC_MESSAGES unset to prove it.
TEST(OsLocaleTest, ApplePreferredLocalesComeFromTheOsNotTheEnvironment) {
#if !defined(__APPLE__)
  GTEST_SKIP() << "POSIX / Win32 readers are environment-based by design";
#endif
  const auto locales = pbr::os::PreferredSystemLocales();
  ASSERT_FALSE(locales.empty());
  const std::string& first = locales.front();
  ASSERT_GE(first.size(), 2u);
  EXPECT_TRUE(std::isalpha(static_cast<unsigned char>(first[0])));
  EXPECT_TRUE(std::isalpha(static_cast<unsigned char>(first[1])));
}
