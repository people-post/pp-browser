// Settings → Security and Devices & transfer, layout and wording only (Kenneth 2026-11-14): the auto-lock options, the
// recovery-phrase row, and the phone's device rows that open an action sheet. Every action keeps its handler and its
// confirm dialog.
#include "gui/SecuritySettingsSection.h"
#include "gui/SettingsOwnDevices.h"

#include "common/runtime/AppRuntime.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "feature/settings/OwnDevices.h"

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

TEST(DevicesLayoutTest, TheThisDeviceBlockIsDisplayOnly) {
  const std::string rml = ReadAsset("views/settings_section_devices.rml");
  const size_t from = rml.find("{{i18n:settings.security.devices.this_device}}</p>");
  const size_t to = rml.find("{{i18n:settings.security.devices.other}}</p>");
  ASSERT_NE(from, std::string::npos);
  ASSERT_GT(to, from);
  const std::string block = rml.substr(from, to - from);
  EXPECT_EQ(block.find("<button"), std::string::npos);
  EXPECT_EQ(block.find("data-event-"), std::string::npos);
  EXPECT_EQ(block.find("chevron"), std::string::npos);
  EXPECT_NE(block.find("group-row--static"), std::string::npos);
}

TEST(DevicesLayoutTest, TheNewStringsExistInBothLanguages) {
  for (const char* locale : {"locales/en.json", "locales/zh-Hans.json"}) {
    const std::string text = ReadAsset(locale);
    for (const char* key : {"settings.security.devices.other", "settings.security.devices.identity",
                            "settings.security.devices.linked", "settings.security.devices.none",
                            "settings.devices.actions_help", "settings.security.grace.caption"}) {
      EXPECT_NE(text.find(std::string("\"") + key + "\""), std::string::npos) << locale << " " << key;
    }
  }
}

class SettingsOwnDevicesTest : public ::testing::Test {
protected:
  void SetUp() override {
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
      confirms_.push_back({title, ok_label, false});
      on_result_ = std::move(on_result);
    };
    feedback_.show_confirm_destructive = [this](const std::string& title, const std::string& message,
                                                std::function<void(bool)> on_result, const std::string& ok_label) {
      confirms_.push_back({title, ok_label, true});
      on_result_ = std::move(on_result);
    };
    devices_ = std::make_unique<SettingsOwnDevices>(commands_, feedback_, [this](const Error&) { ++failures_; });
  }
  void TearDown() override {
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
  };
  SettingsCommands commands_;
  ShellFeedbackPorts feedback_;
  std::unique_ptr<SettingsOwnDevices> devices_;
  std::vector<std::string> removed_;
  std::vector<std::string> made_main_;
  std::vector<Confirm> confirms_;
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

} // namespace
} // namespace pbr
