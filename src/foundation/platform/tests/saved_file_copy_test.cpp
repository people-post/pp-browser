// CopyFileInChunks (DEV/PLAN-received-file-save-as.md step 2): a received file saved where the user chose, written
// beside the target and renamed over it only when whole. Failures are injected with the write hook, never with folder
// permissions.
#include "foundation/platform/SavedFileCopy.h"
#include "foundation/platform/os/OsFolderName.h"

#include "common/PathUtf8.h"
#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

class SavedFileCopyTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_saved_copy_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  static void Write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
  }
  static std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }
  /** Every name in the folder (the temporary `.<name>.pp-part` included). */
  size_t EntryCount() const {
    size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir_)) {
      ++count;
    }
    return count;
  }

  std::filesystem::path dir_;
};

/** 2.5 chunks of varied bytes: a copy that drops or repeats a chunk does not compare equal. */
std::string LargeContent() {
  std::string bytes(kSavedFileChunkBytes * 5 / 2, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>((i * 131 + i / 7) & 0xFF);
  }
  return bytes;
}

TEST_F(SavedFileCopyTest, AFileOverOneChunkIsCopiedByteForByte) {
  const std::string content = LargeContent();
  Write(dir_ / "view.bin", content);
  const auto target = dir_ / "saved.gz";
  size_t chunks = 0;
  ASSERT_TRUE(CopyFileInChunks(PathToUtf8(dir_ / "view.bin"), PathToUtf8(target), [&chunks](size_t) {
    ++chunks;
    return true;
  }));
  EXPECT_EQ(chunks, 3u) << "1 MB at a time";
  EXPECT_EQ(Read(target), content);
  EXPECT_EQ(EntryCount(), 2u) << "no temporary file left";
}

TEST_F(SavedFileCopyTest, AFailedWriteLeavesTheReplacedFileWholeAndNoTemporaryFile) {
  Write(dir_ / "view.bin", LargeContent());
  const auto target = dir_ / "report.pdf";
  Write(target, "the user's own file");
  const auto copied = CopyFileInChunks(PathToUtf8(dir_ / "view.bin"), PathToUtf8(target),
                                       [](const size_t chunk) { return chunk < 1; }); // the second chunk fails
  EXPECT_FALSE(copied);
  EXPECT_EQ(Read(target), "the user's own file");
  EXPECT_FALSE(std::filesystem::exists(dir_ / ".report.pdf.pp-part"));
  EXPECT_EQ(EntryCount(), 2u);
}

TEST_F(SavedFileCopyTest, AMissingFolderIsAnErrorAndLeavesNothing) {
  Write(dir_ / "view.bin", "abc");
  const auto target = dir_ / "gone" / "saved.txt";
  EXPECT_FALSE(CopyFileInChunks(PathToUtf8(dir_ / "view.bin"), PathToUtf8(target)));
  EXPECT_FALSE(std::filesystem::exists(dir_ / "gone"));
  EXPECT_EQ(EntryCount(), 1u);
}

TEST_F(SavedFileCopyTest, AMissingSourceCreatesNoTarget) {
  const auto target = dir_ / "saved.txt";
  EXPECT_FALSE(CopyFileInChunks(PathToUtf8(dir_ / "no-view.bin"), PathToUtf8(target)));
  EXPECT_EQ(EntryCount(), 0u);
}

TEST_F(SavedFileCopyTest, ANonAsciiNameIsKept) {
  Write(dir_ / "view.bin", "日志");
  const std::string target = PathToUtf8(dir_) + "/运行日志 2026.gz";
  ASSERT_TRUE(CopyFileInChunks(PathToUtf8(dir_ / "view.bin"), target));
  EXPECT_EQ(Read(PathFromUtf8(target)), "日志");
}

// The pill names the folder the file went to: its own name where the system has no localized one.
TEST_F(SavedFileCopyTest, TheFolderNameIsTheFoldersOwnNameOrTheSystemsName) {
  const auto folder = dir_ / "Saved here";
  std::filesystem::create_directories(folder);
  EXPECT_EQ(os::FolderDisplayName(PathToUtf8(folder)), "Saved here");
  EXPECT_EQ(os::FolderDisplayName(""), "");
}

} // namespace
} // namespace pbr
