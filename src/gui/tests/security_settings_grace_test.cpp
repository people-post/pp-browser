// PIN grace window (#21, DEV/PLAN-pin-grace.md "设置页"): Me → Security → "Ask for PIN again after" — every time / 5 /
// 15 / 60 minutes, saved in this profile's preferences and handed to the app (set_grace_minutes); the row only shows
// with an own PIN on a platform that keeps the window. And the transfer code asks for that PIN first (plan ⑧), on the PIN
// gate like Change PIN (DEV/PLAN-pin-gate-redesign.md B2).
#include "gui/SecuritySettingsSection.h"
#include "gui/SettingsSecurityActions.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/PinDefaults.h"
#include "foundation/i18n/LocalizationService.h"

#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

class FakeSettingsHost final : public SettingsHost {
public:
  explicit FakeSettingsHost(SessionStore& store) : store_(store) {}
  SessionStore& Store() override { return store_; }
  void ReportFailure(const Error&) override { ++failures; }
  void ShowStatus(const std::string&) override {}
  void Resync(bool) override {}
  SettingsUiState& UiState() override { return state_; }
  void PullFromView() override {}
  void PushToView(bool) override { ++pushes; }
  void SaveProfileNickname() override {}
  void RequestSyncLayout(const char*) override {}
  Roe<void> ForgetToolPermissions() override { return {}; }
  void OpenChangePin() override {}

  int failures = 0;
  int pushes = 0;

private:
  SessionStore& store_;
  SettingsUiState state_;
};

class SecuritySettingsGraceTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("pp_security_grace_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "profile");
    BootstrapResult bootstrap{};
    bootstrap.config = Config::DefaultAppConfig();
    bootstrap.config_path = (root_ / "config.json").string();
    bootstrap.data_dir = root_.string();
    bootstrap.profile_data_dir = (root_ / "profile").string();
    bootstrap.profile_prefs = UserPreferences::DefaultProfile();
    bootstrap.profile_prefs.pin_is_default = false;
    store_.SetUiDemo(false);
    store_.Initialize(std::move(bootstrap));

    commands_.load_pin_protection = []() { return PinProtectionView{.ready = true, .unlocked = true}; };
    commands_.grace_supported = [this]() { return grace_supported_; };
    commands_.set_grace_minutes = [this](const int minutes) { grace_calls_.push_back(minutes); };
    section_.BindPorts(&commands_);
  }

  void TearDown() override { std::filesystem::remove_all(root_); }

  void SetPinIsDefault(const bool is_default) {
    ProfilePreferences prefs = store_.Snapshot().profile_prefs;
    prefs.pin_is_default = is_default;
    ASSERT_TRUE(static_cast<bool>(store_.SaveProfilePrefs(prefs)));
  }

  SettingsUiState Synced() {
    SettingsUiState state;
    section_.SyncFromSession(store_.Snapshot(), state);
    return state;
  }

  std::filesystem::path root_;
  SessionStore store_;
  SettingsCommands commands_;
  SecuritySettingsSection section_;
  bool grace_supported_ = true;
  std::vector<int> grace_calls_;
};

TEST_F(SecuritySettingsGraceTest, TheFourValuesAreSavedAndHandedToTheApp) {
  SettingsUiState state = Synced();
  EXPECT_EQ(state.unlock_grace_minutes, 15) << "15 minutes unless chosen otherwise";
  EXPECT_TRUE(section_.IsPersisted(state, store_.Snapshot()));

  for (const int minutes : {0, 5, 60, 15}) {
    state.unlock_grace_minutes = minutes;
    EXPECT_FALSE(section_.IsPersisted(state, store_.Snapshot())) << minutes;
    ASSERT_TRUE(static_cast<bool>(section_.Flush(state, store_)));
    EXPECT_EQ(store_.Snapshot().profile_prefs.unlock_grace_minutes, minutes);
    EXPECT_EQ(state.unlock_grace_minutes, minutes);
    EXPECT_TRUE(section_.IsPersisted(state, store_.Snapshot()));
  }
  // "Every time" forgets the window (0); any other value restarts it at that length.
  EXPECT_EQ(grace_calls_, (std::vector<int>{0, 5, 60, 15}));

  // The same value again is no change: nothing saved, the app not told.
  ASSERT_TRUE(static_cast<bool>(section_.Flush(state, store_)));
  EXPECT_EQ(grace_calls_.size(), 4U);
}

