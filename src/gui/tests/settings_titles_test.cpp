// Settings titles (#155, phase 1 of DEV/PLAN-settings-reorg.md): every page has one title, centred in its bar; Home's
// bar says "Settings" and there is no large heading under it on phones.
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
  const size_t own_section =
      rml.rfind("<div class=\"settings-section stack storage-section\" data-if=\"tester\">", library);
  ASSERT_NE(own_section, std::string::npos);
  const size_t grouped_close =
      rml.rfind("</div>\n      <div class=\"settings-section stack storage-section\" data-if=\"tester\">", library);
  EXPECT_NE(grouped_close, std::string::npos) << "the grouped section closes before the library section opens";
}

TEST(SettingsTitlesTest, PhoneHomeIsTextRowsWithAVersionLineAndTheCardCaption) {
  // Follows logs/mockups/settings-reorg-1110.html: no icon tiles on the entry rows, the card caption under the name, the app
  // version at the end (not tappable here: the five taps stay on the help page's version row).
  const std::string rml = ReadAsset("views/settings.rml");
  const size_t phone = rml.find("<div class=\"settings-phone-only me-home\">");
  ASSERT_NE(phone, std::string::npos);
  const size_t phone_end = rml.find("settings-desktop-only", phone);
  ASSERT_NE(phone_end, std::string::npos);
  const std::string home = rml.substr(phone, phone_end - phone);
  EXPECT_EQ(home.find("group-icon"), std::string::npos);
  EXPECT_NE(home.find("<p class=\"me-card-meta\">{{i18n:settings.card.caption}}</p>"), std::string::npos);
  EXPECT_NE(home.find("<p class=\"group-foot me-home-ver\">{{app_name}} {{app_version}}</p>"), std::string::npos);
  EXPECT_EQ(home.find("tap_version"), std::string::npos);
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(css.find(".shell-layer-compact .me-home .group-card,\n.shell-layer-compact .settings-mock .group-card {\n"
                     "  margin: 24dp 16dp 0 16dp;\n  border-radius: 12dp;"),
            std::string::npos);
  EXPECT_NE(css.find(".shell-layer-compact .settings-mock .group-row-body {\n  min-height: 52dp;"), std::string::npos);
}

TEST(SettingsTitlesTest, TheMeCardHasThreeButtonsCopyShareAndQr) {
  // At the right end of the card's single row, in this order, identical buttons, each with its event and short label.
  const std::string rml = ReadAsset("views/settings.rml");
  const size_t card = rml.find("<div class=\"me-card-row\">");
  const size_t row = rml.find("<div class=\"me-card-acts row\">");
  ASSERT_NE(card, std::string::npos);
  ASSERT_NE(row, std::string::npos);
  const size_t end = rml.find("</div>", row);
  const std::string buttons = rml.substr(row, end - row);
  const size_t copy = buttons.find("data-event-click=\"copy_profile_id()\"");
  const size_t share = buttons.find("data-event-click=\"share_profile()\"");
  const size_t qr = buttons.find("data-event-click=\"show_my_qr()\"");
  ASSERT_NE(copy, std::string::npos);
  ASSERT_NE(share, std::string::npos);
  ASSERT_NE(qr, std::string::npos);
  EXPECT_LT(copy, share);
  EXPECT_LT(share, qr);
  EXPECT_NE(buttons.find("{{i18n:settings.card.copy}}"), std::string::npos);
  EXPECT_NE(buttons.find("{{i18n:settings.card.share}}"), std::string::npos);
  EXPECT_NE(buttons.find("{{i18n:settings.card.qr}}"), std::string::npos);
  size_t count = 0;
  for (size_t at = buttons.find("class=\"me-card-act\""); at != std::string::npos;
       at = buttons.find("class=\"me-card-act\"", at + 1)) {
    ++count;
  }
  EXPECT_EQ(count, 3u);
  // No account id under the name, no separate copy icon, no hidden share-card button: a caption line only.
  const std::string card_markup = rml.substr(card, end - card);
  EXPECT_EQ(card_markup.find("profile_relay_id"), std::string::npos);
  EXPECT_EQ(card_markup.find("relay_id_none"), std::string::npos);
  EXPECT_EQ(card_markup.find("cd-icon-btn"), std::string::npos);
  EXPECT_EQ(card_markup.find("share_profile_card"), std::string::npos);
  EXPECT_NE(card_markup.find("<p class=\"me-card-meta\">{{i18n:settings.card.caption}}</p>"), std::string::npos);
}

