#include "foundation/diagnostics/LogFile.h"

#include "common/Logger.h"
#include "common/Diag.h"
#include "common/Metrics.h"
#include "common/PbrCompat.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string ReadAll(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteText(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::trunc);
  out << text;
}

class LogFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    // One directory per test: InstallMetrics leaves its FileHandler on the process-global Metrics
    // logger (the logger has no removeHandler), and on Windows an open file can be neither removed
    // nor renamed — a shared directory would break the next test when the binary runs in-process.
    dir_ = fs::temp_directory_path() /
           (std::string("pp-browser-log-file-test-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_ / "logs", ec);
    path_ = pbr::LogFile::DefaultPath(dir_.string());
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  fs::path dir_;
  std::string path_;
};

TEST_F(LogFileTest, DefaultPathUnderDataDirLogs) {
  EXPECT_EQ(fs::path(path_), dir_ / "logs" / "pp-browser.log");
  EXPECT_EQ(fs::path(pbr::LogFile::RotatedPath(path_, 2)), dir_ / "logs" / "pp-browser.2.log");
}

#if !defined(_WIN32)
// PB-062: the log and metrics files are owner-only from the moment they exist (logs carry ids and
// addresses); Windows uses ACLs and is not checked here.
TEST_F(LogFileTest, InstalledFilesAreOwnerOnly) {
  const std::string installed = pbr::LogFile::Install(path_);
  ASSERT_EQ(installed, path_);
  EXPECT_EQ(pbr::LogFile::InstalledPath(), path_) << "Send run logs reads the file the app opened";
  const std::string metrics = pbr::LogFile::InstallMetrics(path_);
  ASSERT_FALSE(metrics.empty());
  const auto owner_only = fs::perms::owner_read | fs::perms::owner_write;
  EXPECT_EQ(fs::status(path_).permissions(), owner_only);
  EXPECT_EQ(fs::status(metrics).permissions(), owner_only);
}
#endif

TEST_F(LogFileTest, RotateShiftsEveryPreviousLaunch) {
  WriteText(path_, "current");
  WriteText(pbr::LogFile::RotatedPath(path_, 1), "prev1");
  WriteText(pbr::LogFile::RotatedPath(path_, 2), "prev2");

  pbr::LogFile::Rotate(path_);

  EXPECT_FALSE(fs::exists(path_));
  EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(path_, 1)), "current");
  EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(path_, 2)), "prev1");
  EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(path_, 3)), "prev2");
}

// Installing only rotates: whatever is on disk stays until the caller knows the retention and prunes
// (DEV/PLAN-send-run-logs.md: the "Collect run logs" switch is read after the log is installed).
TEST_F(LogFileTest, RotateAndInstallNeverDelete) {
  WriteText(path_, "current");
  for (std::size_t i = 1; i <= 5; ++i) {
    WriteText(pbr::LogFile::RotatedPath(path_, i), "old");
    fs::last_write_time(pbr::LogFile::RotatedPath(path_, i),
                        fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));
  }

  ASSERT_EQ(pbr::LogFile::Install(path_), path_); // rotates: the old current is .1, the month-old files .2 to .6

  for (std::size_t i = 1; i <= 6; ++i) {
    EXPECT_TRUE(fs::exists(pbr::LogFile::RotatedPath(path_, i))) << i;
  }
}

TEST_F(LogFileTest, PruneDeletesFilesOlderThanTheLimit) {
  WriteText(path_, "current");
  WriteText(pbr::LogFile::RotatedPath(path_, 1), "fresh");
  WriteText(pbr::LogFile::RotatedPath(path_, 2), "stale");
  fs::last_write_time(pbr::LogFile::RotatedPath(path_, 2),
                      fs::file_time_type::clock::now() - std::chrono::hours(24 * 8));

  pbr::LogFile::Prune(path_, pbr::LogFile::EffectiveRetention(/*collect=*/true));

  EXPECT_EQ(ReadAll(path_), "current");
  EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(path_, 1)), "fresh");
  EXPECT_FALSE(fs::exists(pbr::LogFile::RotatedPath(path_, 2)));
}

TEST_F(LogFileTest, PruneDeletesOldestWhenOverTheSizeCap) {
  pbr::LogRetention retention;
  retention.max_bytes = 10;
  const auto now = fs::file_time_type::clock::now();
  WriteText(pbr::LogFile::RotatedPath(path_, 1), "aaaaaa");
  WriteText(pbr::LogFile::RotatedPath(path_, 2), "bbbbbb");
  fs::last_write_time(pbr::LogFile::RotatedPath(path_, 1), now - std::chrono::hours(1));
  fs::last_write_time(pbr::LogFile::RotatedPath(path_, 2), now - std::chrono::hours(2));

  pbr::LogFile::Prune(path_, retention);

  EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(path_, 1)), "aaaaaa");
  EXPECT_FALSE(fs::exists(pbr::LogFile::RotatedPath(path_, 2)));
}

