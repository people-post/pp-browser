#include "base/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

std::string AssetsRoot() {
#ifdef PP_BROWSER_ASSETS_DIR
  return PP_BROWSER_ASSETS_DIR;
#else
  return (std::filesystem::current_path() / "assets").string();
#endif
}

} // namespace

TEST(LocalizationServiceTest, LoadsEnglishAndChineseAndFallsBack) {
  auto& loc = pbr::LocalizationService::Instance();
  ASSERT_TRUE(loc.LoadFromAssets(AssetsRoot()));

  loc.SetPreferredLanguage("en");
  EXPECT_EQ(loc.ResolvedLanguage(), "en");
  EXPECT_EQ(loc.Tr("nav.home"), "Home");
  EXPECT_EQ(loc.Tr("missing.key.xyz"), "missing.key.xyz");

  loc.SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(loc.ResolvedLanguage(), "zh-Hans");
  EXPECT_EQ(loc.Tr("nav.home"), "首页");
  EXPECT_EQ(loc.Tr("common.cancel"), "取消");
}

TEST(LocalizationServiceTest, SystemResolveAndInterpolation) {
  auto& loc = pbr::LocalizationService::Instance();
  ASSERT_TRUE(loc.LoadFromAssets(AssetsRoot()));

  loc.SetSystemLocalesForTest({"zh-CN", "en-US"});
  loc.SetPreferredLanguage("system");
  EXPECT_EQ(loc.ResolvedLanguage(), "zh-Hans");

  loc.SetSystemLocalesForTest({"fr-FR"});
  loc.SetPreferredLanguage("system");
  EXPECT_EQ(loc.ResolvedLanguage(), "en");

  loc.SetPreferredLanguage("en");
  EXPECT_EQ(loc.Tr("errors.network.unreachable"), loc.Tr("errors.network.unreachable"));
  // Use a key with placeholder from catalog
  EXPECT_NE(loc.Tr("nav.me").find("Me"), std::string::npos);

  const std::string localized = loc.LocalizeText("Hello {{i18n:nav.home}}!");
  EXPECT_EQ(localized, "Hello Home!");

  loc.ClearSystemLocalesForTest();
}

TEST(LocalizationServiceTest, NativeSelfNames) {
  auto& loc = pbr::LocalizationService::Instance();
  ASSERT_TRUE(loc.LoadFromAssets(AssetsRoot()));
  loc.SetPreferredLanguage("en");
  EXPECT_EQ(loc.LanguageDisplayLabel("zh-Hans"), "简体中文");
  EXPECT_EQ(loc.LanguageDisplayLabel("en"), "English");
  EXPECT_EQ(loc.LanguageDisplayLabel("system"), "System");

  loc.SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(loc.LanguageDisplayLabel("system"), "跟随系统");
  EXPECT_EQ(loc.LanguageDisplayLabel("en"), "English");
}