TEST(SettingsTitlesTest, TheMeCardTextIsLabelledInBothLanguages) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.card.caption"), "我的名片");
  EXPECT_EQ(Tr("settings.card.copy"), "复制");
  EXPECT_EQ(Tr("settings.card.share"), "分享");
  EXPECT_EQ(Tr("settings.card.qr"), "二维码");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.card.caption"), "My card");
  EXPECT_EQ(Tr("settings.card.copy"), "Copy");
  EXPECT_EQ(Tr("settings.card.share"), "Share");
  EXPECT_EQ(Tr("settings.card.qr"), "QR code");
}

TEST(SettingsTitlesTest, TheProfilePageFollowsTheMockupAndKeepsItsAccountGates) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  EXPECT_NE(rml.find("settings-section--grouped settings-mock stack"), std::string::npos);
  // The phone's blocks have no Copy row any more (the Me card has Copy, Share and QR); the desktop's own button row
  // keeps its Copy, Share and QR buttons.
  const size_t desktop = rml.find("<div class=\"me-profile-actions row\">");
  ASSERT_NE(desktop, std::string::npos);
  const std::string phone_part = rml.substr(0, desktop);
  EXPECT_EQ(phone_part.find("copy_profile_id()"), std::string::npos);
  EXPECT_EQ(phone_part.find("share_profile()"), std::string::npos);
  EXPECT_EQ(phone_part.find("toggle_profile_qr()"), std::string::npos);
  EXPECT_EQ(phone_part.find("profile_qr_svg_rml"), std::string::npos) << "no inline QR on the phone page";
  const size_t d_copy = rml.find("copy_profile_id()", desktop);
  const size_t d_share = rml.find("share_profile()", d_copy);
  const size_t d_qr = rml.find("toggle_profile_qr()", d_share);
  ASSERT_NE(d_copy, std::string::npos);
  EXPECT_LT(d_copy, d_share);
  EXPECT_LT(d_share, d_qr);
  EXPECT_NE(rml.find("settings-desktop-only", d_qr - 300), std::string::npos);
  EXPECT_EQ(rml.find("group-id-btn"), std::string::npos);
  EXPECT_EQ(rml.find("settings.profile.id_foot"), std::string::npos);
  EXPECT_EQ(rml.find("settings.profile.id_help"), std::string::npos);
  // The account group keeps today's gates: status for everyone, expiry and auto-renew behind the advanced flag,
  // the register button behind its own binding; nothing was un-gated.
  EXPECT_NE(rml.find("data-if=\"advanced && profile_registration_expires != ''\""), std::string::npos);
  EXPECT_NE(rml.find("<div class=\"settings-toggle-row row group-row\" data-if=\"advanced\">"), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"profile_show_register\""), std::string::npos);
  // Not here any more / not shown: the AI credential row, delete account, switch account.
  EXPECT_EQ(rml.find("rotate_brief_llm_key"), std::string::npos);
  EXPECT_EQ(rml.find("delete_account"), std::string::npos);
  EXPECT_EQ(rml.find("switch_account"), std::string::npos);
}

TEST(SettingsTitlesTest, TheProfilePageIsCalledMyProfile) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.profile.title"), "我的资料");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.profile.title"), "My profile");
}

TEST(SettingsTitlesTest, PrivacyFollowsTheMockupOrderAndWords) {
  const std::string rml = ReadAsset("views/settings_section_privacy.rml");
  const size_t calls = rml.find("on_choose_call_invite_policy()");
  const size_t invites = rml.find("on_choose_group_invite_policy()");
  const size_t blocked = rml.find("open_blocked_people()");
  ASSERT_NE(calls, std::string::npos);
  EXPECT_LT(calls, invites) << "who can call me, then who can add me to groups";
  EXPECT_LT(invites, blocked) << "then blocked people";
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.security.group_invites"), "谁可以拉我进群");
  EXPECT_EQ(Tr("settings.profile.peer_id_copied"), "账号 ID 已复制") << "names what the user copied";
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.security.group_invites"), "Who can add me to groups");
  EXPECT_EQ(Tr("settings.profile.peer_id_copied"), "Account ID copied");
}

TEST(SettingsTitlesTest, HomeTitleIsSettings) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.title"), "Settings");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.title"), "设置");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace pbr