// With the switch off the rule is max_age 0: the running launch (current, already installed) and the one before
// (.1) stay, even when .1 is weeks old; every older file goes. Each family in the directory is pruned.
TEST_F(LogFileTest, OffKeepsThisLaunchAndThePreviousOneInEveryFamily) {
  const auto now = fs::file_time_type::clock::now();
  for (const std::string& family : {path_, pbr::LogFile::MetricsPath(path_), pbr::LogFile::DiagPath(path_)}) {
    WriteText(family, "current");
    for (std::size_t i = 1; i <= 4; ++i) {
      WriteText(pbr::LogFile::RotatedPath(family, i), "launch " + std::to_string(i));
      fs::last_write_time(pbr::LogFile::RotatedPath(family, i), now - std::chrono::hours(24 * 20) * i);
    }
  }

  pbr::LogFile::PruneAll(path_, pbr::LogFile::EffectiveRetention(/*collect=*/false));

  for (const std::string& family : {path_, pbr::LogFile::MetricsPath(path_), pbr::LogFile::DiagPath(path_)}) {
    EXPECT_EQ(ReadAll(family), "current") << family;
    EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(family, 1)), "launch 1") << family;
    for (std::size_t i = 2; i <= 4; ++i) {
      EXPECT_FALSE(fs::exists(pbr::LogFile::RotatedPath(family, i))) << family << " " << i;
    }
  }
}

TEST(LogRetentionTest, EffectiveRetentionIsSevenDaysOnAndTwoLaunchesOff) {
  const pbr::LogRetention on = pbr::LogFile::EffectiveRetention(true);
  EXPECT_EQ(on.max_age, std::chrono::hours(24 * 7));
  EXPECT_EQ(on.max_bytes, 50ull * 1024 * 1024);
  const pbr::LogRetention off = pbr::LogFile::EffectiveRetention(false);
  EXPECT_EQ(off.max_age, std::chrono::hours(0));
}

TEST_F(LogFileTest, FamilyFilesListsTheCurrentAndRotatedFilesOfOneFamilyOnly) {
  WriteText(path_, "current");
  WriteText(pbr::LogFile::RotatedPath(path_, 2), "two");
  WriteText(pbr::LogFile::MetricsPath(path_), "metrics");
  WriteText((fs::path(path_).parent_path() / "pp-browser.x.log").string(), "not ours");
  std::vector<pbr::LogFileInfo> files = pbr::LogFile::FamilyFiles(path_);
  ASSERT_EQ(files.size(), 2u);
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  EXPECT_EQ(files[0].name, pbr::LogFile::RotatedPath(path_, 2));
  EXPECT_FALSE(files[0].current);
  EXPECT_EQ(files[1].name, path_);
  EXPECT_TRUE(files[1].current);
  EXPECT_EQ(files[1].size, 7u);
}

TEST(LogRetentionTest, ExpiredFilesUnderLimitsDeletesNothing) {
  const auto now = fs::file_time_type::clock::now();
  EXPECT_TRUE(pbr::LogFile::ExpiredFiles({}, now, {}).empty());
  const std::vector<pbr::LogFileInfo> files = {{"a", now - std::chrono::hours(1), 100, false},
                                               {"b", now - std::chrono::hours(24 * 6), 100, false}};
  EXPECT_TRUE(pbr::LogFile::ExpiredFiles(files, now, {}).empty());
}

TEST(LogRetentionTest, ExpiredFilesDeletesOlderThanMaxAge) {
  const auto now = fs::file_time_type::clock::now();
  const std::vector<pbr::LogFileInfo> files = {{"new", now - std::chrono::hours(1), 1, false},
                                               {"old", now - std::chrono::hours(24 * 7 + 1), 1, false}};
  EXPECT_EQ(pbr::LogFile::ExpiredFiles(files, now, {}), std::vector<std::string>({"old"}));
}

TEST(LogRetentionTest, ExpiredFilesDeletesOldestUntilUnderTheSizeCap) {
  const auto now = fs::file_time_type::clock::now();
  pbr::LogRetention retention;
  retention.max_bytes = 250;
  const std::vector<pbr::LogFileInfo> files = {{"newest", now - std::chrono::hours(1), 100, false},
                                               {"oldest", now - std::chrono::hours(3), 100, false},
                                               {"middle", now - std::chrono::hours(2), 100, false}};
  EXPECT_EQ(pbr::LogFile::ExpiredFiles(files, now, retention), std::vector<std::string>({"oldest"}));
  retention.max_bytes = 150;
  EXPECT_EQ(pbr::LogFile::ExpiredFiles(files, now, retention), std::vector<std::string>({"oldest", "middle"}));
}

