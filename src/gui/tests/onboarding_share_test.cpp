#include "gui/onboarding/OnboardingController.h"
#include "gui/onboarding/OnboardingPorts.h"
#include "domain/people/ProfileIdentityView.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>

namespace {

pbr::OnboardingPorts RegisteredAccountPorts() {
  pbr::OnboardingPorts ports;
  ports.load_identity = []() {
    pbr::ProfileIdentityView view;
    view.ready = true;
    view.registered = "yes";
    view.nickname = "alice";
    view.relay_id = "relay:alice";
    view.account_id = "account:alice";
    return view;
  };
  ports.is_completed = []() { return false; };
  ports.mount = [](const std::string&) {};
  return ports;
}

} // namespace

// "Share my account" goes to the system sheet; where there is none (or it cannot show) the wizard
// copies the same invite text instead. The invite carries the name and the same account id that
// Me → Profile shows (the relay id).
TEST(OnboardingShareTest, FallsBackToClipboardWhenNoSheet) {
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  pbr::LocalizationService::Instance().SetPreferredLanguage("en");
  pbr::OnboardingController controller;
  pbr::OnboardingController::InstallInstance(controller);
  int share_calls = 0;
  std::string copied;
  pbr::OnboardingPorts ports = RegisteredAccountPorts();
  ports.share_text = [&](const std::string&, pbr::ShareAnchor) {
    ++share_calls;
    return false;
  };
  ports.copy_text = [&](const std::string& text) { copied = text; };
  controller.BindPorts(std::move(ports));

  controller.MaybeStart(); // registered account: the "your account" step, fields loaded
  controller.OnShare();
  EXPECT_EQ(share_calls, 1);
  EXPECT_NE(copied.find("alice"), std::string::npos) << copied;
  EXPECT_NE(copied.find("relay:alice"), std::string::npos) << copied;

  controller.BindPorts({});
  pbr::OnboardingController::ClearInstance();
}

TEST(OnboardingShareTest, SheetShownMeansNoCopy) {
  pbr::OnboardingController controller;
  pbr::OnboardingController::InstallInstance(controller);
  int copies = 0;
  pbr::OnboardingPorts ports = RegisteredAccountPorts();
  ports.share_text = [](const std::string&, pbr::ShareAnchor) { return true; };
  ports.copy_text = [&](const std::string&) { ++copies; };
  controller.BindPorts(std::move(ports));
  controller.MaybeStart();
  controller.OnShare();
  EXPECT_EQ(copies, 0);
  controller.BindPorts({});
  pbr::OnboardingController::ClearInstance();
}

TEST(OnboardingShareTest, NothingToShareWithoutAnAccount) {
  pbr::OnboardingController controller;
  pbr::OnboardingController::InstallInstance(controller);
  int share_calls = 0;
  pbr::OnboardingPorts ports;
  ports.share_text = [&](const std::string&, pbr::ShareAnchor) {
    ++share_calls;
    return true;
  };
  controller.BindPorts(std::move(ports));
  controller.OnShare();
  EXPECT_EQ(share_calls, 0);
  controller.BindPorts({});
  pbr::OnboardingController::ClearInstance();
}
