// Settings titles (#155, phase 1 of DEV/PLAN-settings-reorg.md): every page has one title, centred in its bar; Home's
// bar says "Me" and there is no large heading under it on phones.
#include "foundation/i18n/LocalizationService.h"

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

} // namespace

TEST(SettingsTitlesTest, HomeAndPagesHaveOneTitleEachInTheBar) {
  const std::string rml = ReadAsset("views/settings.rml");
  EXPECT_EQ(rml.find("class=\"large-title\""), std::string::npos) << "no big heading under Home's bar";
  EXPECT_EQ(rml.find("page-large-title"), std::string::npos) << "no big heading inside a page";
  EXPECT_NE(rml.find("<p class=\"pane-nav-title\">{{i18n:settings.title}}</p>"), std::string::npos);
  EXPECT_NE(rml.find("<p class=\"settings-header-title\">{{selected_title}}</p>"), std::string::npos);
  EXPECT_NE(rml.find("<p class=\"settings-header-title\">{{security_page_title}}</p>"), std::string::npos);
  // The desktop list keeps its own heading (the phone hides .settings-header); the detail pane's title is in its header.
  EXPECT_NE(rml.find("<h2 class=\"heading-2\">{{i18n:nav.me}}</h2>"), std::string::npos);
  EXPECT_NE(ReadAsset("views/settings_detail.rml").find("<p class=\"settings-header-title\">{{selected_title}}</p>"),
            std::string::npos);
}

TEST(SettingsTitlesTest, TheBarTitleIsNotHiddenAnyMore) {
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_EQ(css.find(".nav-bar-host .settings-header-title"), std::string::npos);
  EXPECT_NE(css.find(".shell-layer-compact .settings-header-title {"), std::string::npos) << "the phone positions it";
  // The desktop rule is the plain one; nothing outside the compact layer hides the title.
  const size_t plain = css.find("\n.settings-header-title {");
  ASSERT_NE(plain, std::string::npos);
  EXPECT_LT(css.find("}", plain), css.find("display: none", plain));
}

TEST(SettingsTitlesTest, PhoneBarTitlesAreBoldInAFamilyThatHasABoldFace) {
  // The default family (LatoLatin) is loaded in one weight, so `bold` alone changes nothing in it; Inter has four
  // weights, so the phone's bar titles name it (Application.cpp loads it) and ask for bold.
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t rule =
      css.find(".shell-layer-compact .settings-header-title,\n.shell-layer-compact .settings-panel .pane-nav-title {");
  ASSERT_NE(rule, std::string::npos);
  const std::string body = css.substr(rule, css.find('}', rule) - rule);
  EXPECT_NE(body.find("font-family: Inter;"), std::string::npos);
  EXPECT_NE(body.find("font-weight: bold;"), std::string::npos);
}

TEST(SettingsTitlesTest, PhoneProfileQrIsCentredWithSpaceBelow) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t rule = css.find(".shell-layer-compact .me-invite-qr {");
  ASSERT_NE(rule, std::string::npos);
  const std::string body = css.substr(rule, css.find('}', rule) - rule);
  EXPECT_NE(body.find("display: flex;"), std::string::npos) << "align-items only works on a flex box";
  EXPECT_NE(body.find("align-items: center;"), std::string::npos);
  EXPECT_NE(body.find("margin: 20dp 16dp 20dp 16dp;"), std::string::npos);
}

TEST(SettingsTitlesTest, TheTesterPagesStorageLibraryIsNotInsideTheGroupedSection) {
  // The grouped section strips its side padding and restyles every .btn inside it; the library list was written for a
  // plain .settings-section (16dp sides, ordinary buttons), so it must sit in its own one.
  const std::string rml = ReadAsset("views/settings_section_tester.rml");
  const size_t library = rml.find("{{i18n:settings.storage.library.title}}");
  ASSERT_NE(library, std::string::npos);
  const size_t own_section = rml.rfind("<div class=\"settings-section stack storage-section\" data-if=\"advanced\">", library);
  ASSERT_NE(own_section, std::string::npos);
  const size_t grouped_close = rml.rfind("</div>\n      <div class=\"settings-section stack storage-section\" data-if=\"advanced\">", library);
  EXPECT_NE(grouped_close, std::string::npos) << "the grouped section closes before the library section opens";
}

TEST(SettingsTitlesTest, HomeTitleIsMe) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.title"), "Me");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.title"), "我");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace pbr
