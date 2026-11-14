// The "Link a new device" page, layout and wording only (Kenneth 2026-11-14): the copy button, the "copied" state and
// the steps. The export itself (PIN gate, confirm, copy, toast) is unchanged; here the view flag its success path sets is
// pinned (set by success only, cleared when the page is left).
#include "gui/SettingsHost.h"
#include "gui/SettingsSecurityActions.h"
#include "gui/UserFeedback.h"

#include "common/runtime/AppRuntime.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pbr {
namespace {

std::string Asset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

TEST(TransferPageMarkupTest, TheNewStringsExistInBothLanguagesAndQuoteTheRealOnes) {
  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string text = Asset(locale);
    for (const char* key : {"settings.security.link_device.copy_button", "settings.security.link_device.copy_again",
                            "settings.security.link_device.lead", "settings.security.link_device.copied_valid",
                            "settings.security.link_device.steps_head", "settings.security.link_device.step1",
                            "settings.security.link_device.step2", "settings.security.link_device.step3",
                            "settings.security.link_device.phone_note"}) {
      EXPECT_NE(text.find(std::string("\"") + key + "\""), std::string::npos) << locale << " " << key;
    }
  }
  const std::string zh = Asset("locales/zh-Hans.json");
  const std::string en = Asset("locales/en.json");
  // The steps quote the new device's real strings.
  EXPECT_NE(zh.find("\"pin.identity_link\": \"我在别的设备上已经有账号\""), std::string::npos);
  EXPECT_NE(zh.find("选“我在别的设备上已经有账号”\""), std::string::npos);
  EXPECT_NE(en.find("“I already have an account on another device”\""), std::string::npos);
  // Three short steps, then one grey line; the old long paragraphs are gone.
  EXPECT_NE(zh.find("\"settings.security.link_device.step2\": \"设一个 PIN\""), std::string::npos);
  EXPECT_NE(zh.find("\"settings.security.link_device.step3\": \"粘贴关联码\""), std::string::npos);
  EXPECT_NE(zh.find("\"新设备上看不到以前的聊天记录。已有别的账号的设备要先重置。\""), std::string::npos);
  EXPECT_NE(en.find("\"Earlier chats do not appear on the new device. A device that already has another account must "
                    "be reset first.\""),
            std::string::npos);
  for (const char* gone : {"link_device.precondition", "link_device.change_phone"}) {
    EXPECT_EQ(zh.find(gone), std::string::npos) << gone;
    EXPECT_EQ(en.find(gone), std::string::npos) << gone;
  }
}

TEST(TransferPageMarkupTest, TheCopyButtonAndTheCopiedBoxFireTheOldEventAndTheOldTextIsGoneOnPhones) {
  const std::string rml = Asset("views/settings_security_transfer.rml");
  EXPECT_NE(rml.find("class=\"btn btn-primary settings-transfer-btn\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.security.link_device.copy_button}}"), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"transfer_code_copied\""), std::string::npos);
  // The three old captions stay for the desktop only; nothing displays the code.
  for (const char* key : {"link_device.help", "link_device.reset_hint"}) {
    const size_t at = rml.find(std::string("{{i18n:settings.security.") + key);
    ASSERT_NE(at, std::string::npos);
    const size_t open = rml.rfind("<p ", at);
    EXPECT_NE(rml.substr(open, at - open).find("settings-desktop-only"), std::string::npos) << key;
  }
  EXPECT_EQ(rml.find("{{recovery"), std::string::npos);
  EXPECT_EQ(rml.find("{{export"), std::string::npos);
  // The phone page's end: the grey note with its one "?" (same event as the main device's hero), no precondition, no
  // "one device at a time" line and no change-phones sentence; send_hint stays on the desktop only.
  const size_t note = rml.find("{{i18n:settings.security.link_device.phone_note}}");
  ASSERT_NE(note, std::string::npos);
  EXPECT_NE(rml.find("ask_setting_help('settings.security.transfer_row')", note), std::string::npos);
  EXPECT_EQ(rml.find("ask_setting_help", rml.find("ask_setting_help") + 1), std::string::npos) << "one \"?\"";
  const size_t send = rml.find("link_device.send_hint");
  ASSERT_NE(send, std::string::npos);
  EXPECT_EQ(rml.find("link_device.send_hint", send + 1), std::string::npos) << "desktop only";
  EXPECT_NE(rml.substr(rml.rfind("<p ", send), send - rml.rfind("<p ", send)).find("settings-desktop-only"),
            std::string::npos);
}

// --- the "copied" flag --------------------------------------------------------------------------------------------

