// Me → Notifications: the "set Banner Style to Persistent" row shows only for a banner that goes away by itself.
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

TEST(SettingsBannerTipTest, OnlyATemporaryBannerShowsTheRow) {
  EXPECT_TRUE(ShowCallBannerTip(BannerStyle::Temporary));
  EXPECT_FALSE(ShowCallBannerTip(BannerStyle::Persistent));
  EXPECT_FALSE(ShowCallBannerTip(BannerStyle::None));
  EXPECT_FALSE(ShowCallBannerTip(BannerStyle::Unknown));
}

TEST(SettingsBannerTipTest, TheRowIsGatedByTheBindingAndOpensTheSystemPage) {
  const std::string view = ReadAsset("views/settings_section_notifications.rml");
  const size_t gate = view.find("data-if=\"call_banner_tip_available\"");
  ASSERT_NE(gate, std::string::npos);
  const size_t button = view.find("open_notification_settings()", gate);
  ASSERT_NE(button, std::string::npos);
  EXPECT_LT(button - gate, 300u); // inside that card, not a later one
  EXPECT_NE(view.find("settings.notifications.banner_tip.action", gate), std::string::npos);
  // One row like the others: a title, the sentence as its small line, "go to settings" and a chevron on the right.
  EXPECT_NE(view.find("settings.notifications.banner_tip.title", gate), std::string::npos);
  EXPECT_NE(view.find("class=\"group-row-sub\">{{i18n:settings.notifications.banner_tip}}", gate), std::string::npos);
  EXPECT_NE(view.find("group-chevron", gate), std::string::npos);
  EXPECT_EQ(view.find("btn btn-secondary", gate), std::string::npos) << "no pill button any more";
}

TEST(SettingsBannerTipTest, TheTextExistsInBothLanguages) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    EXPECT_NE(Tr("settings.notifications.banner_tip"), "settings.notifications.banner_tip") << language;
    EXPECT_NE(Tr("settings.notifications.banner_tip.action"), "settings.notifications.banner_tip.action") << language;
    EXPECT_NE(Tr("settings.notifications.banner_tip.title"), "settings.notifications.banner_tip.title") << language;
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