TEST_F(SecuritySettingsGraceTest, AlwaysIsSavedShownAndHandedToTheApp) {
  SettingsUiState state = Synced();
  state.unlock_grace_minutes = kUnlockGraceAlways;
  ASSERT_TRUE(static_cast<bool>(section_.Flush(state, store_)));
  EXPECT_EQ(store_.Snapshot().profile_prefs.unlock_grace_minutes, kUnlockGraceAlways);
  EXPECT_EQ(grace_calls_, (std::vector<int>{kUnlockGraceAlways}));
  const SettingsUiState back = Synced();
  EXPECT_EQ(back.unlock_grace_minutes, kUnlockGraceAlways);
  EXPECT_EQ(back.unlock_grace_label, UnlockGraceDisplayLabel(kUnlockGraceAlways));
  EXPECT_NE(back.unlock_grace_label, UnlockGraceDisplayLabel(15));
}

TEST_F(SecuritySettingsGraceTest, ChoosingEveryTimeTellsTheAppZeroAndAnHourSixty) {
  SettingsUiState state = Synced();
  state.unlock_grace_minutes = 0;
  ASSERT_TRUE(static_cast<bool>(section_.Flush(state, store_)));
  EXPECT_EQ(grace_calls_, (std::vector<int>{0}));
  state.unlock_grace_minutes = 60;
  ASSERT_TRUE(static_cast<bool>(section_.Flush(state, store_)));
  EXPECT_EQ(grace_calls_, (std::vector<int>{0, 60}));
  EXPECT_EQ(Synced().unlock_grace_minutes, 60);
}

TEST_F(SecuritySettingsGraceTest, TheRowShowsOnlyWithAnOwnPinWhereTheWindowIsKept) {
  EXPECT_TRUE(Synced().security_grace_shown);

  SetPinIsDefault(true);
  EXPECT_FALSE(Synced().security_grace_shown) << "the app default PIN opens silently anyway";
  SetPinIsDefault(false);

  grace_supported_ = false;
  EXPECT_FALSE(Synced().security_grace_shown) << "no secure store on this platform";
  grace_supported_ = true;

  commands_.grace_supported = nullptr;
  EXPECT_FALSE(Synced().security_grace_shown) << "the app did not say";
  commands_.grace_supported = [this]() { return grace_supported_; };

  commands_.load_pin_protection = []() { return PinProtectionView{}; };
  EXPECT_FALSE(Synced().security_grace_shown) << "no PIN set up yet";
}

TEST_F(SecuritySettingsGraceTest, ThePageHasTheRowItsHelpAndBothLanguages) {
  const std::string rml = ReadAsset("views/settings_section_security.rml");
  EXPECT_NE(rml.find("data-if=\"security_grace_shown\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"on_choose_unlock_grace()\""), std::string::npos);
  EXPECT_NE(rml.find("{{unlock_grace_label}}"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.security.grace.help}}"), std::string::npos);
  EXPECT_LT(rml.find("on_choose_unlock_grace"), rml.find("on_choose_group_invite_policy")) << "under the PIN";

  // B2: no PIN field of their own any more; the PIN gate asks (its number pad for a digit PIN).
  for (const char* view : {"views/settings_security_transfer.rml", "views/settings_security_pin.rml",
                           "views/settings_section_security.rml"}) {
    EXPECT_EQ(ReadAsset(view).find("type=\"password\""), std::string::npos) << view;
  }

  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string text = ReadAsset(locale);
    for (const char* key : {"settings.security.grace.title", "settings.security.grace.now", "settings.security.grace.5",
                            "settings.security.grace.15", "settings.security.grace.60",
                            "settings.security.grace.always", "settings.security.grace.help",
                            "pin.verify_export_message", "pin.verify_recovery_message", "pin.change_old_message"}) {
      EXPECT_NE(text.find(std::string("\"") + key + "\""), std::string::npos) << locale << " " << key;
    }
  }
}

class TransferCodePinTest : public SecuritySettingsGraceTest {
protected:
  void SetUp() override {
    SecuritySettingsGraceTest::SetUp();
    commands_.export_link_device = [this]() -> Roe<std::string> {
      ++exports_;
      return std::string("code");
    };
    commands_.verify_pin = [](const SecretText&) -> Roe<void> { return {}; };
    commands_.change_pin = [this](const SecretText& old_pin, const SecretText& new_pin) -> Roe<void> {
      changed_.emplace_back(old_pin.Reveal(), new_pin.Reveal());
      return {};
    };
    feedback_.show_confirm = [this](const std::string&, const std::string&, std::function<void(bool)>,
                                    const std::string&) { ++confirms_; };
    pin_prompt_.show_verify = [this](const PinVerifyPurpose purpose, PinGateCheck, std::function<void()> on_ok) {
      purposes_.push_back(purpose);
      on_ok_ = std::move(on_ok);
    };
    pin_prompt_.show_change = [this](PinGateCheck check_old, PinGateChange change,
                                     std::optional<SecretText> current_pin) {
      check_old_ = std::move(check_old);
      change_ = std::move(change);
      current_pin_ = current_pin ? std::optional<std::string>(current_pin->Reveal()) : std::nullopt;
    };
  }

