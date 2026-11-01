#include "gui/DiagnosticsFeedback.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace pbr {
namespace {

using Kind = DiagnosticsUploadKind;

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::set<std::string> DiagnosticsKeys(const std::string& locale_file) {
  const std::string text = ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "locales" / locale_file);
  const std::regex key("\"(settings\\.security\\.diagnostics\\.[a-z_]+)\"\\s*:");
  std::set<std::string> keys;
  for (auto it = std::sregex_iterator(text.begin(), text.end(), key); it != std::sregex_iterator(); ++it) {
    keys.insert((*it)[1]);
  }
  return keys;
}

DiagnosticsUploadResult Result(Kind kind, std::string id = {}, std::string detail = {}) {
  DiagnosticsUploadResult r;
  r.kind = kind;
  r.upload_id = std::move(id);
  r.detail = std::move(detail);
  return r;
}

class DiagnosticsFeedbackTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  }
};

} // namespace

TEST_F(DiagnosticsFeedbackTest, EveryResultMapsToItsLocaleKey) {
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::Sent), false), "settings.security.diagnostics.sent");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::SelfCheckRefused), false), "settings.security.diagnostics.refused");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::NoNetwork), false), "settings.security.diagnostics.no_network");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::QuotaKey), true), "settings.security.diagnostics.quota_key");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::QuotaKey), false), "settings.security.diagnostics.quota_key_no_id");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::QuotaIp), false), "settings.security.diagnostics.quota_limit");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::QuotaGlobal), false), "settings.security.diagnostics.quota_limit");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::InProgress), false), "settings.security.diagnostics.in_progress");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::Unavailable), false), "settings.security.diagnostics.unavailable");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::TooLarge), false), "settings.security.diagnostics.too_large");
  EXPECT_EQ(DiagnosticsResultKey(Result(Kind::Failed), false), "settings.security.diagnostics.failed");
}

TEST_F(DiagnosticsFeedbackTest, TextCarriesTheReferenceOrDetail) {
  EXPECT_EQ(DiagnosticsResultText(Result(Kind::Sent, "ABCDEFGH12"), "ABCDEFGH12"), "Sent. Your reference: ABCDEFGH12");
  EXPECT_EQ(DiagnosticsResultText(Result(Kind::QuotaKey), "OLDREF0001"),
            "Already sent today. Your reference: OLDREF0001");
  EXPECT_EQ(DiagnosticsResultText(Result(Kind::QuotaKey), ""), "Already sent today.");
  EXPECT_EQ(DiagnosticsResultText(Result(Kind::Failed, "", "HTTP 500"), ""), "Sending failed: HTTP 500");
  EXPECT_EQ(DiagnosticsResultText(Result(Kind::InProgress), ""),
            "The previous send is still being processed; check the reference in a minute.");
}

TEST_F(DiagnosticsFeedbackTest, NoMessageTellsTheUserToCheckTheirNetworkOrPermissions) {
  for (const Kind kind : {Kind::Sent, Kind::SelfCheckRefused, Kind::NoNetwork, Kind::QuotaKey, Kind::QuotaIp,
                          Kind::QuotaGlobal, Kind::InProgress, Kind::Unavailable, Kind::TooLarge, Kind::Failed}) {
    const std::string text = DiagnosticsResultText(Result(kind, "ID", "HTTP 500"), "ID");
    EXPECT_FALSE(text.empty());
    EXPECT_EQ(text.find("check your"), std::string::npos) << text;
    EXPECT_EQ(text.find("permission"), std::string::npos) << text;
  }
}

TEST(DiagnosticsLocaleTest, EveryDiagnosticsKeyExistsInBothLocales) {
  const std::set<std::string> required = {"send", "sending", "help", "help_question", "last_id", "confirm_title",
                                          "confirm_body", "confirm_ok", "sent", "refused", "no_network",
                                          "quota_key", "quota_key_no_id", "quota_limit", "in_progress",
                                          "unavailable", "too_large", "failed"};
  const std::set<std::string> en = DiagnosticsKeys("en.json");
  const std::set<std::string> zh = DiagnosticsKeys("zh-Hans.json");
  EXPECT_EQ(en, zh) << "en.json and zh-Hans.json must carry the same settings.security.diagnostics.* keys";
  for (const std::string& name : required) {
    EXPECT_TRUE(en.count("settings.security.diagnostics." + name)) << name;
  }
}

TEST(DiagnosticsViewTest, SecuritySectionHasBothButtonsTheLastIdLineAndTheSendingGuard) {
  const std::string rml =
      ReadAll(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "settings_section_security.rml");
  EXPECT_NE(rml.find("data-event-click=\"send_diagnostics()\""), std::string::npos);
  EXPECT_NE(rml.find("data-attrif-disabled=\"diagnostics_sending\""), std::string::npos) << "no second send while sending";
  EXPECT_NE(rml.find("data-event-click=\"open_diagnostics_help()\""), std::string::npos);
  EXPECT_NE(rml.find("data-help-key=\"troubleshooting/send-diagnostics\""), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"diagnostics_last_id != ''\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"copy_diagnostics_id()\""), std::string::npos);
  EXPECT_NE(rml.find("{{diagnostics_send_label}}"), std::string::npos);
}

} // namespace pbr
