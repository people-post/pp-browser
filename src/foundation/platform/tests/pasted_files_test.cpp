// DEV/PLAN-desktop-drop-paste.md, D1: the one folder pasted pictures are written to, and the rule that only files inside
// it are ever deleted. Everything runs on the real file system inside the test's own temp folder.
#include "common/PathUtf8.h"
#include "foundation/platform/PastedFiles.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
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

// The folder sits under the user's profile or cache, which may have a Chinese name; paths are UTF-8 strings. On
// Windows a narrow path is the ANSI code page, so such a folder was unreadable (macOS passes either way).
TEST_F(PastedFilesTest, AChineseFolderWithASpaceTakesAWriteHandsBackUtf8AndReleasesIt) {
  const fs::path dir = root_ / PathFromUtf8("\xE7\x94\xA8\xE6\x88\xB7 \xE7\xBC\x93\xE5\xAD\x98") / "pasted";
  const auto written = WritePastedFile(dir, "png-bytes", "png");
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(*written, PathToUtf8(PathFromUtf8(*written))) << "the string is UTF-8, whatever the platform";
  EXPECT_NE(written->find("\xE7\x94\xA8\xE6\x88\xB7 \xE7\xBC\x93\xE5\xAD\x98"), std::string::npos);
  EXPECT_TRUE(fs::exists(PathFromUtf8(*written)));
  EXPECT_EQ(Read(PathFromUtf8(*written)), "png-bytes");
  EXPECT_TRUE(ReleasePastedFile(*written, dir));
  EXPECT_FALSE(fs::exists(PathFromUtf8(*written)));
}

TEST_F(PastedFilesTest, WritingMakesTheFolderAndNamesFilesInOrder) {
  const auto first = WritePastedFile(dir_, "one", "png");
  const auto second = WritePastedFile(dir_, "two", "png");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(fs::path(*first).filename(), fs::path(*second).filename());
  EXPECT_EQ(fs::path(*first).filename().string().rfind("Pasted image ", 0), 0u);
  EXPECT_EQ(fs::path(*first).extension(), ".png");
  EXPECT_EQ(Read(*first), "one");
  EXPECT_EQ(Read(*second), "two");
}

