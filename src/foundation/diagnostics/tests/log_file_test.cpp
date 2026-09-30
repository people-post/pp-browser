#include "foundation/diagnostics/LogFile.h"

#include "common/Logger.h"
#include "common/Metrics.h"
#include "common/PbrCompat.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

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
           (std::string("pp-browser-log-file-test-") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
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

TEST_F(LogFileTest, RotateShiftsPreviousLaunchesAndDropsOldest) {
  WriteText(path_, "current");
  WriteText(pbr::LogFile::RotatedPath(path_, 1), "prev1");
  WriteText(pbr::LogFile::RotatedPath(path_, 2), "prev2");

  pbr::LogFile::Rotate(path_, 2);

  EXPECT_FALSE(fs::exists(path_));
  EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(path_, 1)), "current");
  EXPECT_EQ(ReadAll(pbr::LogFile::RotatedPath(path_, 2)), "prev1");
  EXPECT_FALSE(fs::exists(pbr::LogFile::RotatedPath(path_, 3)));
}

TEST_F(LogFileTest, RotateSkipsGapsAndMissingCurrent) {
  WriteText(pbr::LogFile::RotatedPath(path_, 2), "prev2");

  pbr::LogFile::Rotate(path_, 5);

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

TEST_F(LogFileTest, RotateWithZeroKeepRemovesCurrent) {
  WriteText(path_, "current");

  pbr::LogFile::Rotate(path_, 0);

  EXPECT_FALSE(fs::exists(path_));
  EXPECT_FALSE(fs::exists(pbr::LogFile::RotatedPath(path_, 1)));
}

} // namespace
