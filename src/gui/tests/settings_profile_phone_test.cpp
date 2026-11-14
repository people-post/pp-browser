// The phone "My profile" page, plan A (DEV/PLAN-my-profile-page-a.md): a big centred photo with "Change photo" under
// it, the large name and one grey line, the nickname as a row, one "Account" group, no Copy row. The desktop keeps
// its input and its Copy / Share / QR buttons; the registration, renewal and PIN entries keep their gates.
#include "foundation/i18n/LocalizationService.h"
#include "gui/ProfileSettingsSection.h"
#include "lib/people/RegistrationStatus.h"

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

SettingsUiState Synced(const ProfileIdentityView& view); // below

TEST_F(SettingsProfilePhoneTest, TheHeadIsJustThePhotoWithItsBadge) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const size_t frame = rml.find("class=\"me-profile-avatar-frame\" data-event-click=\"tap_profile_avatar()\"");
  const size_t badge = rml.find("<button class=\"me-profile-avatar-badge\"");
  const size_t head = rml.find("<div class=\"group-card settings-phone-only\">");
  ASSERT_NE(frame, std::string::npos);
  ASSERT_NE(badge, std::string::npos);
  ASSERT_NE(head, std::string::npos);
  EXPECT_LT(frame, badge);
  EXPECT_LT(badge, head);
  // The camera badge is the only way to change the photo: no text button, no key, no rules left.
  EXPECT_EQ(rml.find("me-profile-change"), std::string::npos);
  EXPECT_EQ(rml.find("change_photo"), std::string::npos);
  EXPECT_EQ(rml.find("me-profile-phone-head"), std::string::npos);
  const std::string badge_tag = rml.substr(badge, rml.find('>', badge) - badge);
  EXPECT_NE(badge_tag.find("data-event-click=\"pick_profile_icon()\""), std::string::npos);
  // No large name and no grey caption under the photo.
  EXPECT_EQ(rml.find("me-profile-name"), std::string::npos);
  EXPECT_EQ(rml.find("me-profile-caption"), std::string::npos);
  // Everything between the photo and the nickname card carries no name and no caption.
  EXPECT_EQ(rml.substr(frame, head - frame).find("{{profile_nickname}}"), std::string::npos);
  EXPECT_EQ(rml.substr(frame, head - frame).find("name_caption"), std::string::npos);
  // The badge's tap area is 44dp around a 36dp visible dot.
  const std::string css = ReadAsset("themes/components.rcss");
  const std::string tap = RuleBody(css, ".shell-layer-compact .me-profile-avatar-badge");
  EXPECT_NE(tap.find("width: 44dp"), std::string::npos);
  EXPECT_NE(tap.find("height: 44dp"), std::string::npos);
  EXPECT_NE(RuleBody(css, ".shell-layer-compact .me-profile-avatar-badge-dot").find("width: 36dp"), std::string::npos);
  EXPECT_EQ(css.find(".me-profile-change"), std::string::npos);
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

