// The "Link a new device" page (Kenneth 2026-11-14; the sealed QR handshake, multi-device M027): the scan and paste
// buttons, the digits check, the reply's QR code and the steps. The reply shows only after the person confirmed the
// digits (the seed is sealed then), and goes when the page is left.
#include "gui/SettingsHost.h"
#include "gui/SettingsSecurityActions.h"
#include "gui/UserFeedback.h"

#include "common/runtime/AppRuntime.h"
#include "common/tests/TeardownLikeTheApp.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/LinkHandshake.h"
#include "foundation/error/AppError.h"
#include "gui/LinkDeviceText.h"
#include "foundation/crypto/PinDefaults.h"
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
    for (const char* key : {"settings.security.link_device.scan_button", "settings.security.link_device.paste_button",
                            "settings.security.link_device.check_title", "settings.security.link_device.check_message",
                            "settings.security.link_device.check_ok", "settings.security.link_device.step_1",
                            "settings.security.link_device.step_2", "settings.security.link_device.step_3",
                            "settings.security.link_device.send_reply", "settings.security.link_device.reply_note",
                            "settings.security.link_device.new_device_hint", "settings.security.link_device.reset_hint",
                            "settings.security.link_device.copy_reply", "settings.security.link_device.reply_copied",
                            "settings.security.link_device.copied_valid", "settings.security.link_device.done",
                            "settings.security.link_device.phone_note"}) {
      EXPECT_NE(text.find(std::string("\"") + key + "\""), std::string::npos) << locale << " " << key;
    }
  }
  const std::string zh = Asset("locales/zh-Hans.json");
  const std::string en = Asset("locales/en.json");
  // The line over the steps quotes the new device's real button, in both languages.
  EXPECT_NE(zh.find("\"pin.identity_link\": \"我在别的设备上已经有账号\""), std::string::npos);
  EXPECT_NE(zh.find("选“我在别的设备上已经有账号”。\""), std::string::npos);
  EXPECT_NE(en.find("“I already have an account on another device”.\""), std::string::npos);
  EXPECT_NE(en.find("\"pin.identity_link\": \"I already have an account on another device\""), std::string::npos);
  // The new device's step 1 names the real menus: the Settings tab, the Linked devices section, the Link a new device row.
  EXPECT_NE(en.find("\"nav.me\": \"Settings\""), std::string::npos);
  EXPECT_NE(en.find("\"settings.devices.title\": \"Linked devices\""), std::string::npos);
  EXPECT_NE(en.find("\"settings.security.transfer_row\": \"Link a new device\""), std::string::npos);
  EXPECT_NE(en.find("open Settings → Linked devices → Link a new device, and scan this code"), std::string::npos);
  EXPECT_NE(zh.find("\"nav.me\": \"设置\""), std::string::npos);
  EXPECT_NE(zh.find("\"settings.devices.title\": \"关联的设备\""), std::string::npos);
  EXPECT_NE(zh.find("\"settings.security.transfer_row\": \"关联新设备\""), std::string::npos);
  EXPECT_NE(zh.find("打开 设置 → 关联的设备 → 关联新设备，扫描这个码"), std::string::npos);
  EXPECT_NE(zh.find("\"settings.security.link_device.reset_hint\": \"新设备上已有别的账号？先在“存储”里重置。\""),
            std::string::npos);
  // Three one-line steps (numbered by the page), one short help line (desktop) and one grey note; the old lead, the
  // reply box's title and the old step keys are gone.
  for (const char* orphan : {"link_device.lead\"", "link_device.reply_title\""}) {
    EXPECT_EQ(zh.find(orphan), std::string::npos) << orphan;
    EXPECT_EQ(en.find(orphan), std::string::npos) << orphan;
  }
  EXPECT_NE(zh.find("\"新设备上看不到以前的聊天记录。已有别的账号的设备要先重置。\""), std::string::npos);
  EXPECT_NE(en.find("\"Earlier chats do not appear on the new device. A device that already has another account must "
                    "be reset first.\""),
            std::string::npos);
  for (const char* gone : {"link_device.precondition", "link_device.change_phone", "link_device.copy_button",
                           "link_device.export_confirm_message", "link_device.step1", "link_device.step2",
                           "link_device.step3", "link_device.steps_head", "link_device.send_hint"}) {
    EXPECT_EQ(zh.find(gone), std::string::npos) << gone;
    EXPECT_EQ(en.find(gone), std::string::npos) << gone;
  }
}

