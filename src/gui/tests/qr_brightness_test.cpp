// The screen is held at full brightness only while a page with one of our QR codes is up, and every way out gives it
// back (the page closes, a tab or notification clears it, the app leaves the foreground).
#include "gui/QrBrightness.h"
#include "gui/QrDisplay.h"
#include "gui/contacts/ContactsController.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
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
  const QrDisplay phone = QrDisplayFor(true, 393.f, 852.f, 3.f);
  EXPECT_EQ(phone.side_dp, 340);
  EXPECT_EQ(phone.side_px, 1020);
  EXPECT_EQ(phone.css, "340dp");
  EXPECT_EQ(QrDisplayFor(true, 360.f, 780.f, 2.f).css, "320dp");
  const QrDisplay desktop = QrDisplayFor(false, 1200.f, 800.f, 2.f);
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
    nav.push_layer = [this](const PaneSpec&) {
      open_.push_back(++last_id_);
      return last_id_;
    };
    nav.has_layer = [this](int id) { return std::find(open_.begin(), open_.end(), id) != open_.end(); };
    nav.watch_layer_closed = [this](int id, std::function<void()> closed) { watchers_.emplace(id, std::move(closed)); };
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

  /** What ShellHost::CloseLayer does for any layer that goes: remove it, then tell the watchers (once). */
  void CloseLayer(const int id) {
    open_.erase(std::remove(open_.begin(), open_.end(), id), open_.end());
    auto range = watchers_.equal_range(id);
    std::vector<std::function<void()>> run;
    for (auto it = range.first; it != range.second; ++it) {
      run.push_back(std::move(it->second));
    }
    watchers_.erase(range.first, range.second);
    for (auto& closed : run) {
      closed();
    }
  }

  ContactsController controller_;
  std::vector<int> open_;
  std::multimap<int, std::function<void()>> watchers_;
  int last_id_ = 6;
  std::function<void()> dismiss_;
};

// None of these ticks the controller: on a phone the Contacts tick does not always run (ShouldTickContacts).
TEST_F(QrPageBrightnessTest, TheQrPageHoldsTheScreenBrightUntilItsLayerIsGone) {
  ASSERT_FALSE(QrBrightness().Raised());
  controller_.OpenMyQr();
  EXPECT_TRUE(QrBrightness().Raised());
  CloseLayer(7); // a tab switch or a tapped chat's clear-out: no dismissal callback of the flow coordinator
  EXPECT_FALSE(QrBrightness().Raised());
  CloseLayer(7); // nothing more to give back
  EXPECT_FALSE(QrBrightness().Raised());
}

TEST_F(QrPageBrightnessTest, ClosingWithTheXRestoresAtOnce) {
  controller_.OpenMyQr();
  ASSERT_TRUE(QrBrightness().Raised());
  ASSERT_TRUE(dismiss_);
  dismiss_();
  EXPECT_FALSE(QrBrightness().Raised());
  CloseLayer(7); // the shell then removes the layer: the second notice changes nothing
  EXPECT_FALSE(QrBrightness().Raised());
}

TEST_F(QrPageBrightnessTest, ALayerClosedWhileAnotherPageOwnsTheFlowStillGivesTheBrightnessBack) {
  controller_.OpenMyQr();
  ASSERT_TRUE(QrBrightness().Raised());
  dismiss_ = {}; // another page began its modal: the coordinator no longer knows ours and will never call it
  CloseLayer(7);
  EXPECT_FALSE(QrBrightness().Raised());
}

TEST_F(QrPageBrightnessTest, ThePageOpenedAgainIsHeldByItsOwnLayerNotByTheOldOne) {
  controller_.OpenMyQr();
  CloseLayer(7);
  controller_.OpenMyQr(); // layer 8
  ASSERT_TRUE(QrBrightness().Raised());
  EXPECT_EQ(watchers_.count(7), 0u);
  CloseLayer(8);
  EXPECT_FALSE(QrBrightness().Raised());
}

} // namespace
} // namespace pbr
