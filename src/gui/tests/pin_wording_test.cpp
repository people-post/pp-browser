// The person hears of no PIN or their own PIN, never of a "default PIN": no wording says so and no screen shows the
// digits of the constant that stands behind "no PIN" (kDefaultProfilePin, mechanism unchanged).
#include "foundation/crypto/PinDefaults.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/shell/ShellFeedback.h"
#include "gui/shell/ShellFeedbackPorts.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <algorithm>
#include <cctype>
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

class PinWordingTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
  void SetUp() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

// Every locale value that talks about a PIN keeps to the no-PIN / your-PIN vocabulary.
TEST_F(PinWordingTest, NoPinLocaleValueMentionsADefaultOrSystemOrCustomPinOrCarriesAPlaceholder) {
  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    std::istringstream lines(ReadAsset(locale));
    std::string line;
    while (std::getline(lines, line)) {
      std::string lower = line;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
      if (lower.find("pin") == std::string::npos) {
        continue;
      }
      for (const char* banned : {"default pin", "custom pin", "system default", "{pin}", "默认 pin", "默认pin",
                                 "系统默认", "自定义 pin", "自定义pin", "app default"}) {
        EXPECT_EQ(lower.find(banned), std::string::npos) << locale << ": " << line;
      }
    }
  }
}

// Nothing in a locale file or a view may carry the digits: the constant is the one place.
TEST_F(PinWordingTest, NoLocaleOrViewStringContainsTheDefaultPinDigits) {
  const std::string digits = kDefaultProfilePin;
  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    EXPECT_EQ(ReadAsset(locale).find(digits), std::string::npos) << locale;
  }
  for (const auto& entry :
       std::filesystem::directory_iterator(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views")) {
    if (entry.path().extension() == ".rml") {
      EXPECT_EQ(ReadAsset("views/" + entry.path().filename().string()).find(digits), std::string::npos)
          << entry.path().filename();
    }
  }
  EXPECT_EQ(ReadSource("src/gui/shell/ShellLayoutRml.cpp").find(digits), std::string::npos);
}

// The PIN page: rows, status and captions per state and language. Which rows and captions show in which state is
// pinned by settings_row_checklist (the data-if chains); here the words.
TEST_F(PinWordingTest, ThePinPageWordsForNoPinAndOwnPinInBothLanguages) {
  struct Words {
    const char* key;
    const char* en;
    const char* zh;
  };
  const Words words[] = {
      {"settings.security.set_pin", "Set a PIN", "设置 PIN 码"},
      {"settings.security.change_pin", "Change PIN", "更改 PIN 码"},
      {"settings.security.remove_pin", "Remove PIN", "取消 PIN 码"},
      {"settings.security.pin_status.app_default", "No PIN", "未设 PIN 码"},
      {"settings.security.pin_status.custom", "PIN is on", "已设 PIN 码"},
      {"pin.verify_remove_message", "Enter your current PIN to remove it", "输入当前 PIN 码，取消它"},
      {"settings.security.pin_removed", "PIN removed. Anyone who has this device can open PP.",
       "已取消 PIN 码。拿到这台设备的人都能打开 PP。"},
      {"settings.security.pin_page.note", "Without a PIN, anyone who has this device can open PP.",
       "没有 PIN 码时，拿到这台设备的人都能打开 PP。"},
      {"settings.security.pin_page.note_calls",
       "With a PIN, incoming calls show as a notification instead of ringing full screen.",
       "设了 PIN 码后，来电以通知提醒，不再全屏响铃。"},
      {"settings.security.biometric.needs_own_pin", "Set a PIN first", "先设一个 PIN 码"},
      {"settings.security.recovery.set_pin_first", "Set a PIN first to see your recovery phrase",
       "先设一个 PIN 码，才能查看恢复短语"},
      {"settings.help.topic.pin", "PIN", "PIN 码"},
  };
  for (const Words& w : words) {
    LocalizationService::Instance().SetPreferredLanguage("en");
    EXPECT_EQ(Tr(w.key), w.en) << w.key;
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    EXPECT_EQ(Tr(w.key), w.zh) << w.key;
  }
  // The old default-PIN explanations and the onboarding toast are gone from both catalogs.
  for (const char* gone :
       {"settings.security.default_hint", "settings.security.default_hint.title", "settings.security.default_hint.note",
        "settings.security.set_custom_pin", "pin.default_provisioned_toast", "pin.default_note.device"}) {
    for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
      EXPECT_EQ(ReadAsset(locale).find(std::string("\"") + gone + "\""), std::string::npos) << locale << " " << gone;
    }
  }
  const std::string page = ReadAsset("views/settings_security_pin.rml");
  for (const char* key : {"settings.security.set_pin", "settings.security.change_pin", "settings.security.remove_pin",
                          "settings.security.pin_page.note", "settings.security.pin_page.note_calls"}) {
    EXPECT_NE(page.find(std::string("{{i18n:") + key + "}}"), std::string::npos) << key;
  }
  EXPECT_EQ(page.find("pin_page_note"), std::string::npos) << "no binding feeds a PIN into the page";
}

TEST_F(PinWordingTest, TheConfirmDialogCarriesTheLineAndItsQuestionMarkOpensTheNoteInPlace) {
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

// 2026-11-17: the Security page's PIN row carries the "?" right after its label (it asks PP Support like the other
// settings rows; the "?" stops the tap so the row does not also open the page).
TEST_F(PinWordingTest, TheSecurityPinRowHasASupportQuestionMarkAfterItsLabelAndNoCaption) {
  const std::string rml = ReadAsset("views/settings_section_security.rml");
  EXPECT_EQ(rml.find("settings.security.default_hint"), std::string::npos);
  EXPECT_EQ(rml.find("settings.security.pin_caption"), std::string::npos);
  for (const char* label : {"{{i18n:settings.security.pin_row}}", "{{i18n:settings.security.protection_title}}"}) {
    const size_t line = rml.find(label);
    ASSERT_NE(line, std::string::npos) << label;
    const size_t button = rml.find("data-event-click=\"ask_setting_help('settings.help.topic.pin')\"", line);
    ASSERT_NE(button, std::string::npos) << label;
    EXPECT_LT(button, rml.find("group-row-value", line)) << "the \"?\" comes right after the label, before the value";
    EXPECT_NE(rml.find("settings-q-btn", line), std::string::npos) << "the shared 44dp \"?\" button";
  }
  EXPECT_EQ(ReadAsset("views/settings_security_pin.rml").find("show_default_pin_note()"), std::string::npos);
}

TEST_F(PinWordingTest, TheGateHasNoDefaultPinLineAndTheDialogKeepsItsGenericNote) {
  const std::string layout = ReadSource("src/gui/shell/ShellLayoutRml.cpp");
  EXPECT_EQ(layout.find("pin_gate_default_note"), std::string::npos);
  EXPECT_EQ(layout.find("pin_gate_note_toggle()"), std::string::npos);
  EXPECT_NE(layout.find("dialog_note_line != ''"), std::string::npos);
  EXPECT_NE(layout.find("dialog_toggle_note()"), std::string::npos);
}

} // namespace
} // namespace pbr
