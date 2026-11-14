// Settings → Security and Devices & transfer, layout and wording only (Kenneth 2026-11-14): the auto-lock options, the
// recovery-phrase row, and the phone's device rows that open an action sheet. Every action keeps its handler and its
// confirm dialog.
#include "gui/SecuritySettingsSection.h"
#include "gui/SettingsOwnDevices.h"
#include "gui/UserFeedback.h"

#include "common/runtime/AppRuntime.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "feature/settings/OwnDevices.h"
#include "lib/people/AccountCard.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <fstream>
#include <sstream>
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

TEST(SecurityLayoutTest, AutoLockOffersTheFourOldValuesInTheOldOrderAndNotUntilRestart) {
  // Before this change the chooser listed {0, 5, 15, 60, kUnlockGraceAlways}.
  const std::vector<int> before{0, 5, 15, 60, kUnlockGraceAlways};
  std::vector<int> expected = before;
  expected.pop_back();
  EXPECT_EQ(OfferedUnlockGraceMinutes(), expected);
  for (const int minutes : OfferedUnlockGraceMinutes()) {
    EXPECT_NE(minutes, kUnlockGraceAlways);
  }
}

TEST(SecurityLayoutTest, AStoredUntilRestartStillShowsItsLabelButNothingInTheChooserIsTicked) {
  const std::string label = UnlockGraceDisplayLabel(kUnlockGraceAlways);
  EXPECT_FALSE(label.empty());
  EXPECT_NE(label, UnlockGraceDisplayLabel(15));
  int ticked = 0;
  for (const int minutes : OfferedUnlockGraceMinutes()) {
    ticked += minutes == kUnlockGraceAlways ? 1 : 0; // what the chooser does: current == minutes
  }
  EXPECT_EQ(ticked, 0);
  // Both languages carry the label of the value that is no longer offered.
  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    EXPECT_NE(ReadAsset(locale).find("\"settings.security.grace.always\""), std::string::npos) << locale;
  }
}

TEST(SecurityLayoutTest, TheRecoveryRowIsOneButtonWithTheOldEventAndAChevron) {
  const std::string rml = ReadAsset("views/settings_section_security.rml");
  const size_t event = rml.find("data-event-click=\"show_recovery_phrase()\"");
  ASSERT_NE(event, std::string::npos);
  const size_t end = rml.find("</button>", event);
  ASSERT_NE(end, std::string::npos);
  const std::string row = rml.substr(event, end - event);
  EXPECT_NE(row.find("{{i18n:settings.security.recovery.show}}"), std::string::npos);
  EXPECT_NE(row.find("group-chevron"), std::string::npos);
}

// The row of "This phone": display only, no event, no chevron, no button, no press style (group-row--static).
std::string RowOf(const std::string& rml, const std::string& title_token) {
  const size_t at = rml.find(title_token);
  EXPECT_NE(at, std::string::npos) << title_token;
  const size_t from = rml.rfind("<div class=\"group-row", at);
  const size_t to = rml.find("</div>", at);
  return rml.substr(from, to - from);
}

TEST(DevicesLayoutTest, TheThisPhoneRowsAreDisplayOnlyOnBothPages) {
  const std::string rml = ReadAsset("views/settings_section_devices.rml");
  size_t seen = 0;
  for (const char* tag :
       {"{{i18n:settings.security.devices.this_phone_main}}", "{{i18n:settings.security.devices.this_phone_tag}}"}) {
    const std::string row = RowOf(rml, tag);
    EXPECT_NE(row.find("{{i18n:settings.security.devices.this_phone}}"), std::string::npos) << tag;
    EXPECT_NE(row.find("group-row--static"), std::string::npos) << tag;
    EXPECT_EQ(row.find("<button"), std::string::npos) << tag;
    EXPECT_EQ(row.find("data-event-"), std::string::npos) << tag;
    EXPECT_EQ(row.find("chevron"), std::string::npos) << tag;
    ++seen;
  }
  EXPECT_EQ(seen, 2u);
}

