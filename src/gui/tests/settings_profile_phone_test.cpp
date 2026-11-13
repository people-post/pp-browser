// The phone "My profile" page, plan A (DEV/PLAN-my-profile-page-a.md): a big centred photo with "Change photo" under
// it, the large name and one grey line, the nickname as a row, one "Account" group, no Copy row. The desktop keeps
// its input and its Copy / Share / QR buttons; the registration, renewal and PIN entries keep their gates.
#include "foundation/i18n/LocalizationService.h"
#include "gui/ProfileSettingsSection.h"

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

TEST_F(SettingsProfilePhoneTest, TheHeadIsJustThePhotoWithItsBadgeAndChangePhoto) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const size_t frame = rml.find("class=\"me-profile-avatar-frame\" data-event-click=\"tap_profile_avatar()\"");
  const size_t badge = rml.find("<button class=\"me-profile-avatar-badge\"");
  const size_t head = rml.find("<div class=\"me-profile-phone-head settings-phone-only\">");
  ASSERT_NE(frame, std::string::npos);
  ASSERT_NE(badge, std::string::npos);
  ASSERT_NE(head, std::string::npos);
  EXPECT_LT(frame, badge);
  EXPECT_LT(badge, head);
  EXPECT_NE(rml.find("<button class=\"me-profile-change\"", head), std::string::npos);
  // No large name and no grey caption under the photo.
  EXPECT_EQ(rml.find("me-profile-name"), std::string::npos);
  EXPECT_EQ(rml.find("me-profile-caption"), std::string::npos);
  const size_t head_end = rml.find("<div class=\"group-card settings-phone-only\">", head);
  ASSERT_NE(head_end, std::string::npos);
  EXPECT_EQ(rml.substr(head, head_end - head).find("{{profile_nickname}}"), std::string::npos);
  EXPECT_EQ(rml.substr(head, head_end - head).find("name_caption"), std::string::npos);
  // The change button is a 44dp target in blue.
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(RuleBody(css, ".shell-layer-compact .settings-mock .me-profile-change").find("height: 44dp"),
            std::string::npos);
  EXPECT_EQ(css.find(".me-profile-name"), std::string::npos);
  EXPECT_EQ(css.find(".me-profile-caption"), std::string::npos);
}

TEST_F(SettingsProfilePhoneTest, TheNicknameRowCarriesTheCaptionUnderItsTitleAndStaysOneTap) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const size_t tap = rml.find("data-event-click=\"edit_profile_nickname()\"");
  ASSERT_NE(tap, std::string::npos);
  const size_t title = rml.find("{{i18n:settings.nickname}}", tap);
  const size_t caption = rml.find("<span class=\"group-row-sub\">{{i18n:settings.profile.name_caption}}</span>", title);
  const size_t value = rml.find("{{profile_nickname}}", caption);
  const size_t chevron = rml.find("group-chevron", value);
  const size_t end = rml.find("</button>", tap);
  for (const size_t at : {title, caption, value, chevron}) {
    ASSERT_NE(at, std::string::npos);
    EXPECT_LT(at, end) << "all inside the one tappable button";
  }
  EXPECT_LT(title, caption);
  EXPECT_LT(caption, value);
  EXPECT_LT(value, chevron);
  EXPECT_EQ(Count(rml, "name_caption"), 1u);
  const std::string css = ReadAsset("themes/components.rcss");
  EXPECT_NE(RuleBody(css, ".shell-layer-compact .group-row-sub").find("font-size: 13dp"), std::string::npos);
  EXPECT_NE(RuleBody(css, ".shell-layer-compact .settings-mock .group-row-tap").find("min-height: 52dp"),
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

TEST_F(SettingsProfilePhoneTest, TheAccountGroupHoldsTheRowsInOrderWithTodaysGates) {
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
  for (const size_t at : {card, id, status, expires, renew, node}) {
    ASSERT_NE(at, std::string::npos);
  }
  // The status row has no gate; it shows what the page showed before: the registration status.
  const size_t status_row = rml.rfind("<div class=\"group-row group-row--static\"", status);
  EXPECT_EQ(rml.substr(status_row, status - status_row).find("data-if"), std::string::npos);
  EXPECT_NE(rml.find("{{profile_registration_status}}", status), std::string::npos);
  // The renewal switch keeps its event and its state class.
  EXPECT_NE(rml.find("data-event-click=\"toggle_auto_renew_registration()\"", renew), std::string::npos);
  EXPECT_NE(rml.find("data-class-settings-switch--on=\"auto_renew_registration == 'auto'\"", renew), std::string::npos);
  // No footer sentence under the group.
  EXPECT_EQ(rml.find("id_foot"), std::string::npos);
  EXPECT_EQ(rml.find("group-foot settings-phone-only"), std::string::npos);
  // The desktop register button keeps its binding; the phone shows it only when the card needs action.
  EXPECT_EQ(Count(rml, "data-if=\"profile_show_register\""), 1u);
  EXPECT_EQ(Count(rml, "data-if=\"profile_show_register && profile_register_primary\""), 1u);
  EXPECT_EQ(Count(rml, "toggle_auto_renew_registration()"), 2u) << "one on the phone, one on the desktop";
}

TEST_F(SettingsProfilePhoneTest, TheCardStatusRowAndTheNewLinesAreWordedInBothLanguages) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.profile.registration_row"), "名片状态");
  EXPECT_EQ(Tr("settings.profile.change_photo"), "更换头像");
  EXPECT_EQ(Tr("settings.profile.name_caption"), "好友搜这个名字就能加你");
  EXPECT_EQ(Tr("settings.profile.update_now"), "立即更新一次");
  EXPECT_EQ(Tr("settings.auto_renew_on_caption"), "已打开：名片会自动保持最新，一般不用手动更新");
  EXPECT_EQ(Tr("settings.auto_renew_off_caption"), "已关闭：名片到期前需要你手动更新");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.profile.registration_row"), "Card status");
  EXPECT_EQ(Tr("settings.profile.change_photo"), "Change photo");
  EXPECT_EQ(Tr("settings.profile.name_caption"), "Friends can find you by this name");
  EXPECT_EQ(Tr("settings.profile.update_now"), "Update now");
  EXPECT_EQ(Tr("settings.auto_renew_on_caption"), "On: your card stays up to date by itself");
  EXPECT_EQ(Tr("settings.auto_renew_off_caption"), "Off: update your card yourself before it expires");
  EXPECT_EQ(Tr("settings.profile.id_foot"), "settings.profile.id_foot") << "the footer key is gone";
}

