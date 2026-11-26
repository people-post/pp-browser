#include "gui/chat/ForwardPreview.h"

#include "lib/messaging/AttachmentCache.h"
#include "lib/voice/tests/voice_test_support.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace pbr {
namespace {

namespace fs = std::filesystem;

/** The smallest byte string the thumbnail check reads as a JPEG of `w` x `h`: SOI, a baseline SOF0 segment, EOI. */
std::vector<uint8_t> JpegOfSize(const int w, const int h) {
  return {0xFF,
          0xD8,
          0xFF,
          0xC0,
          0x00,
          0x0B,
          0x08,
          static_cast<uint8_t>(h >> 8),
          static_cast<uint8_t>(h & 0xFF),
          static_cast<uint8_t>(w >> 8),
          static_cast<uint8_t>(w & 0xFF),
          0x01,
          0x01,
          0x11,
          0x00,
          0xFF,
          0xD9};
}

size_t FilesUnder(const fs::path& root) {
  size_t n = 0;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::end(it); it.increment(ec)) {
    if (it->is_regular_file()) {
      ++n;
    }
  }
  return n;
}

TEST(ForwardPreviewFilesTest, ForwardPreviewFilesAreRemovedWhenThePickerCloses) {
  voice_test::TempDir profile;
  ForwardPreviewFiles files(profile.path.string());
  const std::string path = files.Put(JpegOfSize(160, 120), 0);
  ASSERT_FALSE(path.empty());
  ASSERT_TRUE(fs::exists(path));
  files.RemoveAll(); // what the picker's close does (ChatController: forward_ports.preview_done)
  EXPECT_FALSE(fs::exists(path));
  EXPECT_EQ(FilesUnder(profile.path), 0u);
  files.RemoveAll(); // nothing there: no trouble
  EXPECT_FALSE(files.Put(JpegOfSize(160, 120), 1).empty()) << "a new page can make new files after the old ones went";
}

TEST(ForwardPreviewFilesTest, ForwardPreviewFilesAreRemovedOnLockAndSignOut) {
  voice_test::TempDir profile;
  ForwardPreviewFiles files(profile.path.string());
  const std::string path = files.Put(JpegOfSize(160, 120), 0);
  ASSERT_TRUE(fs::exists(path));
  // The folder is a view folder of the profile (threads/<reserved name>/blobs_view): the wipe a lock or a sign-out runs
  // over every attachment view (AttachmentFetchWorkflow::ClearDek) takes it with the others.
  EXPECT_EQ(fs::path(path).parent_path(),
            fs::path(AttachmentViewRoot(profile.path.string(), kForwardPreviewViewThread)));
  ASSERT_TRUE(WipeAllAttachmentViewCaches(profile.path.string()));
  EXPECT_FALSE(fs::exists(path));
}

TEST(ForwardPreviewFilesTest, LeftoverForwardPreviewFilesAreRemovedAtStart) {
  voice_test::TempDir profile;
  {
    ForwardPreviewFiles killed(profile.path.string()); // the app was killed with the page up: no RemoveAll
    ASSERT_FALSE(killed.Put(JpegOfSize(160, 120), 0).empty());
    ASSERT_EQ(FilesUnder(profile.path), 1u);
  }
  // Next start: ConversationsHub::CleanStaleAttachmentFiles runs this wipe once per start.
  ASSERT_TRUE(WipeAllAttachmentViewCaches(profile.path.string()));
  EXPECT_EQ(FilesUnder(profile.path), 0u);
  // And the first file of a new page clears whatever an earlier page left, even without that start wipe.
  {
    ForwardPreviewFiles killed(profile.path.string());
    ASSERT_FALSE(killed.Put(JpegOfSize(160, 120), 0).empty());
  }
  ForwardPreviewFiles next(profile.path.string());
  ASSERT_FALSE(next.Put(JpegOfSize(160, 120), 5).empty());
  EXPECT_EQ(FilesUnder(profile.path), 1u);
}

TEST(ForwardPreviewFilesTest, AForwardPreviewFileIsOnlyAThumbnail) {
  voice_test::TempDir profile;
  ForwardPreviewFiles files(profile.path.string());
  EXPECT_TRUE(files.Put(JpegOfSize(256, 192), 0).empty() == false) << "256 on the long side is the limit";
  EXPECT_TRUE(files.Put(JpegOfSize(600, 450), 1).empty()) << "a larger picture is never written";
  EXPECT_TRUE(files.Put(JpegOfSize(100, 4000), 2).empty()) << "the long side counts, whichever it is";
  EXPECT_TRUE(files.Put({0x89, 'P', 'N', 'G'}, 3).empty()) << "only a JPEG whose size can be read";
  EXPECT_TRUE(files.Put({}, 4).empty());
  EXPECT_EQ(FilesUnder(profile.path), 1u);
  // The name is a counter: no content hash, no message id.
  for (auto it = fs::recursive_directory_iterator(profile.path); it != fs::end(it); ++it) {
    if (it->is_regular_file()) {
      EXPECT_EQ(it->path().filename().string(), "0.jpg");
    }
  }
  // The size the page asks the thumbnailer for is within the limit too.
  EXPECT_LE(kForwardPreviewThumbPx, kForwardPreviewMaxLongSidePx);
}

} // namespace
} // namespace pbr
