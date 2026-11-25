#include "foundation/i18n/LocalizationService.h"
#include "common/Localized.h"

#include <gtest/gtest.h>

#include "common/ValueJson.h"
#include "foundation/platform/AssetIO.h"

#include <filesystem>
#include <map>
#include <regex>
#include <set>

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
  EXPECT_EQ(loc.Tr("nav.home"), "AI");
  EXPECT_EQ(loc.Tr("missing.key.xyz"), "missing.key.xyz");

  loc.SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(loc.ResolvedLanguage(), "zh-Hans");
  EXPECT_EQ(loc.Tr("nav.home"), "AI");
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
  EXPECT_NE(loc.Tr("nav.me").find("Settings"), std::string::npos);

  const std::string localized = loc.LocalizeText("Hello {{i18n:nav.home}}!");
  EXPECT_EQ(localized, "Hello AI!");

  loc.ClearSystemLocalesForTest();
}

// Release 1 ships in English: the picker offers en only (System is the UI's own row); the Chinese catalog stays loaded.
TEST(LocalizationServiceTest, OnlyEnglishIsUserSelectableAndAStoredChineseChoiceFollowsTheSystem) {
  auto& loc = pbr::LocalizationService::Instance();
  ASSERT_TRUE(loc.LoadFromAssets(AssetsRoot()));

  const auto selectable = loc.UserSelectableLocales();
  ASSERT_EQ(selectable.size(), 1U);
  EXPECT_EQ(selectable[0].tag, "en");
  bool zh_loaded = false;
  for (const auto& info : loc.AvailableLocales()) {
    zh_loaded = zh_loaded || info.tag == "zh-Hans";
  }
  EXPECT_TRUE(zh_loaded) << "the Chinese catalog stays in the app";

  EXPECT_EQ(pbr::LocalizationService::EffectivePreference("zh-Hans"), "system");
  EXPECT_EQ(pbr::LocalizationService::EffectivePreference("zh_CN"), "system");
  EXPECT_EQ(pbr::LocalizationService::EffectivePreference("en"), "en");
  EXPECT_EQ(pbr::LocalizationService::EffectivePreference("system"), "system");
  EXPECT_EQ(pbr::LocalizationService::EffectivePreference(""), "system");

  // A stored zh-Hans (an earlier build) is applied like System: Chinese on a Chinese system, English otherwise.
  loc.SetSystemLocalesForTest({"zh-CN"});
  loc.Apply({.language = "zh-Hans"});
  EXPECT_EQ(loc.PreferredLanguage(), "system");
  EXPECT_EQ(loc.ResolvedLanguage(), "zh-Hans");
  loc.SetSystemLocalesForTest({"en-US"});
  loc.Apply({.language = "zh-Hans"});
  EXPECT_EQ(loc.ResolvedLanguage(), "en");
  // An explicit English still wins on a Chinese system; System itself is unchanged.
  loc.SetSystemLocalesForTest({"zh-CN"});
  loc.Apply({.language = "en"});
  EXPECT_EQ(loc.ResolvedLanguage(), "en");
  loc.Apply({.language = "system"});
  EXPECT_EQ(loc.ResolvedLanguage(), "zh-Hans");

  loc.ClearSystemLocalesForTest();
  loc.SetPreferredLanguage("en");
}

TEST(LocalizationServiceTest, NativeSelfNames) {
  auto& loc = pbr::LocalizationService::Instance();
  ASSERT_TRUE(loc.LoadFromAssets(AssetsRoot()));
  loc.SetPreferredLanguage("en");
  // Release 1 ships in English: Chinese is not a choice, so a stored one reads as System.
  EXPECT_EQ(loc.LanguageDisplayLabel("zh-Hans"), "System");
  EXPECT_EQ(loc.LanguageDisplayLabel("en"), "English");
  EXPECT_EQ(loc.LanguageDisplayLabel("system"), "System");

  loc.SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(loc.LanguageDisplayLabel("system"), "跟随系统");
  EXPECT_EQ(loc.LanguageDisplayLabel("en"), "English");
}

// Capabilities ask the base's port (common/Localized.h); the loaded catalogs answer it, and the app's aliases
// give a shared key its platform's text (capability-repos CR015).
TEST(LocalizationServiceTest, LoadedCatalogsAnswerTheBasesTranslator) {
  auto& loc = pbr::LocalizationService::Instance();
  ASSERT_TRUE(loc.LoadFromAssets(AssetsRoot()));
  loc.SetPreferredLanguage("en");
  EXPECT_EQ(pbr::Localized("call.status.reconnecting"), "Reconnecting…");

  loc.SetKeyAlias(pbr::kPlatformNetworkHintKey, "hints.network.lan_linux");
  EXPECT_EQ(pbr::Localized(pbr::kPlatformNetworkHintKey),
            "Check both devices are on the same network and UDP is not blocked.");
}

// A translation that drops or renames a {placeholder} shows the raw token or loses the value (the default-PIN note
// must say the PIN in both languages). Every key in both catalogs names the same placeholders.
TEST(LocalizationServiceTest, EnglishAndChineseUseTheSamePlaceholders) {
  const auto load = [](const char* tag) {
    std::string text;
    EXPECT_TRUE(pbr::AssetIO::ReadText(AssetsRoot() + "/locales/" + tag + ".json", text));
    std::map<std::string, std::string> out;
    auto root = pbr::TryParseObject(text);
    EXPECT_TRUE(root.has_value());
    if (root && root->getObject("strings") != nullptr) {
      for (const auto& [key, value] : root->getObject("strings")->fields()) {
        if (auto str = pbr::asString(value)) {
          out[key] = *str;
        }
      }
    }
    return out;
  };
  const auto en = load("en");
  const auto zh = load("zh-Hans");
  ASSERT_FALSE(en.empty());
  const std::regex token("\\{([A-Za-z_][A-Za-z0-9_]*)\\}");
  const auto names = [&](const std::string& text) {
    std::set<std::string> found;
    for (std::sregex_iterator it(text.begin(), text.end(), token), end; it != end; ++it) {
      found.insert((*it)[1].str());
    }
    return found;
  };
  for (const auto& [key, en_text] : en) {
    const auto zh_it = zh.find(key);
    if (zh_it != zh.end()) {
      EXPECT_EQ(names(en_text), names(zh_it->second)) << key;
    }
  }
}