TEST(DevicesLayoutTest, TheMainDevicesButtonIsAPrimaryButtonWithTheOldEventAndTheHeroSitsOnPhonesOnly) {
  const std::string rml = ReadAsset("views/settings_section_devices.rml");
  const std::string event = "data-event-click=\"open_security_page('transfer')\"";
  const size_t at = rml.find(event);
  ASSERT_NE(at, std::string::npos);
  EXPECT_EQ(rml.find(event, at + 1), std::string::npos) << "one element";
  const size_t tag = rml.rfind("<button", at);
  const size_t end = rml.find("</button>", at);
  const std::string button = rml.substr(tag, end - tag);
  EXPECT_NE(button.find("class=\"btn btn-primary\""), std::string::npos);
  EXPECT_NE(button.find("{{i18n:settings.security.transfer_row}}"), std::string::npos);
  const size_t hero =
      rml.rfind("<div class=\"settings-phone-only settings-dev-hero\" data-if=\"!own_devices_not_main\">", at);
  ASSERT_NE(hero, std::string::npos) << "same gate as the row it replaces";
  EXPECT_LT(at - hero, 1400u);
}

TEST(DevicesLayoutTest, TheDeviceLimitInTheHeroLineIsTheOneTheAccountCardEnforces) {
  ASSERT_EQ(kMaxAccountCardDevices, 5u) << "hero_sub names the number";
  EXPECT_NE(ReadAsset("locales/zh-Hans.json").find("\"最多同时 5 台设备。\""), std::string::npos);
  EXPECT_NE(ReadAsset("locales/en.json").find("\"Up to 5 devices at a time.\""), std::string::npos);
}

TEST(DevicesLayoutTest, TheNewStringsExistInBothLanguages) {
  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string text = ReadAsset(locale);
    for (const char* key : {"settings.security.devices.none", "settings.security.devices.this_phone",
                            "settings.security.devices.this_phone_main", "settings.security.devices.this_phone_tag",
                            "settings.security.devices.hero", "settings.security.devices.hero_sub",
                            "settings.security.devices.tap_hint", "settings.security.devices.view_head",
                            "settings.security.devices.linked_note", "settings.security.devices.linked_note_sub",
                            "settings.security.link_device.phone_note", "settings.security.grace.caption"}) {
      EXPECT_NE(text.find(std::string("\"") + key + "\""), std::string::npos) << locale << " " << key;
    }
  }
  const std::string zh = ReadAsset("locales/zh-Hans.json");
  for (const char* line :
       {"\"在其他设备上使用同一个账号\"", "\"本机 · 主设备\"", "\"点一台设备可以移除，或设为主设备。\"",
        "\"这个账号的设备\"", "\"这台手机已关联到你的账号。\"", "\"要管理设备，请到主设备上操作。\"",
        "\"还没有关联其他设备。\"", "\"新设备上看不到以前的聊天记录。已有别的账号的设备要先重置。\""}) {
    EXPECT_NE(zh.find(line), std::string::npos) << line;
  }
}

class SettingsOwnDevicesTest : public ::testing::Test {
protected:
  // Each test can run alone (ctest runs one per process): load the words it compares against.
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }

  void SetUp() override {
    LocalizationService::Instance().SetPreferredLanguage("en");
    AppRuntime::Initialize();
    commands_.remove_own_device = [this](const std::string& peer_id) -> Roe<void> {
      removed_.push_back(peer_id);
      return {};
    };
    commands_.make_main_device = [this](const std::string& peer_id) -> Roe<void> {
      made_main_.push_back(peer_id);
      return {};
    };
    commands_.own_devices = [this]() -> Roe<OwnDevicesView> { return View(); };
    feedback_.show_confirm = [this](const std::string& title, const std::string& message,
                                    std::function<void(bool)> on_result, const std::string& ok_label) {
      confirms_.push_back({title, ok_label, false, message});
      on_result_ = std::move(on_result);
    };
    feedback_.show_confirm_destructive = [this](const std::string& title, const std::string& message,
                                                std::function<void(bool)> on_result, const std::string& ok_label) {
      confirms_.push_back({title, ok_label, true, message});
      on_result_ = std::move(on_result);
    };
    ShellFeedbackPorts shared;
    shared.show_toast = [this](const std::string& text, ToastDuration) { notes_.push_back(text); };
    UserFeedback::BindPorts(shared);
    devices_ = std::make_unique<SettingsOwnDevices>(commands_, feedback_, [this](const Error&) { ++failures_; });
  }
  void TearDown() override {
    UserFeedback::BindPorts({});
    devices_.reset();
    AppRuntime::Shutdown();
  }

  OwnDevicesView View() const {
    // This device (main) first, then two others.
    return OwnDevicesViewFor({"self-peer", "other-peer-aaaaaa", "other-peer-bbbbbb"}, "self-peer", false);
  }
  void Settle() {
    AppRuntime::DrainWorkersThenUI();
    AppRuntime::RunUITasks();
  }

  struct Confirm {
    std::string title;
    std::string ok_label;
    bool destructive;
    std::string message;
  };
  SettingsCommands commands_;
  ShellFeedbackPorts feedback_;
  std::unique_ptr<SettingsOwnDevices> devices_;
  std::vector<std::string> removed_;
  std::vector<std::string> made_main_;
  std::vector<Confirm> confirms_;
  std::vector<std::string> notes_;
  std::function<void(bool)> on_result_;
  int failures_ = 0;
};