TEST(TransferPageMarkupTest, TheScanAndPasteButtonsAndTheReplyBoxAreOnThePage) {
  const std::string rml = Asset("views/settings_security_transfer.rml");
  EXPECT_NE(rml.find("data-event-click=\"link_scan_offer()\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"link_paste_offer()\""), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"link_reply_shown\""), std::string::npos);
  EXPECT_NE(rml.find("data-rml=\"link_reply_qr_rml\""), std::string::npos) << "the reply as a QR code";
  EXPECT_NE(rml.find("data-event-click=\"copy_link_reply()\""), std::string::npos) << "and as text to copy";
  EXPECT_EQ(rml.find("{{link_reply_text"), std::string::npos) << "the text is never bound (B166)";
  // The help line and the reset hint are for the desktop only (the phone has the grey note); nothing displays the code.
  {
    const size_t hint = rml.find("{{i18n:settings.security.link_device.reset_hint}}");
    ASSERT_NE(hint, std::string::npos);
    EXPECT_NE(rml.substr(rml.rfind("<p ", hint), hint - rml.rfind("<p ", hint)).find("settings-desktop-only"),
              std::string::npos);
    EXPECT_NE(rml.find("{{i18n:settings.security.link_device.new_device_hint}}"), std::string::npos);
  }
  // The one short help line is for the desktop only; nothing displays the code.
  const size_t help = rml.find("{{i18n:settings.security.link_device.help}}");
  ASSERT_NE(help, std::string::npos);
  EXPECT_NE(rml.substr(rml.rfind("<p ", help), help - rml.rfind("<p ", help)).find("settings-desktop-only"),
            std::string::npos);
  EXPECT_EQ(rml.find("{{recovery"), std::string::npos);
  EXPECT_EQ(rml.find("{{export"), std::string::npos);
  // The grey note with its one "?" (same event as the main device's hero); the three steps over the buttons.
  const size_t note = rml.find("{{i18n:settings.security.link_device.phone_note}}");
  ASSERT_NE(note, std::string::npos);
  EXPECT_NE(rml.find("ask_setting_help('settings.security.transfer_row')", note), std::string::npos);
  EXPECT_EQ(rml.find("ask_setting_help", rml.find("ask_setting_help") + 1), std::string::npos) << "one \"?\"";
  EXPECT_NE(rml.find("settings-transfer-steps"), std::string::npos) << "the three numbered steps";
  EXPECT_NE(rml.find("data-event-click=\"send_link_reply()\""), std::string::npos) << "Send… beside Copy";
  EXPECT_NE(rml.find("data-if=\"link_can_share\""), std::string::npos) << "only where the system has a share sheet";
  // The reply ends in one main button, "Done"; the code takes its side from the page.
  EXPECT_NE(rml.find("data-style-width=\"link_qr_side\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.security.link_device.done}}"), std::string::npos);
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

class LinkReplyTest : public testing::Test {
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
    bootstrap.profile_prefs.pin_is_default = true; // the app's public PIN: no PIN step
    store.SetUiDemo(false);
    store.Initialize(std::move(bootstrap));