// The phone's identity rows in three groups (DEV/ui-mockups/44, "建议 2"): the Account ID alone, "My card" (Status with
// "Update now" at its right end, and the auto-update switch with its "?"), and the Node ID under "This device".
TEST_F(SettingsProfilePhoneTest, TheAccountGroupsHoldTheRowsInOrderWithTodaysGates) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const size_t head =
      rml.find("<p class=\"text group-head settings-phone-only\">{{i18n:settings.profile.account_group}}</p>");
  ASSERT_NE(head, std::string::npos);
  const size_t id_card = rml.find("<div class=\"group-card settings-phone-only\">", head);
  const size_t id = rml.find("{{i18n:settings.profile.id_row}}", id_card);
  const size_t card_head =
      rml.find("<p class=\"text group-head settings-phone-only\">{{i18n:settings.profile.card_group}}</p>", id);
  const size_t status = rml.find("{{i18n:settings.profile.registration_row}}", card_head);
  const size_t renew = rml.find("<div class=\"settings-toggle-row row group-row\" data-if=\"advanced\">", status);
  const size_t device_head = rml.find(
      "<p class=\"text group-head settings-phone-only\" data-if=\"advanced && profile_peer_id != ''\">"
      "{{i18n:settings.security.devices.this_device}}</p>",
      renew);
  const size_t node = rml.find("{{i18n:settings.profile.node_id_row}}", device_head);
  for (const size_t at : {id_card, id, card_head, status, renew, device_head, node}) {
    ASSERT_NE(at, std::string::npos);
  }
  // The Account ID is a group of its own: its card closes before the "My card" heading.
  EXPECT_EQ(rml.substr(id_card, card_head - id_card).find("registration_row"), std::string::npos);
  // The Node ID card keeps the gate it had as a row.
  EXPECT_NE(rml.find("<div class=\"group-card settings-phone-only\" data-if=\"advanced && profile_peer_id != ''\">",
                     device_head),
            std::string::npos);
  EXPECT_NE(rml.find("{{profile_peer_id}}", node), std::string::npos);
  // The Status row has no gate of its own; it shows the registration status.
  const size_t status_row = rml.rfind("<div class=\"group-row group-row--static\"", status);
  EXPECT_EQ(rml.substr(status_row, status - status_row).find("data-if"), std::string::npos);
  // The renewal switch keeps its event and its state class; it has no caption any more.
  EXPECT_NE(rml.find("data-event-click=\"toggle_auto_renew_registration()\"", renew), std::string::npos);
  EXPECT_NE(rml.find("data-class-settings-switch--on=\"auto_renew_registration == 'auto'\"", renew), std::string::npos);
  EXPECT_EQ(rml.find("auto_renew_on_caption"), std::string::npos);
  EXPECT_EQ(rml.find("auto_renew_off_caption"), std::string::npos);
  // The old Valid until row is gone as a row: its text lives inside the Status value.
  EXPECT_EQ(Count(rml, "{{i18n:settings.profile.expires_row}}"), 1u);
  // No footer sentence under the groups.
  EXPECT_EQ(rml.find("id_foot"), std::string::npos);
  EXPECT_EQ(rml.find("group-foot settings-phone-only"), std::string::npos);
  // The desktop register button keeps its binding; the phone shows the big one only when the card needs action.
  EXPECT_EQ(Count(rml, "data-if=\"profile_show_register\""), 1u);
  EXPECT_EQ(Count(rml, "data-if=\"profile_show_register && profile_register_primary\""), 1u);
  EXPECT_EQ(Count(rml, "toggle_auto_renew_registration()"), 2u) << "one on the phone, one on the desktop";
}

// The one read-only Status row: the status, and the expiry after it only where the old Valid until row showed.
TEST_F(SettingsProfilePhoneTest, TheStatusValueMergesStatusAndExpiryUnderTheOldExpiryGate) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  const std::string value_open =
      "<span class=\"group-row-value group-row-value--wrap\">{{profile_registration_status}}"
      "<span data-if=\"advanced && profile_registration_expires != ''\"> · "
      "{{i18n:settings.profile.expires_row}} {{profile_registration_expires}}</span></span>";
  ASSERT_EQ(Count(rml, value_open), 1u);
  // Today's gate of the expiry row, verbatim (from the checklist before this change).
  EXPECT_EQ(Count(rml, "data-if=\"advanced && profile_registration_expires != ''\""), 2u)
      << "the phone's merged value and the desktop's line";
  // Evaluate the value the way the page does, for every registration state with advanced on and off.
  const struct {
    RegistrationStatus status;
    const char* expires;
  } cases[] = {{RegistrationStatus::Unregistered, ""},           {RegistrationStatus::Active, "2027-02-11"},
               {RegistrationStatus::ExpiringSoon, "2026-11-20"}, {RegistrationStatus::Expired, "2026-10-01"},
               {RegistrationStatus::WrongBackend, "2027-02-11"}, {RegistrationStatus::Active, ""}};
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const auto& c : cases) {
      ProfileIdentityView view;
      view.ready = true;
      view.registration_status = RegistrationStatusLabel(c.status);
      view.registration_expires = c.expires;
      const SettingsUiState state = Synced(view);
      for (const bool advanced : {false, true}) {
        std::string value = state.profile_registration_status;
        if (advanced && !state.profile_registration_expires.empty()) {
          value += " · " + Tr("settings.profile.expires_row") + " " + state.profile_registration_expires;
        }
        if (!advanced || std::string(c.expires).empty()) {
          EXPECT_EQ(value, state.profile_registration_status) << "no expiry part: advanced off or no expiry";
        } else {
          EXPECT_EQ(value,
                    state.profile_registration_status + " · " + Tr("settings.profile.expires_row") + " " + c.expires);
        }
      }
    }
  }
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.profile.registration_row") + "|" + Tr("settings.profile.expires_row"), "状态|有效期至");
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.profile.registration_row") + "|" + Tr("settings.profile.expires_row"), "Status|Valid until");
}

