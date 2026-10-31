#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

// Every string the attachment limits and status table show must exist in both language files, with the
// same {{placeholders}}, and (a missing key answers with the key itself) must not fall back to the key.
struct Key {
  const char* key;
  std::vector<std::string> placeholders;
};

const std::vector<Key>& AttachmentKeys() {
  static const std::vector<Key> keys = {
      {"chat.attach.photo", {}},
      {"chat.attach.original", {}},
      {"chat.attach.file", {}},
      {"chat.attachment.too_large", {"{{limit}}"}},
      {"chat.attachment.too_large_receive", {}},
      {"chat.attachment.sending_too_fast", {}},
      {"chat.attachment.read_failed", {}},
      {"chat.attachment.image_unprocessable", {}},
      {"chat.attachment.space_warning", {"{{percent}}"}},
      {"errors.blob.not_configured", {}},
      {"errors.blob.upload_rejected", {}},
      {"errors.blob.expired", {}},
      {"errors.blob.too_large", {}},
      {"errors.blob.unavailable", {}},
      {"errors.blob.rate_limited", {}},
      {"errors.blob.quota_exceeded", {}},
      {"blob.quota.message", {"{{usage}}", "{{detail}}"}},
      {"blob.quota.usage_files", {"{{used}}", "{{total}}"}},
      {"blob.quota.usage_bytes", {"{{used}}", "{{total}}"}},
  };
  return keys;
}

class AttachmentLocaleTest : public ::testing::TestWithParam<const char*> {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
};

TEST_P(AttachmentLocaleTest, EveryKeyIsTranslatedWithItsPlaceholders) {
  LocalizationService::Instance().SetPreferredLanguage(GetParam());
  for (const Key& key : AttachmentKeys()) {
    const std::string text = LocalizationService::Instance().Tr(key.key);
    EXPECT_NE(text, key.key) << GetParam() << " is missing " << key.key;
    for (const std::string& placeholder : key.placeholders) {
      EXPECT_NE(text.find(placeholder), std::string::npos) << GetParam() << " " << key.key << " lacks " << placeholder;
    }
  }
}

TEST_P(AttachmentLocaleTest, QuotaConfirmSaysThatPeopleWhoHaveNotDownloadedWillNotGetIt) {
  LocalizationService::Instance().SetPreferredLanguage(GetParam());
  const std::string text = LocalizationService::Instance().Tr("blob.quota.message", {{"usage", "U"}, {"detail", "D"}});
  EXPECT_NE(text.find(std::string(GetParam()) == "en" ? "not downloaded" : "没下载"), std::string::npos) << text;
}

INSTANTIATE_TEST_SUITE_P(Locales, AttachmentLocaleTest, ::testing::Values("en", "zh-Hans"));

} // namespace
} // namespace pbr
