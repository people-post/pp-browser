// Me → Storage → Reset my data… on a device linked to an account but not its main one: the confirm adds one sentence
// (remove this device from the list on the main device). The main device, a signed-out device (also the "Link again"
// button's reset) and a device with no list see the dialog as before.
#include "feature/settings/SettingsCommands.h"
#include "gui/SettingsHost.h"
#include "gui/SettingsSecurityActions.h"

#include "common/runtime/AppRuntime.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

class HostStub final : public SettingsHost {
public:
  explicit HostStub(SessionStore& store) : store_(store) {}
  SessionStore& Store() override { return store_; }
  void ReportFailure(const Error&) override {}
  void ShowStatus(const std::string&) override {}
  void Resync(bool) override {}
  SettingsUiState& UiState() override { return state_; }
  void PullFromView() override {}
  void PushToView(bool) override {}
  void SaveProfileNickname() override {}
  void RequestSyncLayout(const char*) override {}
  Roe<void> ForgetToolPermissions() override { return {}; }
  void OpenChangePin() override {}

private:
  SessionStore& store_;
  SettingsUiState state_;
};

class ResetProfileLinkedHintTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }

  void SetUp() override {
    LocalizationService::Instance().SetPreferredLanguage("en");
    feedback_.show_confirm_with_checkbox = [this](const std::string&, const std::string& message, const std::string&,
                                                  bool, std::function<void(bool, bool)>,
                                                  const std::string&) { messages_.push_back(message); };
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }

  /** The confirm's message when the reset is asked with `view` as the device list (none: no list port at all). */
  std::string AskWith(const OwnDevicesView* view) {
    if (view != nullptr) {
      commands_.own_devices = [view]() -> Roe<OwnDevicesView> { return *view; };
    }
    HostStub host(store_);
    SettingsSecurityActions actions(host, commands_, feedback_, unlock_, pin_prompt_);
    actions.ResetProfile();
    EXPECT_EQ(messages_.size(), 1U);
    return messages_.empty() ? std::string() : messages_.back();
  }

  static OwnDevicesView Linked(const bool main, const bool signed_out = false) {
    OwnDevicesView view;
    view.signed_out = signed_out;
    view.this_is_main = main && !signed_out;
    view.devices.push_back({.peer_id = "main", .is_this_device = main, .is_main = true});
    view.devices.push_back({.peer_id = "other", .is_this_device = !main, .is_main = false});
    return view;
  }

  SessionStore store_;
  SettingsCommands commands_;
  ShellFeedbackPorts feedback_;
  UnlockEnsurePorts unlock_;
  PinPromptPorts pin_prompt_;
  std::vector<std::string> messages_;
};

TEST_F(ResetProfileLinkedHintTest, ALinkedNonMainDeviceIsToldToRemoveItself) {
  const std::string plain = Tr("settings.storage.reset_confirm_message");
  const OwnDevicesView view = Linked(/*main=*/false);
  const std::string message = AskWith(&view);
  EXPECT_EQ(message, plain + "\n\nAfter the reset, remove this device from the device list on your main device.");
}

TEST_F(ResetProfileLinkedHintTest, TheChineseSentenceIsThere) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  const OwnDevicesView view = Linked(/*main=*/false);
  const std::string message = AskWith(&view);
  EXPECT_NE(message.find("重置后，请在主设备的设备列表里移除这台设备。"), std::string::npos);
}

TEST_F(ResetProfileLinkedHintTest, TheMainDeviceSeesTheDialogAsBefore) {
  const OwnDevicesView view = Linked(/*main=*/true);
  EXPECT_EQ(AskWith(&view), Tr("settings.storage.reset_confirm_message"));
}

TEST_F(ResetProfileLinkedHintTest, ASignedOutDeviceSeesTheDialogAsBefore) {
  const OwnDevicesView view = Linked(/*main=*/false, /*signed_out=*/true);
  EXPECT_EQ(AskWith(&view), Tr("settings.storage.reset_confirm_message"));
}

TEST_F(ResetProfileLinkedHintTest, ADeviceWithNoListOrNoPortSeesTheDialogAsBefore) {
  OwnDevicesView empty;
  empty.this_is_main = true; // no list yet: this device would start one
  EXPECT_EQ(AskWith(&empty), Tr("settings.storage.reset_confirm_message"));
  messages_.clear();
  OwnDevicesView getting;
  getting.pending = OwnListPending::Getting;
  EXPECT_EQ(AskWith(&getting), Tr("settings.storage.reset_confirm_message"));
  messages_.clear();
  commands_.own_devices = nullptr;
  EXPECT_EQ(AskWith(nullptr), Tr("settings.storage.reset_confirm_message"));
}

} // namespace
} // namespace pbr
