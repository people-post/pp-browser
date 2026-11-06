// PIN grace window (#21, DEV/PLAN-pin-grace.md "设置页"): Me → Security → "Ask for PIN again after" — every time / 5 /
// 15 / 60 minutes, saved in this profile's preferences and handed to the app (set_grace_minutes); the row only shows
// with an own PIN on a platform that keeps the window. And the transfer code asks for that PIN first (plan ⑧).
#include "gui/SecuritySettingsSection.h"
#include "gui/SettingsSecurityActions.h"

#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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
  void PushToView(bool) override {}
  void SaveProfileNickname() override {}
  void RequestSyncLayout(const char*) override {}
  Roe<void> ForgetToolPermissions() override { return {}; }
  void OpenChangePin() override {}

  int failures = 0;

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

  const std::string transfer = ReadAsset("views/settings_security_transfer.rml");
  EXPECT_NE(transfer.find("data-value=\"export_pin\""), std::string::npos);
  EXPECT_NE(transfer.find("cancel_export_pin()"), std::string::npos);

  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string text = ReadAsset(locale);
    for (const char* key : {"settings.security.grace.title", "settings.security.grace.now", "settings.security.grace.5",
                            "settings.security.grace.15", "settings.security.grace.60", "settings.security.grace.help",
                            "settings.security.export_pin_prompt"}) {
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
    feedback_.show_confirm = [this](const std::string&, const std::string&, std::function<void(bool)>,
                                    const std::string&) { ++confirms_; };
  }

  ShellFeedbackPorts feedback_;
  UnlockEnsurePorts unlock_;
  int confirms_ = 0;
  int exports_ = 0;
};

TEST_F(TransferCodePinTest, AnOwnPinIsAskedForBeforeTheCode) {
  FakeSettingsHost host(store_);
  SettingsSecurityActions actions(host, commands_, feedback_, unlock_);

  actions.ExportLinkDevice();
  EXPECT_TRUE(actions.ExportPinOpen()) << "the PIN field comes out first";
  EXPECT_EQ(confirms_, 0);
  EXPECT_EQ(exports_, 0);

  actions.ExportLinkDevice();  // nothing typed
  EXPECT_EQ(host.failures, 1);
  EXPECT_EQ(confirms_, 0);
  EXPECT_EQ(exports_, 0);

  actions.CancelExportPin();
  EXPECT_FALSE(actions.ExportPinOpen());
}

TEST_F(TransferCodePinTest, TheAppDefaultPinOnlyConfirmsAsBefore) {
  SetPinIsDefault(true);
  FakeSettingsHost host(store_);
  SettingsSecurityActions actions(host, commands_, feedback_, unlock_);

  actions.ExportLinkDevice();
  EXPECT_FALSE(actions.ExportPinOpen());
  EXPECT_EQ(confirms_, 1);
  EXPECT_EQ(host.failures, 0);
}

} // namespace
} // namespace pbr