TEST(LogRetentionTest, ExpiredFilesNeverDeletesTheCurrentFile) {
  const auto now = fs::file_time_type::clock::now();
  pbr::LogRetention retention;
  retention.max_bytes = 50;
  // The current file alone is over the cap and old: it stays; its size still pushes the older ones out.
  const std::vector<pbr::LogFileInfo> files = {{"current", now - std::chrono::hours(24 * 30), 100, true},
                                               {"prev", now - std::chrono::hours(1), 10, false},
                                               {"older", now - std::chrono::hours(2), 10, false}};
  EXPECT_EQ(pbr::LogFile::ExpiredFiles(files, now, retention), std::vector<std::string>({"older"}));
}

TEST(LogRetentionTest, ExpiredFilesKeepsTheNewestRotatedFileEvenWhenOldOrOverTheCap) {
  const auto now = fs::file_time_type::clock::now();
  // A crash log opened again 8 days later: the previous launch's file is past the age limit and kept; the one before
  // it, equally old, goes.
  const std::vector<pbr::LogFileInfo> aged = {{"prev", now - std::chrono::hours(24 * 8), 1, false},
                                              {"older", now - std::chrono::hours(24 * 9), 1, false},
                                              {"current", now, 1, true}};
  EXPECT_EQ(pbr::LogFile::ExpiredFiles(aged, now, {}), std::vector<std::string>({"older"}));
  // Alone over the size cap: still kept.
  pbr::LogRetention retention;
  retention.max_bytes = 10;
  const std::vector<pbr::LogFileInfo> big = {{"prev", now - std::chrono::hours(1), 100, false}};
  EXPECT_TRUE(pbr::LogFile::ExpiredFiles(big, now, retention).empty());
}

TEST(LogRetentionTest, ExpiredFilesEqualTimesKeepInputOrder) {
  const auto now = fs::file_time_type::clock::now();
  pbr::LogRetention retention;
  retention.max_bytes = 10;
  const auto same = now - std::chrono::hours(1);
  const std::vector<pbr::LogFileInfo> files = {{"first", same, 10, false}, {"second", same, 10, false}};
  EXPECT_EQ(pbr::LogFile::ExpiredFiles(files, now, retention), std::vector<std::string>({"first"}));
}

TEST_F(LogFileTest, RotateSkipsGapsAndMissingCurrent) {
  WriteText(pbr::LogFile::RotatedPath(path_, 2), "prev2");

  pbr::LogFile::Rotate(path_);

  EXPECT_FALSE(fs::exists(path_));
  EXPECT_FALSE(fs::exists(pbr::LogFile::RotatedPath(path_, 1)));
  EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(path_, 3)), "prev2");
}

// Device test 2026-09-30: an app opened normally (root level WARNING) wrote no metrics at all —
// the Metrics logger had no handler of its own and the root dropped its INFO lines.
TEST_F(LogFileTest, MetricsReachTheirOwnFileAtAnyRootLevel) {
  auto root = pbr::logging::getRootLogger();
  const auto saved = root.getLevel();
  root.setLevel(pbr::logging::Level::WARNING);

  const std::string metrics = pbr::LogFile::InstallMetrics(path_);
  ASSERT_EQ(fs::path(metrics), dir_ / "logs" / "metrics.log");
  pbr::MetricsLine("test.event").Add("n", 7).Emit();
  root.setLevel(saved);

  const std::string text = ReadAll(metrics);
  EXPECT_NE(text.find("[Metrics] event=test.event n=7"), std::string::npos) << text;
}

TEST_F(LogFileTest, InstallMetricsRotatesThePreviousLaunch) {
  WriteText(pbr::LogFile::MetricsPath(path_), "previous launch");
  ASSERT_FALSE(pbr::LogFile::InstallMetrics(path_).empty());
  EXPECT_EQ(ReadAll((dir_ / "logs" / "metrics.1.log").string()), "previous launch");
}

// The diagnostics a user sends read diag.log: the Diag channel's lines, and nothing from any other channel.
TEST_F(LogFileTest, DiagLinesReachTheirOwnFileOnly) {
  auto root = pbr::logging::getRootLogger();
  const auto saved = root.getLevel();
  root.setLevel(pbr::logging::Level::WARNING);

  const std::string diag = pbr::LogFile::InstallDiag(path_);
  ASSERT_EQ(fs::path(diag), dir_ / "logs" / "diag.log");
  pbr::DiagLine("test.diag").Add("n", 8).Emit();
  pbr::logging::getLogger("SomeModule").warning << "a developer line naming Alice";
  root.setLevel(saved);

  const std::string text = ReadAll(diag);
  EXPECT_NE(text.find("[Diag] event=test.diag n=8"), std::string::npos) << text;
  EXPECT_EQ(text.find("Alice"), std::string::npos) << text;
}

} // namespace