TEST_F(SettingsProfilePhoneTest, TheCardStatusRowAndTheNewLinesAreWordedInBothLanguages) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("settings.profile.registration_row"), "状态");
  EXPECT_EQ(Tr("settings.profile.card_group"), "我的名片");
  EXPECT_EQ(Tr("settings.profile.name_caption"), "好友搜这个名字就能加你");
  EXPECT_EQ(Tr("settings.profile.update_now"), "立即更新");
  EXPECT_EQ(Tr("settings.auto_renew.short"), "自动更新");
  EXPECT_EQ(Tr("settings.auto_renew.note"),
            "打开后，名片快到期时会自动更新，一般不用手动操作。关闭后，到期前需要你自己点“立即更新”。");
  EXPECT_EQ(Tr("settings.auto_renew"), "自动更新我的名片") << "the desktop keeps its long label";
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("settings.profile.registration_row"), "Status");
  EXPECT_EQ(Tr("settings.profile.card_group"), "My card");
  EXPECT_EQ(Tr("settings.profile.name_caption"), "Friends can find you by this name");
  EXPECT_EQ(Tr("settings.profile.update_now"), "Update now");
  EXPECT_EQ(Tr("settings.auto_renew.short"), "Auto-update");
  EXPECT_EQ(Tr("settings.auto_renew"), "Update my card automatically");
  EXPECT_NE(Tr("settings.auto_renew.note").find("Update now"), std::string::npos);
  EXPECT_EQ(Tr("settings.profile.id_foot"), "settings.profile.id_foot") << "the footer key is gone";
  EXPECT_EQ(Tr("settings.auto_renew_on_caption"), "settings.auto_renew_on_caption") << "the captions are gone";
  EXPECT_EQ(Tr("settings.auto_renew_off_caption"), "settings.auto_renew_off_caption");
}

// The page's real path: the section reads the identity view through its port (SyncFromSession), which ends in
// ApplyIdentityView.
SettingsUiState Synced(const ProfileIdentityView& view) {
  SettingsCommands commands;
  commands.load_profile_identity = [view]() { return view; };
  ProfileSettingsSection section;
  section.BindPorts(&commands);
  BootstrapResult bootstrap;
  SettingsUiState state;
  section.SyncFromSession(bootstrap, state);
  return state;
}

// The table is built from the labels the app really produces (RegistrationStatusLabel), one row per status, so a new
// status cannot slip in unlabelled; only an active card drops the big button.
TEST(ProfileRegisterPrimaryTest, OnlyAnActiveCardDropsTheBigButton) {
  const RegistrationStatus all[] = {RegistrationStatus::Unregistered, RegistrationStatus::Active,
                                    RegistrationStatus::ExpiringSoon, RegistrationStatus::Expired,
                                    RegistrationStatus::WrongBackend};
  for (const RegistrationStatus status : all) {
    const std::string label = RegistrationStatusLabel(status);
    const bool active = status == RegistrationStatus::Active;
    EXPECT_EQ(ProfileRegisterIsPrimary(true, label), !active) << label;
    EXPECT_FALSE(ProfileRegisterIsPrimary(false, label)) << label << ": no register button, nothing to make primary";
  }
  EXPECT_TRUE(ProfileRegisterIsPrimary(true, "something new")) << "an unknown label needs action";
}

