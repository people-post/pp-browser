// The Chats page's "save your recovery phrase" card: when it shows (pure rule) and that the view offers both actions.
#include "foundation/i18n/LocalizationService.h"
#include "gui/RecoveryReminder.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

RecoveryReminderInput Ready() {
  RecoveryReminderInput in;
  in.onboarding_completed = true;
  in.can_view_phrase = true;
  in.now_ms = 1000000000000;
  return in;
}

TEST(RecoveryReminderTest, ShowsForANewAccountThatNeverOpenedThePhrase) {
  EXPECT_TRUE(ShouldShowRecoveryReminder(Ready()));
}

TEST(RecoveryReminderTest, NeverOverFirstRunSheetsOrOnADeviceWithoutAPhrase) {
  RecoveryReminderInput in = Ready();
  in.onboarding_completed = false;
  EXPECT_FALSE(ShouldShowRecoveryReminder(in));
  in = Ready();
  in.can_view_phrase = false;
  EXPECT_FALSE(ShouldShowRecoveryReminder(in));
}

TEST(RecoveryReminderTest, GoneOnceThePhraseWasOpened) {
  RecoveryReminderInput in = Ready();
  in.phrase_seen = true;
  EXPECT_FALSE(ShouldShowRecoveryReminder(in));
}

TEST(RecoveryReminderTest, LaterHidesItThenItReturnsOnceAfterSevenDays) {
  RecoveryReminderInput in = Ready();
  in.dismissals = 1;
  in.dismissed_ms = in.now_ms - 1000;
  EXPECT_FALSE(ShouldShowRecoveryReminder(in));
  in.dismissed_ms = in.now_ms - kRecoveryReminderRepeatMs + 1;
  EXPECT_FALSE(ShouldShowRecoveryReminder(in));
  in.dismissed_ms = in.now_ms - kRecoveryReminderRepeatMs;
  EXPECT_TRUE(ShouldShowRecoveryReminder(in));
  // The second "Later" is the last.
  in.dismissals = 2;
  in.dismissed_ms = in.now_ms - 100 * kRecoveryReminderRepeatMs;
  EXPECT_FALSE(ShouldShowRecoveryReminder(in));
}

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(RecoveryReminderTest, TheCardOffersBothActionsOnlyOnAnUnfilteredList) {
  const std::string rml = ReadAsset("views/sidebar.rml");
  // Its own card (the removed-device notice, DeviceRemovedNotice, may sit above it).
  const size_t block = rml.find("class=\"sidebar-tip\" data-if=\"recovery_reminder");
  ASSERT_NE(block, std::string::npos);
  const std::string tag = rml.substr(rml.rfind('<', block), rml.find('>', block) - rml.rfind('<', block));
  EXPECT_NE(tag.find("recovery_reminder"), std::string::npos);
  EXPECT_NE(tag.find("!showing_archived"), std::string::npos);
  const std::string inner = rml.substr(block, rml.find("</div>", rml.find("recovery_reminder_later()", block)) - block);
  EXPECT_NE(inner.find("recovery_reminder_open()"), std::string::npos);
  EXPECT_NE(inner.find("recovery_reminder_later()"), std::string::npos);
  EXPECT_NE(inner.find("{{i18n:recovery_reminder.body}}"), std::string::npos);
}

TEST(RecoveryReminderTest, TheLinesAreTranslated) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("recovery_reminder.body"), "把恢复短语抄下来放好。手机丢了或换了，靠它才能找回账号。");
  EXPECT_EQ(Tr("recovery_reminder.now"), "现在去抄");
  EXPECT_EQ(Tr("recovery_reminder.later"), "以后再说");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
