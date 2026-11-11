// Me → Notifications on Android: a row shows only for what the system said is wrong (auto-start excepted), and each row
// leads to its system screen. The decisions are the pure ComputeNotificationTips.
#include "foundation/i18n/LocalizationService.h"
#include "gui/SettingsController.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(NotificationTipsTest, NothingWrongShowsNoRow) {
  const NotificationTips tips = ComputeNotificationTips(NotificationHealth{}, false);
  EXPECT_FALSE(tips.notifications_blocked);
  EXPECT_FALSE(tips.silenced);
  EXPECT_FALSE(tips.battery);
  EXPECT_FALSE(tips.autostart);
  EXPECT_EQ(tips.stay_help, PushGap::None);
}

TEST(NotificationTipsTest, UnknownHealthShowsNothingEvenWithStayConnectedOn) {
  // A health that could not be read is the default one: no claim, whatever the switch says.
  const NotificationTips tips = ComputeNotificationTips(NotificationHealth{}, true);
  EXPECT_FALSE(tips.notifications_blocked);
  EXPECT_FALSE(tips.silenced);
  EXPECT_FALSE(tips.battery);
  EXPECT_FALSE(tips.autostart);
}

TEST(NotificationTipsTest, BlockedNotificationsHideTheSilencedRow) {
  NotificationHealth health;
  health.notifications_blocked = true;
  health.silenced = true;
  const NotificationTips tips = ComputeNotificationTips(health, false);
  EXPECT_TRUE(tips.notifications_blocked);
  EXPECT_FALSE(tips.silenced);
}

TEST(NotificationTipsTest, SilencedRowShowsForTotalSilence) {
  NotificationHealth health;
  health.silenced = true;
  const NotificationTips tips = ComputeNotificationTips(health, false);
  EXPECT_TRUE(tips.silenced);
  EXPECT_FALSE(tips.notifications_blocked);
}

TEST(NotificationTipsTest, BatteryAndAutostartRowsNeedStayConnectedOn) {
  NotificationHealth health;
  health.battery_restricted = true;
  health.autostart_vendor = true;
  const NotificationTips off = ComputeNotificationTips(health, false);
  EXPECT_FALSE(off.battery);
  EXPECT_FALSE(off.autostart);
  const NotificationTips on = ComputeNotificationTips(health, true);
  EXPECT_TRUE(on.battery);
  EXPECT_TRUE(on.autostart);
}

TEST(NotificationTipsTest, AutostartRowNeedsAVendorPhone) {
  NotificationHealth health;
  health.battery_restricted = true;
  const NotificationTips tips = ComputeNotificationTips(health, true);
  EXPECT_TRUE(tips.battery);
  EXPECT_FALSE(tips.autostart);
}

TEST(NotificationTipsTest, BatteryRowHidesOnceThePhoneLetsPpRun) {
  NotificationHealth health;
  health.autostart_vendor = true;
  const NotificationTips tips = ComputeNotificationTips(health, true);
  EXPECT_FALSE(tips.battery);
  EXPECT_TRUE(tips.autostart); // not detected: it stays while the switch is on
}

TEST(NotificationTipsTest, StayConnectedHelpFollowsThePushGap) {
  NotificationHealth health;
  health.push_gap = PushGap::BuildHasNone;
  EXPECT_EQ(ComputeNotificationTips(health, false).stay_help, PushGap::BuildHasNone);
  health.push_gap = PushGap::NoGoogleServices;
  EXPECT_EQ(ComputeNotificationTips(health, true).stay_help, PushGap::NoGoogleServices);
}

TEST(NotificationTipsViewTest, EachRowIsGatedByItsBindingAndOpensItsScreen) {
  const std::string view = ReadAsset("views/settings_section_notifications.rml");
  const struct {
    const char* gate;
    const char* fix;
    const char* key;
  } rows[] = {{"tip_notifications_blocked", "notifications", "blocked"},
              {"tip_silenced", "silenced", "silenced"},
              {"tip_autostart", "autostart", "autostart"}};
  for (const auto& row : rows) {
    const size_t gate = view.find(std::string("data-if=\"") + row.gate + "\"");
    ASSERT_NE(gate, std::string::npos) << row.gate;
    const size_t button = view.find(std::string("open_notification_fix('") + row.fix + "')", gate);
    ASSERT_NE(button, std::string::npos) << row.fix;
    EXPECT_LT(button - gate, 300u) << "inside that card, not a later one";
    EXPECT_NE(view.find(std::string("settings.notifications.tip.") + row.key + ".body", gate), std::string::npos);
    EXPECT_NE(view.find("group-chevron", gate), std::string::npos);
  }
  EXPECT_NE(view.find("data-if=\"stay_help_build\""), std::string::npos);
  EXPECT_NE(view.find("data-if=\"stay_help_no_google\""), std::string::npos);
}

TEST(NotificationTipsViewTest, TheTextExistsInBothLanguagesAndUsesPlainWords) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  const char* keys[] = {"settings.notifications.tip.blocked.title",   "settings.notifications.tip.blocked.body",
                        "settings.notifications.tip.silenced.title",  "settings.notifications.tip.silenced.body",
                                                "settings.notifications.tip.autostart.title", "settings.notifications.tip.autostart.body",
                        "settings.stay_connected.help.build",         "settings.stay_connected.help.no_google"};
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const char* key : keys) {
      const std::string text = Tr(key);
      EXPECT_NE(text, key) << language << " " << key;
      for (const char* word : {"推送", "前台服务", "白名单"}) {
        EXPECT_EQ(text.find(word), std::string::npos) << language << " " << key << " has " << word;
      }
    }
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
