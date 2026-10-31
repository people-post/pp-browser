#include "foundation/platform/NativeFileDialog.h"
#include "foundation/platform/PickedFileName.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace pbr {
namespace {

TEST(PickedFileNameTest, ReadsTheNameOutOfAPathOrAContentUri) {
  EXPECT_EQ(DisplayNameFromPickedPath("/Users/me/clip.MOV"), "clip.MOV");
  EXPECT_EQ(DisplayNameFromPickedPath("content://com.android.externalstorage.documents/document/primary%3ADCIM%2FCamera%2FVID_1.mp4"),
            "VID_1.mp4");
  EXPECT_EQ(DisplayNameFromPickedPath("content://media/external/video/media/1234?x=1"), "") << "a media id is no name";
  EXPECT_EQ(DisplayNameFromPickedPath("content://com.android.providers.media.documents/document/video%3A1234"), "");
  EXPECT_EQ(DisplayNameFromPickedPath("content://x/document/My%20Trip.mp4"), "My Trip.mp4");
  EXPECT_EQ(DisplayNameFromPickedPath(""), "");
  EXPECT_EQ(DisplayNameFromPickedPath("/dir/noext"), "");
  EXPECT_EQ(DisplayNameFromPickedPath("/dir/.hidden"), "") << "a leading dot is not an extension";
  EXPECT_EQ(DisplayNameFromPickedPath("content://x/a%2F..%2F..%2Fetc%2Fpasswd.txt"), "passwd.txt")
      << "never a directory component";
}

class ImportPickedFileTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_import_pick_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
    source_ = (dir_ / "movie.mov").string();
    std::ofstream out(source_, std::ios::binary);
    const std::string chunk(1 << 20, 'v');
    for (int i = 0; i < 3; ++i) {
      out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }
    out << "tail";
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::filesystem::path dir_;
  std::string source_;
};

TEST_F(ImportPickedFileTest, APlainPathIsUsedAsItIsWithNoCopy) {
  auto pick = ImportPickedFile(source_, (dir_ / "scratch").string());
  ASSERT_TRUE(pick);
  EXPECT_EQ(pick->path, source_);
  EXPECT_EQ(pick->name, "movie.mov");
  EXPECT_FALSE(pick->copied);
  EXPECT_FALSE(std::filesystem::exists(dir_ / "scratch"));
}

// What a content:// URI goes through, tried on a plain file (Android's URIs open the same way through SDL).
TEST_F(ImportPickedFileTest, ACopyIsStreamedIntoTheScratchFolderWithItsNameAndRemovedAfter) {
  auto pick = ImportPickedFile(source_, (dir_ / "scratch").string(), /*force_copy=*/true);
  ASSERT_TRUE(pick);
  EXPECT_TRUE(pick->copied);
  EXPECT_NE(pick->path, source_);
  EXPECT_EQ(std::filesystem::path(pick->path).filename(), "movie.mov");
  EXPECT_EQ(std::filesystem::file_size(pick->path), std::filesystem::file_size(source_));
  EXPECT_EQ(std::filesystem::path(pick->path).parent_path().parent_path(), dir_ / "scratch");
  RemoveImportedPick(*pick);
  EXPECT_FALSE(std::filesystem::exists(pick->path));
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(pick->path).parent_path()));
  EXPECT_TRUE(std::filesystem::exists(source_)) << "the person's own file is never touched";
}

TEST_F(ImportPickedFileTest, AnUnreadableUriOrNoScratchFolderIsNothing) {
  EXPECT_FALSE(ImportPickedFile("content://nowhere/document/video%3A1", (dir_ / "scratch").string()));
  EXPECT_FALSE(ImportPickedFile("content://nowhere/document/video%3A1", ""));
  EXPECT_FALSE(ImportPickedFile((dir_ / "missing.mov").string(), (dir_ / "scratch").string(), /*force_copy=*/true));
  RemoveImportedPick(ImportedPick{});  // nothing to remove: no effect
  RemoveImportedPick(ImportedPick{.path = source_, .name = "movie.mov", .copied = false});
  EXPECT_TRUE(std::filesystem::exists(source_)) << "a plain pick is never deleted";
}

} // namespace
} // namespace pbr
