// The phone "My profile" page, plan A (DEV/PLAN-my-profile-page-a.md): a big centred photo with "Change photo" under
// it, the large name and one grey line, the nickname as a row, one "Account" group, no Copy row. The desktop keeps
// its input and its Copy / Share / QR buttons; the registration, renewal and PIN entries keep their gates.
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

size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

std::string RuleBody(const std::string& css, const std::string& selector) {
  const size_t at = css.find("\n" + selector + " {");
  EXPECT_NE(at, std::string::npos) << selector;
  return at == std::string::npos ? "" : css.substr(at, css.find("\n}", at) - at);
}

class SettingsProfilePhoneTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(SettingsProfilePhoneTest, TheHeadIsThePhotoChangePhotoTheBigNameAndOneGreyLine) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const size_t frame = rml.find("class=\"me-profile-avatar-frame\" data-event-click=\"tap_profile_avatar()\"");
  const size_t badge = rml.find("<button class=\"me-profile-avatar-badge\"");
  const size_t head = rml.find("<div class=\"me-profile-phone-head settings-phone-only\">");
  ASSERT_NE(frame, std::string::npos);
  ASSERT_NE(badge, std::string::npos);
  ASSERT_NE(head, std::string::npos);
  EXPECT_LT(frame, badge);
  EXPECT_LT(badge, head);
  const size_t change = rml.find("<button class=\"me-profile-change\"", head);
  const size_t name = rml.find("<p class=\"me-profile-name\"", head);
  const size_t caption = rml.find("<p class=\"me-profile-caption\">{{i18n:settings.profile.name_caption}}</p>", head);
  ASSERT_NE(change, std::string::npos);
  EXPECT_LT(change, name);
  EXPECT_LT(name, caption);
  EXPECT_NE(rml.find("{{profile_nickname}}", name), std::string::npos);
  // The change button is a 44dp target in blue; the name is 22dp bold; the line is 13dp grey.
  const std::string css = ReadAsset("themes/components.rcss");
  const std::string button = RuleBody(css, ".shell-layer-compact .settings-mock .me-profile-change");
  EXPECT_NE(button.find("height: 44dp"), std::string::npos);
  EXPECT_NE(RuleBody(css, ".shell-layer-compact .settings-mock .me-profile-name").find("font-size: 22dp"),
            std::string::npos);
  EXPECT_NE(RuleBody(css, ".shell-layer-compact .settings-mock .me-profile-caption").find("font-size: 13dp"),
            std::string::npos);
}

TEST_F(SettingsProfilePhoneTest, ChangePhotoIsBlueAndGreyedWhileAPhotoUploads) {
  const std::string tmpl = ReadAsset("themes/colors.rcss.tmpl");
  EXPECT_NE(RuleBody(tmpl, ".me-profile-change").find("color: {{accent}}"), std::string::npos);
  EXPECT_NE(RuleBody(tmpl, ".me-profile-change:disabled").find("color: {{label-3}}"), std::string::npos);
  EXPECT_NE(ReadAsset("themes/colors-light.rcss").find(".me-profile-change:disabled"), std::string::npos);
  EXPECT_NE(ReadAsset("themes/colors-dark.rcss").find(".me-profile-change:disabled"), std::string::npos);
}

TEST_F(SettingsProfilePhoneTest, ThePhoneHasNoInputAndNoCopyRowAndTheDesktopKeepsBoth) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const size_t desktop_input =
      rml.find("<div class=\"group-card settings-desktop-only\">\n          <label class=\"settings-field");
  ASSERT_NE(desktop_input, std::string::npos) << "the desktop's nickname input is in a desktop-only card";
  EXPECT_NE(rml.find("data-event-blur=\"on_profile_nickname_commit()\"", desktop_input), std::string::npos);
  EXPECT_EQ(Count(rml, "<input"), 1u);
  const size_t actions = rml.find("<div class=\"me-profile-actions row\">");
  ASSERT_NE(actions, std::string::npos);
  EXPECT_EQ(rml.substr(0, actions).find("copy_profile_id()"), std::string::npos);
  EXPECT_EQ(Count(rml, "copy_profile_id()"), 1u);
  EXPECT_NE(rml.find("copy_profile_id()", actions), std::string::npos);
  EXPECT_NE(rml.find("settings-desktop-only", actions), std::string::npos);
}

TEST_F(SettingsProfilePhoneTest, TheAccountGroupHoldsTheFiveRowsInOrderWithTodaysGates) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const size_t head =
      rml.find("<p class=\"text group-head settings-phone-only\">{{i18n:settings.profile.account_group}}</p>");
  ASSERT_NE(head, std::string::npos);
  const size_t card = rml.find("<div class=\"group-card settings-phone-only\">", head);
  const size_t id = rml.find("{{i18n:settings.profile.id_row}}", card);
  const size_t status = rml.find("{{i18n:settings.profile.registration_row}}", id);
  const size_t expires = rml.find("data-if=\"advanced && profile_registration_expires != ''\"", status);
  const size_t renew = rml.find("<div class=\"settings-toggle-row row group-row\" data-if=\"advanced\">", expires);
  const size_t node = rml.find("data-if=\"advanced && profile_peer_id != ''\"", renew);
  const size_t foot = rml.find("{{i18n:settings.profile.id_foot}}", node);
  for (const size_t at : {card, id, status, expires, renew, node, foot}) {
    ASSERT_NE(at, std::string::npos);
  }
  // The status row has no gate; it shows what the page showed before: the registration status.
  const size_t status_row = rml.rfind("<div class=\"group-row group-row--static\"", status);
  EXPECT_EQ(rml.substr(status_row, status - status_row).find("data-if"), std::string::npos);
  EXPECT_NE(rml.find("{{profile_registration_status}}", status), std::string::npos);
  // The renewal switch keeps its event and its state class.
  EXPECT_NE(rml.find("data-event-click=\"toggle_auto_renew_registration()\"", renew), std::string::npos);
  EXPECT_NE(rml.find("data-class-settings-switch--on=\"auto_renew_registration == 'auto'\"", renew), std::string::npos);
  // The register button is untouched: one place, behind its own binding, desktop and phone alike.
  EXPECT_EQ(Count(rml, "register_profile()"), 1u);
  EXPECT_EQ(Count(rml, "data-if=\"profile_show_register\""), 1u);
  EXPECT_EQ(Count(rml, "toggle_auto_renew_registration()"), 2u) << "one on the phone, one on the desktop";
}

TEST_F(SettingsProfilePhoneTest, TheCardStatusRowAndTheNewLinesAreWordedInBothLanguages) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.profile.registration_row"), "名片状态");
  EXPECT_EQ(Tr("settings.profile.change_photo"), "更换头像");
  EXPECT_EQ(Tr("settings.profile.name_caption"), "好友搜这个名字就能加你");
  EXPECT_EQ(Tr("settings.profile.id_foot"), "好友输入这个 ID 也能加你。复制、分享、二维码在“我”页的名片上。");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.profile.registration_row"), "Card status");
  EXPECT_EQ(Tr("settings.profile.change_photo"), "Change photo");
  EXPECT_EQ(Tr("settings.profile.name_caption"), "Friends can find you by this name");
  EXPECT_EQ(Tr("settings.profile.id_foot"),
            "Friends can also add you with this ID. Copy, share and QR code are on your card on the Me page.");
}

} // namespace
} // namespace pbr