TEST(ProfileRegisterPrimaryTest, OnlyAnActiveCardDropsTheBigButton) {
  struct Row {
    bool show_register;
    const char* status;
    bool primary;
  };
  const Row table[] = {
      {true, "not registered", true}, {true, "expired", true},       {true, "expiring soon", true},
      {true, "needs renewal", true},  {true, "something new", true}, {true, "active", false},
      {false, "active", false},       {false, "expired", false},
  };
  for (const Row& row : table) {
    EXPECT_EQ(ProfileRegisterIsPrimary(row.show_register, row.status), row.primary) << row.status;
  }
}

TEST_F(SettingsProfilePhoneTest, TheUpdateNowRowAndTheBigButtonBothFireRegisterProfileAndTheSwitchHasCaptions) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  EXPECT_EQ(Count(rml, "data-event-click=\"register_profile()\""), 3u) << "desktop button, phone button, Update now";
  const size_t card = rml.find("<p class=\"text group-head settings-phone-only\">");
  const size_t toggle = rml.find("<div class=\"settings-toggle-row row group-row\" data-if=\"advanced\">", card);
  const size_t on =
      rml.find("data-if=\"auto_renew_registration == 'auto'\">{{i18n:settings.auto_renew_on_caption}}", toggle);
  const size_t off =
      rml.find("data-if=\"auto_renew_registration != 'auto'\">{{i18n:settings.auto_renew_off_caption}}", on);
  const size_t switch_event = rml.find("data-event-click=\"toggle_auto_renew_registration()\"", off);
  const size_t plain =
      rml.find("data-if=\"advanced && profile_show_register && !profile_register_primary\"", switch_event);
  ASSERT_NE(toggle, std::string::npos);
  for (const size_t at : {on, off, switch_event, plain}) {
    ASSERT_NE(at, std::string::npos);
  }
  const size_t plain_event = rml.find("data-event-click=\"register_profile()\"", plain);
  const size_t plain_label = rml.find("{{i18n:settings.profile.update_now}}", plain);
  ASSERT_NE(plain_event, std::string::npos);
  EXPECT_LT(plain_event, plain_label);
  // The plain row sits in the account group right after the switch row, before the Node ID row.
  EXPECT_LT(plain, rml.find("{{i18n:settings.profile.node_id_row}}", plain));
  // The phone's big button is gated by the new predicate; the desktop's is not.
  const size_t phone_button = rml.find("data-if=\"profile_show_register && profile_register_primary\"");
  ASSERT_NE(phone_button, std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"register_profile()\"", phone_button), std::string::npos);
  EXPECT_NE(rml.find("btn btn-primary settings-desktop-only\" type=\"button\" data-if=\"profile_show_register\""),
            std::string::npos);
}

} // namespace
} // namespace pbr
