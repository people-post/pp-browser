#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentPlatform.h"

#include "common/Utilities.h"
#include "foundation/crypto/AttachmentContentHash.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

// A view of an attachment is a file from someone else: the OS is told it was downloaded (quarantine), through
// the platform's hook the app installs (capability-repos CR015).
class AttachmentPlatformTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_attachment_platform_" + util::GenerateUuid());
  }
  void TearDown() override {
    SetAttachmentPlatform({});
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  Roe<std::string> StoreView() {
    const ByteVector plaintext = {'h', 'i'};
    auto hash = AttachmentContentHash(plaintext);
    if (!hash) {
      return hash.error();
    }
    return StoreAttachmentViewBytes(dir_.string(), "thread-1", *hash, "text/plain", "note.txt", plaintext);
  }

  std::filesystem::path dir_;
};

TEST_F(AttachmentPlatformTest, AWrittenViewIsMarkedAsDownloaded) {
  std::vector<std::string> marked;
  SetAttachmentPlatform({.mark_downloaded = [&marked](const std::string& path) -> Roe<void> {
    marked.push_back(path);
    return {};
  }});
  auto view = StoreView();
  ASSERT_TRUE(view) << view.error().message;
  EXPECT_EQ(marked, std::vector<std::string>{*view});
}

TEST_F(AttachmentPlatformTest, WithNoHookTheViewIsStillWritten) {
  auto view = StoreView();
  ASSERT_TRUE(view) << view.error().message;
  EXPECT_TRUE(std::filesystem::exists(*view));
}

} // namespace
} // namespace pbr
