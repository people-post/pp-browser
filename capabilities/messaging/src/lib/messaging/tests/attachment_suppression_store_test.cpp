#include "lib/messaging/AttachmentSuppressionStore.h"

#include "common/Utilities.h"
#include "foundation/crypto/AttachmentContentHash.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pbr {
namespace {

// attachment_suppressions.json (R020; DATA_LAYOUT): what the person deleted stays deleted. INVENTORY S3: the file says
// its schema from now on; a file without one (every file before) is today's; a newer app's is refused and never
// written over.
class AttachmentSuppressionStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_attachment_suppressions_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path Path() const { return dir_ / "attachment_suppressions.json"; }
  std::string Read() const {
    std::ifstream in(Path());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  void Write(const std::string& text) const { std::ofstream(Path(), std::ios::trunc) << text; }

  static std::vector<uint8_t> Hash(const uint8_t fill) {
    return std::vector<uint8_t>(kAttachmentContentHashSize, fill);
  }

  std::filesystem::path dir_;
};

TEST_F(AttachmentSuppressionStoreTest, AFileSaysItsVersionAndOneWithoutIsToday) {
  {
    AttachmentSuppressionStore store(dir_.string());
    ASSERT_TRUE(store.Suppress("thread-1", Hash(1)));
  }
  EXPECT_NE(Read().find("\"schema_version\": 1"), std::string::npos) << Read();

  const std::string unversioned =
      R"({"threads":{"thread-2":[")" + std::string(kAttachmentContentHashSize * 2, 'b') + R"("]}})";
  Write(unversioned);
  AttachmentSuppressionStore store(dir_.string());
  EXPECT_TRUE(store.IsSuppressed("thread-2", Hash(0xbb)));
}

TEST_F(AttachmentSuppressionStoreTest, ANewerFileIsRefusedAndNeverWrittenOver) {
  const std::string newer = R"({"schema_version":2,"threads":{},"later":true})";
  Write(newer);
  AttachmentSuppressionStore store(dir_.string());
  EXPECT_FALSE(store.IsSuppressed("thread-1", Hash(1)));
  auto suppressed = store.Suppress("thread-1", Hash(1));
  ASSERT_FALSE(suppressed);
  EXPECT_NE(suppressed.error().message.find("newer"), std::string::npos) << suppressed.error().message;
  EXPECT_FALSE(store.ClearThread("thread-1"));
  EXPECT_EQ(Read(), newer);
}

} // namespace
} // namespace pbr
