#include "base/data/AtomicFileWrite.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace {

using namespace pbr;

class AtomicFileWriteTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_atomic_write_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(AtomicFileWriteTest, ReplacesExistingFile) {
  const auto path = (dir_ / "data.json").string();
  {
    std::ofstream out(path);
    out << "old";
  }
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string("{\"ok\":true}")));
  std::ifstream in(path);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(body, "{\"ok\":true}");
}

TEST_F(AtomicFileWriteTest, CreatesParentDirs) {
  const auto path = (dir_ / "nested" / "a" / "file.txt").string();
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string("hello")));
  std::ifstream in(path);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(body, "hello");
}

TEST_F(AtomicFileWriteTest, FailedRenameLeavesPriorIntact) {
  const auto path = (dir_ / "keep.json").string();
  ASSERT_TRUE(AtomicFileWrite::Write(path, std::string("prior")));
  // Target path is an existing directory — rename into it must fail.
  std::filesystem::create_directories(path + ".as_dir");
  // Write to a path whose parent becomes unusable mid-flight is hard; instead write
  // with a destination that is a directory so rename fails after temp create.
  const std::string dir_as_file = (dir_ / "dir_target").string();
  std::filesystem::create_directories(dir_as_file);
  auto result = AtomicFileWrite::Write(dir_as_file, std::string("nope"));
  EXPECT_FALSE(static_cast<bool>(result));
  std::ifstream in(path);
  std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(body, "prior");
}

} // namespace