TEST_F(SettingsOwnDevicesTest, TheSheetOffersTheTwoActionsOnlyForADeviceTheMainOneManages) {
  devices_->Show(View());
  EXPECT_TRUE(devices_->DeviceActions(0).empty()) << "this device";
  EXPECT_TRUE(devices_->DeviceActions(7).empty());
  const auto actions = devices_->DeviceActions(1);
  ASSERT_EQ(actions.size(), 3U);
  EXPECT_EQ(actions[0].label, Tr("settings.security.devices.make_main"));
  EXPECT_EQ(actions[1].label, Tr("settings.security.devices.remove"));
  EXPECT_TRUE(actions[1].danger);
  EXPECT_TRUE(actions[2].cancel);

  // A linked device (not main) manages nothing: no sheet, as the buttons were hidden before.
  devices_->Show(OwnDevicesViewFor({"main-peer", "self-peer", "other-peer-cccccc"}, "self-peer", false));
  EXPECT_TRUE(devices_->DeviceActions(2).empty());
  EXPECT_TRUE(devices_->DeviceActions(0).empty());
}

TEST_F(SettingsOwnDevicesTest, MakeMainInTheSheetAsksTheOldQuestionAndCallsTheOldCommand) {
  devices_->Show(View());
  auto actions = devices_->DeviceActions(2);
  actions[0].run();
  ASSERT_EQ(confirms_.size(), 1U);
  EXPECT_EQ(confirms_[0].title, Tr("settings.security.devices.make_main_confirm_title"));
  EXPECT_EQ(confirms_[0].ok_label, Tr("settings.security.devices.make_main_confirm_ok"));
  EXPECT_FALSE(confirms_[0].destructive);
  EXPECT_TRUE(made_main_.empty()) << "nothing before the answer";
  on_result_(false);
  Settle();
  EXPECT_TRUE(made_main_.empty());
  actions[0].run();
  on_result_(true);
  Settle();
  EXPECT_EQ(made_main_, std::vector<std::string>{"other-peer-bbbbbb"});
  EXPECT_TRUE(removed_.empty());
}

TEST_F(SettingsOwnDevicesTest, RemoveInTheSheetAsksTheOldQuestionAndCallsTheOldCommand) {
  devices_->Show(View());
  auto actions = devices_->DeviceActions(1);
  actions[1].run();
  ASSERT_EQ(confirms_.size(), 1U);
  EXPECT_EQ(confirms_[0].title, Tr("settings.security.devices.remove_confirm_title"));
  EXPECT_EQ(confirms_[0].ok_label, Tr("settings.security.devices.remove_confirm_ok"));
  EXPECT_TRUE(confirms_[0].destructive);
  on_result_(true);
  Settle();
  EXPECT_EQ(removed_, std::vector<std::string>{"other-peer-aaaaaa"});
  EXPECT_TRUE(made_main_.empty());
}

TEST_F(SettingsOwnDevicesTest, ConfirmNamesTheDeviceForBothActions) {
  devices_->Show(View());
  const std::string label = View().devices[2].label;
  ASSERT_FALSE(label.empty());
  devices_->DeviceActions(2)[0].run();
  devices_->DeviceActions(2)[1].run();
  ASSERT_EQ(confirms_.size(), 2U);
  EXPECT_NE(confirms_[0].message.find(label), std::string::npos) << confirms_[0].message;
  EXPECT_NE(confirms_[1].message.find(label), std::string::npos) << confirms_[1].message;
}

