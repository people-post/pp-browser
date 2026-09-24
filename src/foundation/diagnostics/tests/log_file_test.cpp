#include "foundation/diagnostics/LogFile.h"

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
    dir_ = fs::temp_directory_path() / "pp-browser-log-file-test";
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

TEST_F(LogFileTest, RotateWithZeroKeepRemovesCurrent) {
  WriteText(path_, "current");

  pbr::LogFile::Rotate(path_, 0);

  EXPECT_FALSE(fs::exists(path_));
  EXPECT_FALSE(fs::exists(pbr::LogFile::RotatedPath(path_, 1)));
}

} // namespace
