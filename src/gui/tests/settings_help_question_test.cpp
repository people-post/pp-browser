// Settings "?" (Kenneth 2026-11-04): only beside the hard settings, not in every page's bar. A tap asks a ready question
// about that setting: on phones the AI answers it in the ask-AI panel as a one-off (DEV/PLAN-ui-batch5-ask-ai.md 原话
// 11–13; the panel keeps "Ask PP Support"), on the desktop it goes to PP 客服 as before.
#include "foundation/i18n/LocalizationService.h"
#include "gui/SettingsController.h"
#include "feature/settings/SettingsCommands.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

class SettingsHelpQuestionTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(SettingsHelpQuestionTest, TheQuestionNamesTheSettingInTheUiLanguage) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(SettingHelpQuestion("settings.security.crash_reports.label"), "「发送崩溃报告」是什么意思？");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(SettingHelpQuestion("settings.security.crash_reports.label"), "What does “Send crash reports” do?");
}

TEST_F(SettingsHelpQuestionTest, OnlySettingsLabelsAsk) {
  EXPECT_TRUE(SettingHelpQuestion("").empty());
  EXPECT_TRUE(SettingHelpQuestion("chat.menu.block").empty()) << "not a settings label";
  EXPECT_TRUE(SettingHelpQuestion("settings.no_such_label").empty()) << "a missing key asks nothing";
}

TEST_F(SettingsHelpQuestionTest, NoPageBarHasAQuestionMarkAnyMore) {
  const std::string rml = ReadAsset("views/settings.rml");
  EXPECT_EQ(rml.find("open_settings_help()"), std::string::npos);
  EXPECT_EQ(rml.find("settings-help-btn"), std::string::npos);
}

TEST_F(SettingsHelpQuestionTest, TheHardSettingsHaveOneBesideThem) {
  const struct {
    const char* view;
    const char* key;
  } expected[] = {
      {"settings_section_security.rml", "settings.security.crash_reports.label"},
      {"settings_section_security.rml", "settings.security.recovery"},
      {"settings_section_security.rml", "settings.security.transfer_row"},
      {"settings_section_security.rml", "settings.help.topic.account_copy"},
      {"settings_section_network.rml", "settings.network.help_network"},
      {"settings_section_network.rml", "settings.network.circuit_relay"},
      {"settings_section_network.rml", "settings.network.media_relay"},
      {"settings_section_network.rml", "settings.network.dht"},
      {"settings_section_network.rml", "settings.network.prefer_contacts"},
      {"settings_section_network.rml", "settings.network.trusted_relays"},
      {"settings_section_network.rml", "settings.network.direct_connections"},
      {"settings_section_network.rml", "settings.network.reachable_without_relay"},
      {"settings_section_profile.rml", "settings.stay_connected"},
      {"settings_section_integrations.rml", "settings.help.topic.mcp"},
      {"settings_section_about.rml", "settings.about.advanced.label"},
  };
  for (const auto& item : expected) {
    const std::string rml = ReadAsset(std::string("views/") + item.view);
    const std::string click = std::string("data-event-click=\"ask_setting_help('") + item.key + "')\"";
    EXPECT_NE(rml.find(click), std::string::npos) << item.view << " " << item.key;
    EXPECT_FALSE(SettingHelpQuestion(item.key).empty()) << item.key;
  }
  // Send diagnostics keeps its own "?" (its question names the support article).
  EXPECT_NE(ReadAsset("views/settings_section_security.rml").find("open_diagnostics_help()"), std::string::npos);
}

TEST_F(SettingsHelpQuestionTest, TheQuestionMarkIsABigEnoughTarget) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t button = css.find(".settings-q-btn {");
  ASSERT_NE(button, std::string::npos);
  EXPECT_NE(css.find("width: 44dp;", button), std::string::npos);
  EXPECT_NE(css.find("height: 44dp;", button), std::string::npos);
  const size_t icon = css.find(".settings-q-icon {");
  ASSERT_NE(icon, std::string::npos);
  EXPECT_NE(css.find("width: 20dp;", icon), std::string::npos);
}

// Phones: the "?" opens the ask-AI panel with the question asked; PP 客服 is not messaged.
TEST_F(SettingsHelpQuestionTest, OnAPhoneTheAiAnswersAndSupportIsNotMessaged) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  std::vector<std::string> to_ai;
  std::vector<std::string> to_support;
  SettingsCommands commands;
  commands.ask_ai_once = [&to_ai](const std::string& question) {
    to_ai.push_back(question);
    return true;
  };
  commands.ask_support = [&to_support](const std::string& question) -> Roe<void> {
    to_support.push_back(question);
    return {};
  };
  SettingsController settings;
  settings.BindCommands(commands);
  settings.OnAskSettingHelp("settings.security.crash_reports.label");
  ASSERT_EQ(to_ai.size(), 1u);
  EXPECT_EQ(to_ai[0], "「发送崩溃报告」是什么意思？");
  EXPECT_TRUE(to_support.empty());
}

// The desktop has no panel (ask_ai_once says no): the question goes to PP 客服 as before.
TEST_F(SettingsHelpQuestionTest, WithoutThePanelSupportIsAsked) {
  std::vector<std::string> to_support;
  SettingsCommands commands;
  commands.ask_ai_once = [](const std::string&) { return false; };
  commands.ask_support = [&to_support](const std::string& question) -> Roe<void> {
    to_support.push_back(question);
    return {};
  };
  SettingsController settings;
  settings.BindCommands(commands);
  settings.OnAskSettingHelp("settings.security.crash_reports.label");
  ASSERT_EQ(to_support.size(), 1u);
  EXPECT_EQ(to_support[0], "What does “Send crash reports” do?");
}

} // namespace
} // namespace pbr