    commands.scan_link_offer = [this](std::function<void(std::string)> done) {
      ++scans;
      done("pp-browser-link-offer:AQ");
    };
    commands.link_offer_code = [this](const std::string&) { return code_result; };
    commands.seal_link_reply = [this](const std::string& offer) -> Roe<std::vector<uint8_t>> {
      ++seals;
      sealed_offer = offer;
      return seal_result;
    };
    commands.load_pin_protection = []() { return PinProtectionView{.ready = true, .unlocked = true}; };
    commands.verify_pin = [](const SecretText&) -> Roe<void> { return {}; };
    feedback.show_confirm = [this](const std::string&, const std::string& message, std::function<void(bool)> on_result,
                                   const std::string&) {
      ++confirms;
      confirm_message = message;
      on_confirm = std::move(on_result);
    };
    feedback.show_confirm_with_note = [this](const std::string&, const std::string& message,
                                             const std::string& note_line, const std::string& note_text,
                                             std::function<void(bool)> on_result, const std::string&) {
      ++confirms;
      ++noted_confirms;
      confirm_message = message;
      confirm_note_line = note_line;
      confirm_note_text = note_text;
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
    test::FreeLikeTheApp(actions); // it posts raw `this` onto the workers and the UI mailbox
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
  /** Scan, the digits read: the check is up. */
  void ScanToTheCheck() {
    actions->LinkDevice(/*scan=*/true);
    Settle();
  }

  SessionStore store;
  SettingsCommands commands;
  ShellFeedbackPorts feedback;
  UnlockEnsurePorts unlock;
  PinPromptPorts pin_prompt;
  std::filesystem::path root;
  std::unique_ptr<FakeHost> host;
  std::unique_ptr<SettingsSecurityActions> actions;
  Roe<std::string> code_result = std::string("0427");
  Roe<std::vector<uint8_t>> seal_result = std::vector<uint8_t>{1, 2, 3, 4};
  std::string sealed_offer;
  std::function<void(bool)> on_confirm;
  std::function<void()> on_pin_ok;
  std::vector<std::string> toasts;
  int scans = 0;
  int seals = 0;
  int confirms = 0;
  int noted_confirms = 0;
  std::string confirm_message;
  std::string confirm_note_line;
  std::string confirm_note_text;
  int pin_prompts = 0;
};

// "Scan" follows the machine's cameras: none listed, only Paste is left (and it is the main button, see the layout test).
TEST_F(LinkReplyTest, ScanIsOfferedOnlyWhileACameraIsListed) {
  bool camera = true;
  commands.can_scan_link_offer = [&camera]() { return camera; };
  actions->RefreshLinkCanScan();
  EXPECT_TRUE(actions->LinkCanScanForTest());
  camera = false;
  actions->RefreshLinkCanScan();
  EXPECT_FALSE(actions->LinkCanScanForTest()) << "no camera listed";
  actions->ClearLinkReply();
  EXPECT_FALSE(actions->LinkCanScanForTest()) << "leaving the reply keeps it";
  camera = true;
  actions->RefreshLinkCanScan();
  EXPECT_TRUE(actions->LinkCanScanForTest());
}

// With no PIN the digits check is the plain confirm: no line about a default PIN and no "?" that tells it.
TEST_F(LinkReplyTest, TheCheckWithNoPinIsAPlainConfirmWithoutANote) {
  ScanToTheCheck();
  ASSERT_EQ(confirms, 1);
  EXPECT_EQ(noted_confirms, 0);
  EXPECT_NE(confirm_message.find("0427"), std::string::npos) << "the digits to compare";
  EXPECT_TRUE(confirm_note_line.empty());
}

TEST_F(LinkReplyTest, AnOwnPinIsAskedBeforeTheCameraAndTheCheckHasNoNote) {
  SetOwnPin();
  actions->LinkDevice(/*scan=*/true);
  ASSERT_EQ(pin_prompts, 1);
  EXPECT_EQ(scans, 0) << "the PIN first";
  on_pin_ok();
  Settle();
  EXPECT_EQ(scans, 1);
  ASSERT_EQ(confirms, 1);
  EXPECT_EQ(noted_confirms, 0);
}

// M027: the seed is sealed only once the person said the new device shows the same digits.
TEST_F(LinkReplyTest, TheReplyIsSealedAndShownOnlyAfterTheDigitsMatch) {
  ScanToTheCheck();
  ASSERT_EQ(confirms, 1);
  EXPECT_EQ(seals, 0) << "nothing sealed before the check";
  EXPECT_FALSE(actions->LinkReplyShown());
  on_confirm(true);
  Settle();
  EXPECT_EQ(seals, 1);
  EXPECT_EQ(sealed_offer, "pp-browser-link-offer:AQ") << "the code that was checked";
  EXPECT_TRUE(actions->LinkReplyShown());
  EXPECT_NE(actions->LinkReplyQrForTest().find("h1v1h-1z"), std::string::npos) << "a QR code";
  actions->ClearLinkReply();
  EXPECT_FALSE(actions->LinkReplyShown());
  EXPECT_TRUE(actions->LinkReplyQrForTest().empty());
}

TEST_F(LinkReplyTest, DigitsThatDoNotMatchSealNothing) {
  ScanToTheCheck();
  ASSERT_EQ(confirms, 1);
  on_confirm(false);
  Settle();
  EXPECT_EQ(seals, 0);
  EXPECT_FALSE(actions->LinkReplyShown());
}

TEST_F(LinkReplyTest, ACheckAnsweredAfterThePageWasLeftSealsNothing) {
  ScanToTheCheck();
  actions->ClearLinkReply(); // the person left the page with the dialog still up
  on_confirm(true);
  Settle();
  EXPECT_EQ(seals, 0);
  EXPECT_FALSE(actions->LinkReplyShown());
}

TEST_F(LinkReplyTest, ACodeThatIsNotANewDevicesIsSaidSo) {
  code_result = MakeLinkError(LinkError::Unreadable, "no offer");
  ScanToTheCheck();
  EXPECT_EQ(confirms, 0);
  EXPECT_EQ(host->failures, 1);
  EXPECT_EQ(LinkErrorText(MakeLinkError(LinkError::Unreadable, "x"), false),
            Tr("settings.security.link_device.err_unreadable"));
  EXPECT_EQ(LinkErrorText(AppError::NewerFormat("x"), false), Tr("settings.security.link_device.err_newer"));
  EXPECT_EQ(LinkErrorText(Error("Only your main device can add a device"), false), "") << "the error's own words";
}

TEST_F(LinkReplyTest, AFailedSealShowsNoReply) {
  seal_result = Error("Only your main device can add a device");
  ScanToTheCheck();
  on_confirm(true);
  Settle();
  EXPECT_EQ(seals, 1);
  EXPECT_FALSE(actions->LinkReplyShown());
  EXPECT_EQ(host->failures, 1);
}

TEST_F(LinkReplyTest, TheValidityLineCarriesTheLinksMinutes) {
  ASSERT_EQ(kLinkWindowMs, 15 * 60 * 1000) << "the string below is derived from this constant";
  const std::string minutes = std::to_string(kLinkWindowMs / 60000);
  EXPECT_EQ(Tr("settings.security.link_device.copied_valid", {{"minutes", minutes}}), "Valid for 15 minutes");
}

} // namespace
} // namespace pbr