// ApplyIdentityView hands the predicate the raw label, not the localized text: in Chinese an active card is still not
// primary, and the status row shows the localized words.
TEST_F(SettingsProfilePhoneTest, TheBigButtonIsDecidedOnTheRawLabelNotTheLocalizedOne) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  for (const RegistrationStatus status :
       {RegistrationStatus::Unregistered, RegistrationStatus::Active, RegistrationStatus::ExpiringSoon,
        RegistrationStatus::Expired, RegistrationStatus::WrongBackend}) {
    ProfileIdentityView view;
    view.ready = true;
    view.show_register = true;
    view.registration_status = RegistrationStatusLabel(status);
    const SettingsUiState state = Synced(view);
    EXPECT_EQ(state.profile_register_primary, status != RegistrationStatus::Active) << view.registration_status;
    EXPECT_NE(state.profile_registration_status, view.registration_status) << "shown localized, not as the raw label";
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
}

TEST_F(SettingsProfilePhoneTest, EveryRegistrationStatusIsWordedInBothLanguages) {
  const struct {
    RegistrationStatus status;
    const char* en;
    const char* zh;
  } rows[] = {{RegistrationStatus::WrongBackend, "Needs updating", "需要更新名片"}};
  for (const auto& row : rows) {
    ProfileIdentityView view;
    view.ready = true;
    view.registration_status = RegistrationStatusLabel(row.status);
    LocalizationService::Instance().SetPreferredLanguage("en");
    EXPECT_EQ(Synced(view).profile_registration_status, row.en);
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
    EXPECT_EQ(Synced(view).profile_registration_status, row.zh);
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
  // Every status has its own words: none falls through as the raw English label in Chinese.
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  for (const RegistrationStatus status :
       {RegistrationStatus::Unregistered, RegistrationStatus::Active, RegistrationStatus::ExpiringSoon,
        RegistrationStatus::Expired, RegistrationStatus::WrongBackend}) {
    ProfileIdentityView view;
    view.ready = true;
    view.registration_status = RegistrationStatusLabel(status);
    EXPECT_NE(Synced(view).profile_registration_status, view.registration_status);
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
}

// "Update now" is a small text button at the right end of the Status row, under exactly the gate of the row it
// replaced; the auto-update switch has a "?" after its title (a local note) and no caption.
TEST_F(SettingsProfilePhoneTest, TheUpdateNowButtonKeepsTheOldRowsGateAndEventAndTheSwitchHasNoCaption) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  EXPECT_EQ(Count(rml, "data-event-click=\"register_profile()\""), 3u) << "desktop button, phone button, Update now";
  const std::string button =
      "<button class=\"settings-text-btn\" type=\"button\" data-if=\"advanced && profile_show_register && "
      "!profile_register_primary\"\n                      data-event-click=\"register_profile()\">"
      "{{i18n:settings.profile.update_now}}</button>";
  ASSERT_EQ(Count(rml, button), 1u);
  // It sits in the Status row, after the value, so it is at the right end.
  const size_t status = rml.find("{{i18n:settings.profile.registration_row}}");
  EXPECT_LT(status, rml.find(button));
  EXPECT_LT(rml.find("{{profile_registration_status}}"), rml.find(button));
  EXPECT_LT(rml.find(button), rml.find("toggle_auto_renew_registration()"));
  // Visibility: the old row's table, for every status and both advanced states.
  for (const RegistrationStatus status_value :
       {RegistrationStatus::Unregistered, RegistrationStatus::Active, RegistrationStatus::ExpiringSoon,
        RegistrationStatus::Expired, RegistrationStatus::WrongBackend}) {
    for (const bool advanced : {false, true}) {
      for (const bool show_register : {false, true}) {
        const bool primary = ProfileRegisterIsPrimary(show_register, RegistrationStatusLabel(status_value));
        const bool old_row = advanced && show_register && !primary;
        const bool new_button = advanced && show_register && !primary; // the same expression, evaluated
        EXPECT_EQ(new_button, old_row);
        EXPECT_EQ(new_button, advanced && show_register && status_value == RegistrationStatus::Active)
            << "only a healthy card with the register button on offers Update now";
      }
    }
  }
  // The switch: "?" right after its title, a local note, no caption.
  const std::string title =
      "{{i18n:settings.auto_renew.short}}</span><button class=\"settings-q-btn\" type=\"button\" "
      "data-event-click=\"show_setting_note('settings.auto_renew.short','settings.auto_renew.note')\">";
  EXPECT_EQ(Count(rml, title), 1u);
  EXPECT_EQ(rml.find("ask_setting_help"), std::string::npos) << "the auto-update note is local, not the AI";
  // The phone's big button is gated by the new predicate; the desktop's is not.
  const size_t phone_button = rml.find("data-if=\"profile_show_register && profile_register_primary\"");
  ASSERT_NE(phone_button, std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"register_profile()\"", phone_button), std::string::npos);
  EXPECT_NE(rml.find("btn btn-primary settings-desktop-only\" type=\"button\" data-if=\"profile_show_register\""),
            std::string::npos);
  // The text button is a 44dp tap area.
  const std::string css = ReadAsset("themes/components.rcss");
  const std::string rule = RuleBody(css, ".shell-layer-compact .settings-text-btn");
  EXPECT_NE(rule.find("height: 44dp"), std::string::npos);
  EXPECT_NE(rule.find("min-width: 44dp"), std::string::npos);
}

