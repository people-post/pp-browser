// DEV/PLAN-desktop-drop-paste.md, D1: the one folder pasted pictures are written to, and the rule that only files inside
// it are ever deleted. Everything runs on the real file system inside the test's own temp folder.
#include "foundation/platform/PastedFiles.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace pbr {
namespace {

namespace fs = std::filesystem;

void Touch(const fs::path& path, const std::string& content = "x") {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << content;
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

class PastedFilesTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> counter{0};
    root_ = fs::temp_directory_path() /
            ("pp_pasted_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
             std::to_string(counter++));
    fs::create_directories(root_);
    dir_ = root_ / "cache" / "pasted";
    outside_ = root_ / "elsewhere";
    fs::create_directories(outside_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }
  fs::path root_;
  fs::path dir_;
  fs::path outside_;
};

TEST_F(PastedFilesTest, WritingMakesTheFolderAndNamesFilesInOrder) {
  const auto first = WritePastedFile(dir_, "one", "png");
  const auto second = WritePastedFile(dir_, "two", "png");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(fs::path(*first).filename(), "Pasted image 1.png");
  EXPECT_EQ(fs::path(*second).filename(), "Pasted image 2.png");
  EXPECT_EQ(Read(*first), "one");
  EXPECT_EQ(Read(*second), "two");
}

TEST_F(PastedFilesTest, AFailedWriteLeavesNothingBehind) {
  // The folder's place is taken by a plain file: it cannot be made.
  Touch(root_ / "cache" / "pasted", "i am a file");
  EXPECT_FALSE(WritePastedFile(dir_, "bytes", "png").has_value());
  EXPECT_EQ(Read(root_ / "cache" / "pasted"), "i am a file") << "the thing in the way is not touched";
}

TEST_F(PastedFilesTest, ReleaseDeletesAPlainFileInsideTheFolder) {
  const auto path = WritePastedFile(dir_, "x", "png");
  ASSERT_TRUE(path.has_value());
  EXPECT_TRUE(ReleasePastedFile(*path, dir_));
  EXPECT_FALSE(fs::exists(*path));
  EXPECT_FALSE(ReleasePastedFile(*path, dir_)) << "already gone";
}

TEST_F(PastedFilesTest, ReleaseRefusesAFileOutsideTheFolder) {
  const fs::path mine = outside_ / "my photo.png";
  Touch(mine);
  fs::create_directories(dir_);
  EXPECT_FALSE(ReleasePastedFile(mine.string(), dir_));
  EXPECT_TRUE(fs::exists(mine));
  // A sibling whose name starts like the folder's.
  const fs::path sibling = root_ / "cache" / "pasted-not" / "a.png";
  Touch(sibling);
  EXPECT_FALSE(ReleasePastedFile(sibling.string(), dir_));
  EXPECT_TRUE(fs::exists(sibling));
}

TEST_F(PastedFilesTest, ReleaseRefusesDotDotTricks) {
  const fs::path mine = outside_ / "my photo.png";
  Touch(mine);
  fs::create_directories(dir_);
  const std::string tricky = (dir_ / ".." / ".." / "elsewhere" / "my photo.png").string();
  EXPECT_FALSE(ReleasePastedFile(tricky, dir_));
  EXPECT_TRUE(fs::exists(mine));
}

TEST_F(PastedFilesTest, ReleaseRefusesALinkInsideTheFolderThatPointsOut) {
  const fs::path mine = outside_ / "my photo.png";
  Touch(mine);
  fs::create_directories(dir_);
  std::error_code ec;
  fs::create_symlink(mine, dir_ / "link.png", ec);
  if (ec) {
    GTEST_SKIP() << "no symlinks here";
  }
  EXPECT_FALSE(ReleasePastedFile((dir_ / "link.png").string(), dir_));
  EXPECT_TRUE(fs::exists(mine));
  EXPECT_TRUE(fs::is_symlink(dir_ / "link.png"));
}

TEST_F(PastedFilesTest, ReleaseRefusesAPathThroughAFolderLinkThatPointsOut) {
  const fs::path mine = outside_ / "my photo.png";
  Touch(mine);
  fs::create_directories(dir_);
  std::error_code ec;
  fs::create_directory_symlink(outside_, dir_ / "gate", ec);
  if (ec) {
    GTEST_SKIP() << "no symlinks here";
  }
  EXPECT_FALSE(ReleasePastedFile((dir_ / "gate" / "my photo.png").string(), dir_));
  EXPECT_TRUE(fs::exists(mine));
}

TEST_F(PastedFilesTest, ReleaseRefusesTheFolderItselfAFolderAndNothing) {
  fs::create_directories(dir_ / "sub");
  EXPECT_FALSE(ReleasePastedFile(dir_.string(), dir_));
  EXPECT_FALSE(ReleasePastedFile((dir_ / "sub").string(), dir_));
  EXPECT_FALSE(ReleasePastedFile("", dir_));
  EXPECT_TRUE(fs::exists(dir_ / "sub"));
}

TEST_F(PastedFilesTest, TheSweepEmptiesTheFolderAndTouchesNothingElse) {
  Touch(dir_ / "Pasted image 1.png");
  Touch(dir_ / "Pasted image 2.png");
  Touch(dir_ / "sub" / "deep.png");
  const fs::path profile_file = root_ / "profile" / "profile.db";
  const fs::path cache_file = root_ / "cache" / "thumb.bin";
  Touch(profile_file);
  Touch(cache_file);
  Touch(outside_ / "mine.png");
  std::error_code ec;
  fs::create_symlink(outside_ / "mine.png", dir_ / "link.png", ec);

  const size_t removed = SweepPastedDir(dir_);

  EXPECT_GE(removed, 3u);
  EXPECT_TRUE(fs::is_directory(dir_));
  EXPECT_TRUE(fs::is_empty(dir_));
  EXPECT_TRUE(fs::exists(profile_file));
  EXPECT_TRUE(fs::exists(cache_file));
  EXPECT_TRUE(fs::exists(outside_ / "mine.png")) << "a link inside is removed itself, its target stays";
}

TEST_F(PastedFilesTest, TheSweepOfAMissingFolderIsNothing) {
  EXPECT_EQ(SweepPastedDir(dir_), 0u);
  EXPECT_FALSE(fs::exists(dir_));
}

TEST_F(PastedFilesTest, TheSweepDoesNotFollowAFolderThatIsItselfALink) {
  Touch(outside_ / "mine.png");
  fs::create_directories(dir_.parent_path());
  std::error_code ec;
  fs::create_directory_symlink(outside_, dir_, ec);
  if (ec) {
    GTEST_SKIP() << "no symlinks here";
  }
  EXPECT_EQ(SweepPastedDir(dir_), 0u);
  EXPECT_TRUE(fs::exists(outside_ / "mine.png"));
}

} // namespace
} // namespace pbr
