// The screen is held at full brightness only while a page with one of our QR codes is up, and every way out gives it
// back (the page closes, a tab or notification clears it, the app leaves the foreground).
#include "gui/QrBrightness.h"
#include "gui/QrDisplay.h"
#include "gui/contacts/ContactsController.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

struct FakeScreen {
  std::vector<bool> calls;
  bool visible = true;
  QrBrightnessHold Hold() {
    return QrBrightnessHold([this](bool on) { calls.push_back(on); }, [this] { return visible; });
  }
};

TEST(QrBrightnessHoldTest, RaisedOnceOnShowAndRestoredOnClose) {
  FakeScreen screen;
  auto hold = screen.Hold();
  hold.Want(QrBrightnessHold::kMyQr, true);
  hold.Want(QrBrightnessHold::kMyQr, true); // each frame says it again: no second write
  EXPECT_EQ(screen.calls, (std::vector<bool>{true}));
  hold.Want(QrBrightnessHold::kMyQr, false);
  hold.Want(QrBrightnessHold::kMyQr, false);
  EXPECT_EQ(screen.calls, (std::vector<bool>{true, false}));
}

TEST(QrBrightnessHoldTest, GoingToTheBackgroundRestoresAndComingBackRaisesAgainWhileThePageIsUp) {
  FakeScreen screen;
  auto hold = screen.Hold();
  hold.Want(QrBrightnessHold::kGroupLink, true);
  screen.visible = false;
  hold.Refresh();
  EXPECT_FALSE(hold.Raised());
  hold.Refresh();
  screen.visible = true;
  hold.Refresh();
  EXPECT_TRUE(hold.Raised());
  EXPECT_EQ(screen.calls, (std::vector<bool>{true, false, true}));
}

TEST(QrBrightnessHoldTest, AClosedPageDoesNotRaiseAfterTheAppReturns) {
  FakeScreen screen;
  auto hold = screen.Hold();
  hold.Want(QrBrightnessHold::kMyQr, true);
  screen.visible = false;
  hold.Refresh();
  hold.Want(QrBrightnessHold::kMyQr, false); // closed while away
  screen.visible = true;
  hold.Refresh();
  EXPECT_EQ(screen.calls, (std::vector<bool>{true, false}));
}

TEST(QrBrightnessHoldTest, TwoPagesHoldItTogetherAndTheLastToLeaveRestores) {
  FakeScreen screen;
  auto hold = screen.Hold();
  hold.Want(QrBrightnessHold::kMyQr, true);
  hold.Want(QrBrightnessHold::kGroupLink, true);
  hold.Want(QrBrightnessHold::kMyQr, false);
  EXPECT_TRUE(hold.Raised());
  hold.Want(QrBrightnessHold::kGroupLink, false);
  EXPECT_EQ(screen.calls, (std::vector<bool>{true, false}));
}

TEST(QrDisplayTest, PhoneIsPageWideMinusMarginsDesktopKeepsItsOwnSize) {
  const QrDisplay phone = QrDisplayFor(true, 393.f, 3.f);
  EXPECT_EQ(phone.side_dp, 340);
  EXPECT_EQ(phone.side_px, 1020);
  EXPECT_EQ(phone.css, "340dp");
  EXPECT_EQ(QrDisplayFor(true, 360.f, 2.f).css, "320dp");
  const QrDisplay desktop = QrDisplayFor(false, 1200.f, 2.f);
  EXPECT_EQ(desktop.side_dp, 200);
  EXPECT_EQ(desktop.css, "200dp");
}

class QrPageBrightnessTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }
  void SetUp() override {
    QrBrightness().Want(QrBrightnessHold::kMyQr, false); // other fixtures leave their QR page open
    LocalizationService::Instance().SetPreferredLanguage("en");
    ShellNavigationPorts nav;
    nav.push_layer = [](const PaneSpec&) { return 7; };
    nav.has_layer = [this](int) { return layer_up_; };
    controller_.BindShellNavigation(nav);
    FlowCoordinatorPorts flow;
    flow.begin_modal = [this](int, std::function<bool()>, std::function<void()> on_cancel) {
      dismiss_ = std::move(on_cancel);
    };
    controller_.BindFlowCoordinator(flow);
    ContactsAccountPorts ports;
    ports.load_identity = []() {
      ProfileIdentityView view;
      view.ready = true;
      view.account_id = "account:me";
      view.nickname = "Me";
      view.contact_locator = "mine-locator";
      return view;
    };
    controller_.BindAccountPorts(ports);
  }
  void TearDown() override { QrBrightness().Want(QrBrightnessHold::kMyQr, false); }

  ContactsController controller_;
  bool layer_up_ = true;
  std::function<void()> dismiss_;
};

TEST_F(QrPageBrightnessTest, TheQrPageHoldsTheScreenBrightUntilItsLayerIsGone) {
  ASSERT_FALSE(QrBrightness().Raised());
  controller_.OpenMyQr();
  EXPECT_TRUE(QrBrightness().Raised());
  controller_.Tick();
  EXPECT_TRUE(QrBrightness().Raised());
  layer_up_ = false; // a tab switch or a notification cleared the layer without calling the dismissal
  controller_.Tick();
  EXPECT_FALSE(QrBrightness().Raised());
}

TEST_F(QrPageBrightnessTest, ClosingWithTheXRestoresAtOnce) {
  controller_.OpenMyQr();
  ASSERT_TRUE(QrBrightness().Raised());
  ASSERT_TRUE(dismiss_);
  dismiss_();
  EXPECT_FALSE(QrBrightness().Raised());
}

} // namespace
} // namespace pbr