  ShellFeedbackPorts feedback_;
  UnlockEnsurePorts unlock_;
  PinPromptPorts pin_prompt_;
  std::vector<PinVerifyPurpose> purposes_;
  std::function<void()> on_ok_;
  PinGateCheck check_old_;
  PinGateChange change_;
  std::optional<std::string> current_pin_;
  std::vector<std::pair<std::string, std::string>> changed_;
  int confirms_ = 0;
  int exports_ = 0;
};

TEST_F(TransferCodePinTest, AnOwnPinIsAskedForBeforeTheCode) {
  FakeSettingsHost host(store_);
  SettingsSecurityActions actions(host, commands_, feedback_, unlock_, pin_prompt_);

  actions.ExportLinkDevice();
  ASSERT_EQ(purposes_, std::vector<PinVerifyPurpose>{PinVerifyPurpose::ExportTransferCode}) << "the PIN gate first";
  EXPECT_EQ(confirms_, 0);
  EXPECT_EQ(exports_, 0);

  ASSERT_TRUE(on_ok_);
  on_ok_(); // the gate took the PIN and closed
  EXPECT_EQ(confirms_, 1);
  EXPECT_EQ(host.failures, 0);
}

TEST_F(TransferCodePinTest, TheAppDefaultPinOnlyConfirmsAsBefore) {
  SetPinIsDefault(true);
  FakeSettingsHost host(store_);
  SettingsSecurityActions actions(host, commands_, feedback_, unlock_, pin_prompt_);

  actions.ExportLinkDevice();
  EXPECT_TRUE(purposes_.empty());
  EXPECT_EQ(confirms_, 1);
  EXPECT_EQ(host.failures, 0);
}

// B2: Change PIN is the gate's change mode; the engine's change is the same, and the new PIN's kind is kept so the
// gate's number pad follows it.
TEST_F(TransferCodePinTest, ChangePinGoesThroughTheGateAndKeepsTheNewKind) {
  FakeSettingsHost host(store_);
  SettingsSecurityActions actions(host, commands_, feedback_, unlock_, pin_prompt_);

  actions.ChangePin();
  ASSERT_TRUE(check_old_);
  ASSERT_TRUE(change_);

  int done = 0;
  change_("4321", "135790", [&done](Roe<void> result) {
    EXPECT_TRUE(static_cast<bool>(result));
    ++done;
  });
  EXPECT_EQ(done, 1);
  EXPECT_EQ(changed_, (std::vector<std::pair<std::string, std::string>>{{"4321", "135790"}}));
  EXPECT_EQ(store_.Snapshot().profile_prefs.pin_digits, 6);
  EXPECT_FALSE(store_.Snapshot().profile_prefs.pin_is_default);

  change_("135790", "correct horse", [&done](Roe<void>) { ++done; });
  EXPECT_EQ(store_.Snapshot().profile_prefs.pin_digits, 0) << "a text PIN";

  commands_.change_pin = [](const SecretText&, const SecretText&) -> Roe<void> { return Error("refused"); };
  std::optional<bool> ok;
  change_("correct horse", "2468", [&ok](Roe<void> result) { ok = static_cast<bool>(result); });
  ASSERT_TRUE(ok.has_value());
  EXPECT_FALSE(*ok) << "the gate shows the failure";
  EXPECT_EQ(store_.Snapshot().profile_prefs.pin_digits, 0) << "nothing written";
}

// Kenneth 2026-11-07: Change PIN on the app default PIN (public anyway) skips the "current PIN" step; the change uses
// the default PIN as the old one. An own PIN still starts with it.
TEST_F(TransferCodePinTest, ChangePinOnTheDefaultPinSkipsTheCurrentPin) {
  FakeSettingsHost host(store_);
  SettingsSecurityActions actions(host, commands_, feedback_, unlock_, pin_prompt_);
  actions.ChangePin();
  EXPECT_FALSE(current_pin_.has_value()) << "an own PIN: asked first";

  SetPinIsDefault(true);
  actions.ChangePin();
  ASSERT_TRUE(current_pin_.has_value());
  EXPECT_EQ(*current_pin_, std::string(kDefaultProfilePin));
}

// --- Face / fingerprint unlock (at-rest A014, DEV/PLAN-pin-gate-redesign.md C): Me → Security's switch. ---