class FakeHost final : public SettingsHost {
public:
  explicit FakeHost(SessionStore& store) : store_(store) {}
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

class TransferCopiedFlagTest : public testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    root = std::filesystem::temp_directory_path() /
           ("pp_transfer_flag_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "profile");
    BootstrapResult bootstrap{};
    bootstrap.config = Config::DefaultAppConfig();
    bootstrap.config_path = (root / "config.json").string();
    bootstrap.data_dir = root.string();
    bootstrap.profile_data_dir = (root / "profile").string();
    bootstrap.profile_prefs = UserPreferences::DefaultProfile();
    bootstrap.profile_prefs.pin_is_default = true; // the app's public PIN: the export only confirms
    store.SetUiDemo(false);
    store.Initialize(std::move(bootstrap));

    commands.export_link_device = [this]() -> Roe<std::string> {
      ++exports;
      return export_result;
    };
    commands.load_pin_protection = []() { return PinProtectionView{.ready = true, .unlocked = true}; };
    commands.verify_pin = [](const SecretText&) -> Roe<void> { return {}; };
    feedback.show_confirm = [this](const std::string&, const std::string&, std::function<void(bool)> on_result,
                                   const std::string&) {
      ++confirms;
      on_confirm = std::move(on_result);
    };
    pin_prompt.show_verify = [this](const PinVerifyPurpose, PinGateCheck, std::function<void()> on_ok) {
      ++pin_prompts;
      on_pin_ok = std::move(on_ok);
    };
    ShellFeedbackPorts toast;
    toast.show_toast = [this](const std::string& text, ToastDuration) { toasts.push_back(text); };
    UserFeedback::BindPorts(toast);
    host = std::make_unique<FakeHost>(store);
    actions = std::make_unique<SettingsSecurityActions>(*host, commands, feedback, unlock, pin_prompt);
  }
  void TearDown() override {
    actions.reset();
    UserFeedback::BindPorts({});
    AppRuntime::Shutdown();
    std::filesystem::remove_all(root);
  }
  void SetOwnPin() {
    ProfilePreferences prefs = store.Snapshot().profile_prefs;
    prefs.pin_is_default = false;
    ASSERT_TRUE(static_cast<bool>(store.SaveProfilePrefs(prefs)));
  }
  void Settle() {
    AppRuntime::DrainWorkersThenUI();
    AppRuntime::RunUITasks();
  }

  SessionStore store;
  SettingsCommands commands;
  ShellFeedbackPorts feedback;
  UnlockEnsurePorts unlock;
  PinPromptPorts pin_prompt;
  std::filesystem::path root;
  std::unique_ptr<FakeHost> host;
  std::unique_ptr<SettingsSecurityActions> actions;
  Roe<std::string> export_result = std::string("code");
  std::function<void(bool)> on_confirm;
  std::function<void()> on_pin_ok;
  std::vector<std::string> toasts;
  int exports = 0;
  int confirms = 0;
  int pin_prompts = 0;
};

TEST_F(TransferCopiedFlagTest, SuccessSetsTheFlagAndLeavingThePageClearsIt) {
  EXPECT_FALSE(actions->TransferCodeCopied());
  actions->ExportLinkDevice();
  ASSERT_EQ(confirms, 1);
  EXPECT_FALSE(actions->TransferCodeCopied()) << "not before the confirm";
  on_confirm(true);
  Settle();
  EXPECT_EQ(exports, 1);
  EXPECT_TRUE(actions->TransferCodeCopied());
  EXPECT_EQ(toasts.size(), 1u) << "the toast is still shown";
  actions->ClearTransferCopied();
  EXPECT_FALSE(actions->TransferCodeCopied());
}

TEST_F(TransferCopiedFlagTest, ACancelledConfirmDoesNotSetTheFlag) {
  actions->ExportLinkDevice();
  ASSERT_EQ(confirms, 1);
  on_confirm(false);
  Settle();
  EXPECT_EQ(exports, 0);
  EXPECT_FALSE(actions->TransferCodeCopied());
}

TEST_F(TransferCopiedFlagTest, ACancelledPinDoesNotSetTheFlag) {
  SetOwnPin();
  actions->ExportLinkDevice();
  ASSERT_EQ(pin_prompts, 1) << "an own PIN is asked first";
  EXPECT_EQ(confirms, 0);
  Settle();
  EXPECT_FALSE(actions->TransferCodeCopied()) << "the gate was dismissed: nothing else happens";
  // The PIN accepted, the confirm cancelled: still no flag; accepted and confirmed: the flag.
  on_pin_ok();
  ASSERT_EQ(confirms, 1);
  on_confirm(false);
  Settle();
  EXPECT_FALSE(actions->TransferCodeCopied());
  on_pin_ok();
  on_confirm(true);
  Settle();
  EXPECT_TRUE(actions->TransferCodeCopied());
}

TEST_F(TransferCopiedFlagTest, AFailedExportDoesNotSetTheFlag) {
  export_result = Error("Only your main device can add a device");
  actions->ExportLinkDevice();
  on_confirm(true);
  Settle();
  EXPECT_EQ(exports, 1);
  EXPECT_FALSE(actions->TransferCodeCopied());
  EXPECT_EQ(host->failures, 1);
}

TEST_F(TransferCopiedFlagTest, TheValidityLinesCarryTheNumberOfMinutesOfTheCodesOwnLifetime) {
  ASSERT_EQ(kLinkDeviceDefaultTtlMs, 15 * 60 * 1000) << "the strings below are derived from this constant";
  // Not bound yet, so read the strings the way the page does: the helper formats {minutes}.
  const std::string minutes = std::to_string(kLinkDeviceDefaultTtlMs / 60000);
  EXPECT_EQ(Tr("settings.security.link_device.copied_valid", {{"minutes", minutes}}), "Valid for 15 minutes");
  EXPECT_EQ(Tr("settings.security.link_device.lead", {{"minutes", minutes}}), "The link code is valid for 15 minutes.");
}

} // namespace
} // namespace pbr