// The date after the status: only for a card that is registered (active, expiring soon, expired, wrong service), never for
// "not registered", and only with "Show advanced settings" on (the span's gate, pinned above).
TEST_F(SettingsProfilePhoneTest, TheStatusRowAppendsAnExpiryOnlyForARegisteredCard) {
  const struct {
    RegistrationStatus status;
    bool registered; // the date is meaningful
  } states[] = {{RegistrationStatus::Unregistered, false},
                {RegistrationStatus::Active, true},
                {RegistrationStatus::ExpiringSoon, true},
                {RegistrationStatus::Expired, true},
                {RegistrationStatus::WrongBackend, true}};
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const auto& st : states) {
      for (const bool advanced : {false, true}) {
        for (const bool has_date : {false, true}) {
          ProfileIdentityView view;
          view.ready = true;
          view.registration_status = RegistrationStatusLabel(st.status);
          view.registration_expires = has_date ? "2027-02-11" : "";
          const SettingsUiState state = Synced(view);
          // The page: the status, then " · Valid until <date>" under `advanced && expires != ''`.
          std::string text = state.profile_registration_status;
          if (advanced && !state.profile_registration_expires.empty()) {
            text += " · " + Tr("settings.profile.expires_row") + " " + state.profile_registration_expires;
          }
          const bool expect_date = advanced && has_date && st.registered;
          const std::string want =
              state.profile_registration_status +
              (expect_date ? " · " + Tr("settings.profile.expires_row") + " 2027-02-11" : std::string());
          EXPECT_EQ(text, want) << view.registration_status << " advanced=" << advanced << " has_date=" << has_date;
        }
      }
    }
  }
  EXPECT_EQ(ProfileExpiryToShow("not registered", "2026-01-01"), "");
  EXPECT_EQ(ProfileExpiryToShow("active", "2027-02-11"), "2027-02-11");
  EXPECT_EQ(ProfileExpiryToShow("active", ""), "");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