TEST_F(SettingsOwnDevicesTest, ASheetActsOnItsDeviceAfterTheListShiftsUnderIt) {
  devices_->Show(View());
  auto actions = devices_->DeviceActions(2); // B = other-peer-bbbbbb, position 2
  // A was removed: B is now at position 1.
  devices_->Show(OwnDevicesViewFor({"self-peer", "other-peer-bbbbbb"}, "self-peer", false));
  actions[1].run();
  ASSERT_EQ(confirms_.size(), 1U);
  on_result_(true);
  Settle();
  EXPECT_EQ(removed_, std::vector<std::string>{"other-peer-bbbbbb"});
  actions[0].run();
  on_result_(true);
  Settle();
  EXPECT_EQ(made_main_, std::vector<std::string>{"other-peer-bbbbbb"});
  for (const std::string& note : notes_) {
    EXPECT_NE(note, Tr("settings.security.devices.list_changed"));
  }
}

TEST_F(SettingsOwnDevicesTest, ASheetForAGoneDeviceDoesNothingButSayTheListChanged) {
  devices_->Show(View());
  auto actions = devices_->DeviceActions(2);
  devices_->Show(OwnDevicesViewFor({"self-peer", "other-peer-aaaaaa"}, "self-peer", false));
  actions[1].run();
  actions[0].run();
  EXPECT_TRUE(confirms_.empty());
  EXPECT_EQ(notes_, (std::vector<std::string>{Tr("settings.security.devices.list_changed"),
                                              Tr("settings.security.devices.list_changed")}));
  Settle();
  EXPECT_TRUE(removed_.empty());
  EXPECT_TRUE(made_main_.empty());
}

TEST_F(SettingsOwnDevicesTest, ADeviceThatVanishesWhileTheConfirmIsOpenIsNotTouched) {
  devices_->Show(View());
  auto actions = devices_->DeviceActions(2);
  actions[1].run();
  ASSERT_EQ(confirms_.size(), 1U);
  devices_->Show(OwnDevicesViewFor({"self-peer", "other-peer-aaaaaa"}, "self-peer", false));
  on_result_(true);
  Settle();
  EXPECT_TRUE(removed_.empty());
  EXPECT_EQ(notes_.size(), 1U);
  // Same for make main.
  devices_->Show(View());
  actions = devices_->DeviceActions(2);
  actions[0].run();
  devices_->Show(OwnDevicesViewFor({"self-peer", "other-peer-aaaaaa"}, "self-peer", false));
  on_result_(true);
  Settle();
  EXPECT_TRUE(made_main_.empty());
  EXPECT_EQ(notes_.size(), 2U);
}

TEST_F(SettingsOwnDevicesTest, CancellingEitherConfirmIssuesNothing) {
  devices_->Show(View());
  auto actions = devices_->DeviceActions(1);
  actions[1].run();
  on_result_(false);
  actions[0].run();
  on_result_(false);
  Settle();
  EXPECT_TRUE(removed_.empty());
  EXPECT_TRUE(made_main_.empty());
  EXPECT_TRUE(notes_.empty());
}

TEST(DevicesLayoutTest, ASignedOutDeviceShowsOnlyItsNoticeAmongTheDeviceBlocks) {
  const std::string rml = ReadAsset("views/settings_section_devices.rml");
  // Every phone block of the list carries a gate that excludes a signed-out device (which is also "not main").
  for (const char* gate : {"data-if=\"own_devices_shown && !own_devices_not_main\"",
                           "data-if=\"own_devices_not_main && !own_devices_signed_out\""}) {
    EXPECT_NE(rml.find(gate), std::string::npos) << gate;
  }
  EXPECT_NE(rml.find("data-if=\"own_devices_signed_out\""), std::string::npos);
  EXPECT_GT(rml.find("data-if=\"own_devices_signed_out\""), rml.find("{{i18n:settings.security.devices.none}}"));
}

} // namespace
} // namespace pbr
