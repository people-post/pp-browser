// People on the app's default PIN are never asked to type it. They see one line ("you are on the system default PIN")
// with a "?" that tells the digits (Kenneth 2026-11-15). The digits live only in kDefaultProfilePin.
#include "foundation/crypto/PinDefaults.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/DefaultPinNote.h"
#include "gui/SettingsController.h"
#include "gui/shell/ShellFeedback.h"
#include "gui/shell/ShellFeedbackPorts.h"
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

std::string ReadSource(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

class DefaultPinNoteTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  }
  void SetUp() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(DefaultPinNoteTest, TheNoteFillsInThePinFromTheConstantInBothLanguages) {
  for (const char* lang : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(lang);
    const std::string text = DefaultPinNoteText();
    EXPECT_NE(text.find(kDefaultProfilePin), std::string::npos) << lang;
    EXPECT_EQ(text.find("{pin}"), std::string::npos) << lang;
  }
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(DefaultPinNoteText(), std::string("系统默认的 PIN 是 ") + kDefaultProfilePin +
                                      "。建议到 设置 → 安全 → 更改 PIN 换成只有你知道的 PIN。");
  EXPECT_EQ(Tr("settings.security.default_hint"), "你现在用的是系统默认的 PIN");
  EXPECT_EQ(Tr("pin.default_note.device"), "这台设备将使用系统默认的 PIN");
}

// Nothing in a locale file or a view may carry the digits: the constant is the one place.
TEST_F(DefaultPinNoteTest, NoLocaleOrViewStringContainsTheDefaultPinDigits) {
  const std::string digits = kDefaultProfilePin;
  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    EXPECT_EQ(ReadAsset(locale).find(digits), std::string::npos) << locale;
  }
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views")) {
    if (entry.path().extension() == ".rml") {
      EXPECT_EQ(ReadAsset("views/" + entry.path().filename().string()).find(digits), std::string::npos)
          << entry.path().filename();
    }
  }
  EXPECT_EQ(ReadSource("src/gui/shell/ShellLayoutRml.cpp").find(digits), std::string::npos);
}

TEST_F(DefaultPinNoteTest, TheConfirmDialogCarriesTheLineAndItsQuestionMarkOpensTheNoteInPlace) {
  ShellState state;
  ShellFeedback::ShowConfirmWithNote(state, "Copy?", "Message", "line", "note {digits}", [](bool) {});
  EXPECT_TRUE(state.dialog.active);
  EXPECT_TRUE(state.dialog.show_cancel);
  EXPECT_EQ(std::string(state.dialog.note_line.c_str()), "line");
  EXPECT_FALSE(state.dialog.note_open);
  ShellFeedback::ToggleDialogNote(state);
  EXPECT_TRUE(state.dialog.note_open);
  EXPECT_TRUE(state.dialog.active) << "the confirm stays; the note opens inside it";
  ShellFeedback::ToggleDialogNote(state);
  EXPECT_FALSE(state.dialog.note_open);
  // A plain confirm has no line, and a toggle does nothing.
  ShellFeedback::ShowConfirm(state, "Copy?", "Message", [](bool) {});
  EXPECT_TRUE(state.dialog.note_line.empty());
  ShellFeedback::ToggleDialogNote(state);
  EXPECT_FALSE(state.dialog.note_open);
}

TEST_F(DefaultPinNoteTest, TheSecurityQuestionMarkOpensALocalNoteOnlyOnTheDefaultPin) {
  struct Alert {
    std::string title;
    std::string message;
  };
  std::vector<Alert> alerts;
  int asked = 0;
  SettingsCommands commands;
  commands.ask_ai_once = [&asked](const std::string&) {
    ++asked;
    return true;
  };
  commands.ask_support = [&asked](const std::string&) -> Roe<void> {
    ++asked;
    return {};
  };
  ShellFeedbackPorts feedback;
  feedback.show_alert = [&alerts](const std::string& title, const std::string& message, std::function<void()>,
                                  const std::string&) { alerts.push_back({title, message}); };
  SettingsController settings;
  settings.BindCommands(commands);
  settings.BindShellFeedback(feedback);
  SettingsHost& host = settings;
  host.UiState().pin_protection_is_default = false;
  settings.OnShowDefaultPinNote();
  EXPECT_TRUE(alerts.empty()) << "an own PIN has no such note";
  host.UiState().pin_protection_is_default = true;
  settings.OnShowDefaultPinNote();
  ASSERT_EQ(alerts.size(), 1u);
  EXPECT_EQ(alerts[0].title, "System default PIN");
  EXPECT_EQ(alerts[0].message, DefaultPinNoteText());
  EXPECT_NE(alerts[0].message.find(kDefaultProfilePin), std::string::npos);
  EXPECT_EQ(asked, 0) << "a local note: no AI, no support";
}

TEST_F(DefaultPinNoteTest, TheSecurityPageShowsTheLineAndItsQuestionMarkOnlyOnTheDefaultPin) {
  const std::string rml = ReadAsset("views/settings_section_security.rml");
  const size_t head = rml.find("data-if=\"pin_protection_is_default\"");
  ASSERT_NE(head, std::string::npos);
  const size_t line = rml.find("{{i18n:settings.security.default_hint}}", head);
  const size_t button = rml.find("data-event-click=\"show_default_pin_note()\"", head);
  ASSERT_NE(line, std::string::npos);
  ASSERT_NE(button, std::string::npos);
  EXPECT_LT(line, button) << "the \"?\" comes right after the text";
  EXPECT_NE(rml.find("settings-q-btn", head), std::string::npos) << "the shared 44dp \"?\" button";
}

TEST_F(DefaultPinNoteTest, TheGateAndDialogMarkupHaveTheLineAndTheQuestionMark) {
  const std::string layout = ReadSource("src/gui/shell/ShellLayoutRml.cpp");
  EXPECT_NE(layout.find("pin_gate_default_note_line != ''"), std::string::npos);
  EXPECT_NE(layout.find("pin_gate_note_toggle()"), std::string::npos);
  EXPECT_NE(layout.find("dialog_note_line != ''"), std::string::npos);
  EXPECT_NE(layout.find("dialog_toggle_note()"), std::string::npos);
}

} // namespace
} // namespace pbr