// N1: a released file's name is never handed out again, so a late second release of it cannot delete a newer file.
TEST_F(PastedFilesTest, ANameIsNeverReusedAfterItsFileIsReleased) {
  const auto first = WritePastedFile(dir_, "one", "png");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(ReleasePastedFile(*first, dir_));
  const auto second = WritePastedFile(dir_, "two", "png");
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_FALSE(ReleasePastedFile(*first, dir_)) << "the stale second release finds nothing";
  EXPECT_TRUE(fs::exists(*second));
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

// ---- S4: one folder per run, stale ones swept ----------------------------------------------------------------------------

void Age(const fs::path& path, const std::chrono::hours by) {
  std::error_code ec;
  fs::last_write_time(path, fs::file_time_type::clock::now() - by, ec);
}

/** Two "instances" share one cache root; each has its own run folder under <cache>/pasted. */
TEST_F(PastedFilesTest, ACleanExitRemovesThisRunsFolderWithWhatIsLeftInItAndNothingElse) {
  const fs::path mine = dir_ / "run-a";
  const fs::path other = dir_ / "run-b";
  Touch(mine / "Pasted image 1.png");
  Touch(mine / "sub" / "x.png");
  Touch(other / "Pasted image 2.png");
  Touch(dir_ / "stray.txt");
  EXPECT_TRUE(RemovePastedRunDir(dir_, mine));
  EXPECT_FALSE(fs::exists(mine));
  EXPECT_TRUE(fs::exists(other / "Pasted image 2.png")) << "another run's folder is never touched";
  EXPECT_TRUE(fs::exists(dir_ / "stray.txt"));
  EXPECT_TRUE(fs::is_directory(dir_)) << "the shared root stays";
  EXPECT_FALSE(RemovePastedRunDir(dir_, mine)) << "nothing there any more";
}

TEST_F(PastedFilesTest, ACleanExitRefusesTheRootAnythingDeeperAndAnythingOutside) {
  Touch(dir_ / "run-a" / "deep" / "f.png");
  Touch(outside_ / "mine.txt");
  EXPECT_FALSE(RemovePastedRunDir(dir_, dir_)) << "the shared root";
  EXPECT_FALSE(RemovePastedRunDir(dir_, dir_ / "run-a" / "deep")) << "not directly under the root";
  EXPECT_FALSE(RemovePastedRunDir(dir_, outside_)) << "outside the root";
  EXPECT_FALSE(RemovePastedRunDir(dir_, dir_ / ".." / "elsewhere")) << ".. does not get out";
  EXPECT_FALSE(RemovePastedRunDir(dir_, fs::path()));
  EXPECT_TRUE(fs::exists(dir_ / "run-a" / "deep" / "f.png"));
  EXPECT_TRUE(fs::exists(outside_ / "mine.txt"));
}

TEST_F(PastedFilesTest, ACleanExitNeverFollowsALink) {
  Touch(outside_ / "precious.txt");
  std::error_code ec;
  // The run folder itself is a link to a folder elsewhere: left alone.
  fs::create_directories(dir_);
  fs::create_directory_symlink(outside_, dir_ / "run-link", ec);
  if (ec) {
    GTEST_SKIP() << "this platform cannot make a folder link here";
  }
  EXPECT_FALSE(RemovePastedRunDir(dir_, dir_ / "run-link"));
  EXPECT_TRUE(fs::exists(outside_ / "precious.txt"));
  // A link inside the run folder is removed as itself; what it points at stays.
  Touch(dir_ / "run-a" / "f.png");
  fs::create_directory_symlink(outside_, dir_ / "run-a" / "gate", ec);
  ASSERT_FALSE(ec);
  EXPECT_TRUE(RemovePastedRunDir(dir_, dir_ / "run-a"));
  EXPECT_FALSE(fs::exists(dir_ / "run-a"));
  EXPECT_TRUE(fs::exists(outside_ / "precious.txt")) << "the link's target is untouched";
}

TEST_F(PastedFilesTest, ACleanExitRefusesWhenTheRootIsALink) {
  Touch(outside_ / "run-a" / "f.png");
  std::error_code ec;
  fs::create_directories(dir_.parent_path());
  fs::create_directory_symlink(outside_, root_ / "linked-root", ec);
  if (ec) {
    GTEST_SKIP() << "this platform cannot make a folder link here";
  }
  EXPECT_FALSE(RemovePastedRunDir(root_ / "linked-root", root_ / "linked-root" / "run-a"));
  EXPECT_TRUE(fs::exists(outside_ / "run-a" / "f.png"));
}

class PastedRunsTest : public PastedFilesTest {
protected:
  void SetUp() override {
    PastedFilesTest::SetUp();
    run_a_ = dir_ / "run-a";
    run_b_ = dir_ / "run-b";
  }
  fs::path run_a_;
  fs::path run_b_;
};

TEST_F(PastedRunsTest, AnotherInstancesSweepLeavesALiveFileAlone) {
  const auto live = WritePastedFile(run_a_, "mine", "png");
  ASSERT_TRUE(live.has_value());
  fs::create_directories(run_b_);
  EXPECT_EQ(SweepStalePastedRuns(dir_, run_b_), 0u);
  EXPECT_TRUE(fs::exists(*live));
  EXPECT_TRUE(fs::is_directory(run_b_)) << "its own folder stays too";
}

TEST_F(PastedRunsTest, ARunFolderLeftBehindLongAgoIsRemovedWhole) {
  Touch(run_a_ / "Pasted image 1.png");
  Touch(run_a_ / "sub" / "deep.png");
  for (const fs::path& path : {run_a_ / "Pasted image 1.png", run_a_ / "sub" / "deep.png", run_a_ / "sub", run_a_}) {
    Age(path, std::chrono::hours(48));
  }
  fs::create_directories(run_b_);
  EXPECT_EQ(SweepStalePastedRuns(dir_, run_b_), 1u);
  EXPECT_FALSE(fs::exists(run_a_));
  EXPECT_TRUE(fs::is_directory(run_b_));
}

TEST_F(PastedRunsTest, AFolderWithAnyRecentFileIsLive) {
  Touch(run_a_ / "old.png");
  Touch(run_a_ / "new.png");
  Age(run_a_ / "old.png", std::chrono::hours(72));
  Age(run_a_, std::chrono::hours(72));
  EXPECT_EQ(SweepStalePastedRuns(dir_, run_b_), 0u);
  EXPECT_TRUE(fs::exists(run_a_ / "old.png"));
}

TEST_F(PastedRunsTest, ARunsOwnFolderIsNeverSweptEvenWhenOld) {
  Touch(run_b_ / "x.png");
  Age(run_b_ / "x.png", std::chrono::hours(72));
  Age(run_b_, std::chrono::hours(72));
  EXPECT_EQ(SweepStalePastedRuns(dir_, run_b_), 0u);
  EXPECT_TRUE(fs::exists(run_b_ / "x.png"));
}

TEST_F(PastedRunsTest, AnOldFlatFileFromBeforeRunFoldersGoesToo) {
  Touch(dir_ / "Pasted image 1.png");
  Age(dir_ / "Pasted image 1.png", std::chrono::hours(48));
  Touch(dir_ / "Pasted image 2.png"); // fresh: another build's, maybe live
  EXPECT_EQ(SweepStalePastedRuns(dir_, run_b_), 1u);
  EXPECT_FALSE(fs::exists(dir_ / "Pasted image 1.png"));
  EXPECT_TRUE(fs::exists(dir_ / "Pasted image 2.png"));
}

TEST_F(PastedRunsTest, NothingOutsideThePastedFolderIsTouchedAndALinkIsNotFollowed) {
  const fs::path profile_file = root_ / "profile" / "profile.db";
  const fs::path cache_file = root_ / "cache" / "thumb.bin";
  Touch(profile_file);
  Touch(cache_file);
  Touch(outside_ / "mine.png");
  Touch(run_a_ / "x.png");
  Age(run_a_ / "x.png", std::chrono::hours(48));
  Age(run_a_, std::chrono::hours(48));
  std::error_code ec;
  fs::create_directory_symlink(outside_, dir_ / "run-link", ec);
  const bool linked = !ec;
  Age(profile_file, std::chrono::hours(48));
  Age(cache_file, std::chrono::hours(48));

  SweepStalePastedRuns(dir_, run_b_);

  EXPECT_FALSE(fs::exists(run_a_));
  EXPECT_TRUE(fs::exists(profile_file));
  EXPECT_TRUE(fs::exists(cache_file));
  EXPECT_TRUE(fs::exists(outside_ / "mine.png")) << "a link is judged and removed as itself, never followed";
  if (linked) {
    EXPECT_TRUE(fs::is_directory(outside_));
  }
}

TEST_F(PastedRunsTest, AMissingEmptyOrLinkedRootIsNothing) {
  EXPECT_EQ(SweepStalePastedRuns(dir_, run_b_), 0u);
  EXPECT_FALSE(fs::exists(dir_));
  EXPECT_EQ(SweepStalePastedRuns({}, run_b_), 0u);
  Touch(outside_ / "mine.png");
  Age(outside_ / "mine.png", std::chrono::hours(72));
  fs::create_directories(dir_.parent_path());
  std::error_code ec;
  fs::create_directory_symlink(outside_, dir_, ec);
  if (!ec) {
    EXPECT_EQ(SweepStalePastedRuns(dir_, run_b_), 0u);
    EXPECT_TRUE(fs::exists(outside_ / "mine.png"));
  }
}

TEST_F(PastedRunsTest, ARunIdIsRandomPerCallAndSafeAsAFolderName) {
  const std::string a = NewPastedRunId();
  const std::string b = NewPastedRunId();
  EXPECT_NE(a, b);
  EXPECT_GE(a.size(), 16u);
  for (const char c : a) {
    EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)) != 0) << a;
  }
}

} // namespace
} // namespace pbr