class SecuritySettingsBiometricTest : public TransferCodePinTest {
protected:
  void SetUp() override {
    TransferCodePinTest::SetUp();
    commands_.biometric_kind = [this]() { return kind_; };
    commands_.biometric_enabled = [this]() { return enabled_; };
    commands_.enable_biometric = [this](const SecretText&) -> Roe<void> {
      enabled_ = true;
      return {};
    };
    commands_.disable_biometric = [this]() {
      enabled_ = false;
      ++disables_;
    };
  }

  std::string kind_ = "face";
  bool enabled_ = false;
  int disables_ = 0;
};

TEST_F(SecuritySettingsBiometricTest, TheSwitchShowsWithAnOwnPinOnADeviceWithTheSensor) {
  SettingsUiState state = Synced();
  EXPECT_TRUE(state.security_biometric_shown) << "supported + a PIN set + not the default";
  EXPECT_FALSE(state.security_biometric_on);
  EXPECT_EQ(state.security_biometric_label, Tr("settings.security.biometric.face"));
  enabled_ = true;
  EXPECT_TRUE(Synced().security_biometric_on);

  kind_ = "touch";
  EXPECT_EQ(Synced().security_biometric_label, Tr("settings.security.biometric.touch"));
  kind_ = "fingerprint";
  EXPECT_EQ(Synced().security_biometric_label, Tr("settings.security.biometric.fingerprint"));

  SetPinIsDefault(true);
  EXPECT_FALSE(Synced().security_biometric_shown) << "the app default PIN";
  EXPECT_FALSE(Synced().security_biometric_on);
  SetPinIsDefault(false);

  kind_.clear();
  EXPECT_FALSE(Synced().security_biometric_shown) << "no sensor (or no secure store)";
  kind_ = "face";

  commands_.load_pin_protection = []() { return PinProtectionView{}; };
  EXPECT_FALSE(Synced().security_biometric_shown) << "no PIN set up yet";
}

TEST_F(SecuritySettingsBiometricTest, OnAsksForThePinFirstOffDoesNot) {
  FakeSettingsHost host(store_);
  host.UiState() = Synced();
  SettingsSecurityActions actions(host, commands_, feedback_, unlock_, pin_prompt_);

  actions.ToggleBiometric();
  ASSERT_EQ(purposes_, std::vector<PinVerifyPurpose>{PinVerifyPurpose::EnableBiometric}) << "the PIN gate first";
  EXPECT_FALSE(host.UiState().security_biometric_on) << "not before the PIN";
  EXPECT_EQ(host.pushes, 0);
  ASSERT_TRUE(on_ok_);
  enabled_ = true; // the gate's check ran enable_biometric with the PIN: the engine sealed the DEK
  on_ok_();        // and the gate closed
  EXPECT_TRUE(host.UiState().security_biometric_on);
  EXPECT_EQ(host.pushes, 1) << "the switch redrawn";

  actions.ToggleBiometric();
  EXPECT_EQ(purposes_.size(), 1U) << "off needs no PIN";
  EXPECT_EQ(disables_, 1);
  EXPECT_FALSE(host.UiState().security_biometric_on);
  EXPECT_EQ(host.pushes, 2);
  EXPECT_EQ(host.failures, 0);

  // The switch shows what the engine holds, not what was asked for (here nothing was sealed).
  actions.ToggleBiometric();
  ASSERT_TRUE(on_ok_);
  on_ok_();
  EXPECT_FALSE(host.UiState().security_biometric_on);
}

TEST_F(SecuritySettingsBiometricTest, ThePageHasTheSwitchItsHelpAndBothLanguages) {
  const std::string rml = ReadAsset("views/settings_section_security.rml");
  EXPECT_NE(rml.find("data-if=\"security_biometric_shown\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"toggle_biometric()\""), std::string::npos);
  EXPECT_NE(rml.find("data-class-settings-switch--on=\"security_biometric_on\""), std::string::npos);
  EXPECT_NE(rml.find("{{security_biometric_label}}"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.security.biometric.help}}"), std::string::npos);
  EXPECT_EQ(rml.find("biometric toggle row goes here"), std::string::npos);
  EXPECT_LT(rml.find("toggle_biometric()"), rml.find("on_choose_unlock_grace()")) << "first in the Unlock group";

  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string text = ReadAsset(locale);
    for (const char* key :
         {"settings.security.biometric.face", "settings.security.biometric.touch",
          "settings.security.biometric.fingerprint", "settings.security.biometric.help",
          "settings.security.biometric.enabled", "pin.biometric_link.face", "pin.biometric_link.touch",
          "pin.biometric_link.fingerprint", "pin.biometric_prompt", "pin.biometric_reset_hint"}) {
      EXPECT_NE(text.find(std::string("\"") + key + "\""), std::string::npos) << locale << " " << key;
    }
  }
}

} // namespace
} // namespace pbr
