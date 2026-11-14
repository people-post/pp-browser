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
    LocalizationService::Instance().SetPreferredLanguage("en"); // not the machine's language
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(SettingsHelpQuestionTest, TheQuestionNamesTheSettingInTheUiLanguage) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(SettingHelpQuestion("settings.security.crash_reports.label"), "「闪退时自动发送报告」是什么意思？");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(SettingHelpQuestion("settings.security.crash_reports.label"),
            "What does “Send a report if PP crashes” do?");
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
      {"settings_section_help.rml", "settings.security.crash_reports.label"},
      {"settings_section_security.rml", "settings.security.recovery"},
      {"settings_section_devices.rml", "settings.security.transfer_row"},
      {"settings_section_devices.rml", "settings.help.topic.account_copy"},
      {"settings_section_advanced.rml", "settings.network.help_network"},
      {"settings_section_advanced.rml", "settings.network.circuit_relay"},
      {"settings_section_advanced.rml", "settings.network.media_relay"},
      {"settings_section_advanced.rml", "settings.network.dht"},
      {"settings_section_advanced.rml", "settings.network.prefer_contacts"},
      {"settings_section_advanced.rml", "settings.network.trusted_relays"},
      {"settings_section_advanced.rml", "settings.network.direct_connections"},
      {"settings_section_advanced.rml", "settings.network.reachable_without_relay"},
      {"settings_section_notifications.rml", "settings.stay_connected"},
      {"settings_section_advanced.rml", "settings.help.topic.mcp"},
  };
  for (const auto& item : expected) {
    const std::string rml = ReadAsset(std::string("views/") + item.view);
    const std::string click = std::string("data-event-click=\"ask_setting_help('") + item.key + "')\"";
    EXPECT_NE(rml.find(click), std::string::npos) << item.view << " " << item.key;
    EXPECT_FALSE(SettingHelpQuestion(item.key).empty()) << item.key;
  }
  // Send diagnostics keeps its own "?" (its question names the support article).
  EXPECT_NE(ReadAsset("views/settings_section_help.rml").find("open_diagnostics_help()"), std::string::npos);
}

// Phones: the crash-report switch and Send problem logs sit on the Help page, in one "Help improve PP" card, each
// with its "?" and a line under it; the desktop has its own copies of the two (hidden on phones) on the same page.
TEST_F(SettingsHelpQuestionTest, PhonesShowCrashReportsAndProblemLogsInHelpImprovePp) {
  const std::string about = ReadAsset("views/settings_section_help.rml");
  const size_t card = about.find("{{i18n:settings.about.improve.title}}");
  ASSERT_NE(card, std::string::npos);
  // The title and the card are phone-only, and every piece follows the title.
  EXPECT_NE(about.find("<p class=\"text group-head settings-phone-only\">{{i18n:settings.about.improve.title}}"),
            std::string::npos);
  EXPECT_NE(about.find("<div class=\"group-card settings-phone-only\">", card), std::string::npos);
  for (const char* piece :
       {"ask_setting_help('settings.security.crash_reports.label')", "toggle_crash_reports()",
        "{{i18n:settings.security.crash_reports.help}}", "send_diagnostics()", "open_diagnostics_help()",
        "{{i18n:settings.security.diagnostics.help}}", "copy_diagnostics_id()"}) {
    EXPECT_NE(about.find(piece, card), std::string::npos) << piece;
  }
  EXPECT_NE(about.find("<div class=\"group-card settings-desktop-only\">"), std::string::npos);
  EXPECT_NE(about.find("group-actions settings-desktop-only\" data-if=\"show_diagnostics_send\""), std::string::npos);
  EXPECT_EQ(Tr("settings.about.improve.title"), "Help improve PP");
  EXPECT_EQ(Tr("settings.security.diagnostics.send"), "Send diagnostics");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.about.improve.title"), "帮我们改进 PP");
  EXPECT_EQ(Tr("settings.security.crash_reports.label"), "闪退时自动发送报告");
  EXPECT_EQ(Tr("settings.security.crash_reports.help"), "只在闪退时发送，不含聊天内容。");
  EXPECT_EQ(Tr("settings.security.diagnostics.send"), "发送诊断");
  EXPECT_EQ(Tr("settings.security.diagnostics.help"), "遇到问题、客服请你发时用。");
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
  EXPECT_EQ(to_ai[0], "「闪退时自动发送报告」是什么意思？");
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
  EXPECT_EQ(to_support[0], "What does “Send a report if PP crashes” do?");
}

// show_setting_note('<title key>','<text key>'): a "?" that explains a setting from the app's own text. It opens the
// shell's alert with that title and text and sends nothing anywhere (no AI question, no support message).
TEST_F(SettingsHelpQuestionTest, AShownNoteOpensAnAlertWithItsTitleAndTextAndAsksNobody) {
  struct Alert {
    std::string title;
    std::string message;
    std::string ok;
  };
  std::vector<Alert> alerts;
  int sent = 0;
  SettingsCommands commands;
  commands.ask_ai_once = [&sent](const std::string&) {
    ++sent;
    return true;
  };
  commands.ask_support = [&sent](const std::string&) -> Roe<void> {
    ++sent;
    return {};
  };
  ShellFeedbackPorts feedback;
  feedback.show_alert = [&alerts](const std::string& title, const std::string& message, std::function<void()>,
                                  const std::string& ok) { alerts.push_back({title, message, ok}); };
  feedback.show_toast = [&sent](const std::string&, ToastDuration) { ++sent; };
  SettingsController settings;
  settings.BindCommands(commands);
  settings.BindShellFeedback(feedback);
  settings.OnShowSettingNote("settings.auto_renew.short", "settings.auto_renew.note");
  ASSERT_EQ(alerts.size(), 1u);
  EXPECT_EQ(alerts[0].title, "Auto-update");
  EXPECT_EQ(alerts[0].message, Tr("settings.auto_renew.note"));
  EXPECT_EQ(alerts[0].ok, "OK");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  settings.OnShowSettingNote("settings.auto_renew.short", "settings.auto_renew.note");
  ASSERT_EQ(alerts.size(), 2u);
  EXPECT_EQ(alerts[1].title, "自动更新");
  EXPECT_EQ(alerts[1].message,
            "打开后，名片快到期时会自动更新，一般不用手动操作。关闭后，到期前需要你自己点“立即更新”。");
  // Not a settings string, or a key without text: nothing opens.
  settings.OnShowSettingNote("chat.menu.block", "settings.auto_renew.note");
  settings.OnShowSettingNote("settings.auto_renew.short", "settings.no_such_key");
  settings.OnShowSettingNote("", "");
  EXPECT_EQ(alerts.size(), 2u);
  EXPECT_EQ(sent, 0) << "a note never asks the AI, never messages support, never toasts";
}

} // namespace
} // namespace pbr
