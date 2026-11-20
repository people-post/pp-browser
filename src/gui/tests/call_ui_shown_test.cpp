// A call screen coming up closes the recovery phrase page (DEV/PLAN-release1-small-batch.md D). CallController only
// tells that a call screen has just come up (BindCallUiShown); these tests drive the real controller through every way a
// call screen appears and count the notifications: one per appearance, none for a repeat of the same frame, one more
// for the next call.
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/CallSystemUi.h"
#include "foundation/runtime/AppLifecycle.h"
#include "gui/CallController.h"
#include "feature/presenters/CallPresenter.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace pbr {

struct CallControllerTestAccess {
  static CallScreen& Screen(CallController& c) { return c.presenter_.ScreenForTest(); }
  static std::optional<CallFailedPage>& Failed(CallController& c) { return c.presenter_.FailedPageForTest(); }
  static void Render(CallController& c) { c.Render(); }
  static void RenderAwaitingRing(CallController& c) { c.RenderAwaitingRing(); }
  /** What RenderInCall ends with, without a live media backend behind it. */
  static void InCallSync(CallController& c) {
    c.in_call_.active = true;
    c.SyncShellState();
  }
  static void EndCall(CallController& c) {
    c.in_call_ = {};
    c.ring_ = {};
    c.SyncShellState();
  }
};

namespace {

/** The phone's own call screen: available, and a push's call is waiting, so a ring goes to it. */
class FakeSystemCallScreen final : public ICallSystemUi {
public:
  bool Available() const override { return true; }
  bool HasPendingPushCall() const override { return true; }
  void ReportIncoming(const std::string&, const std::string&, bool, bool) override { ++reported; }
  int reported = 0;
};

class CallUiShownTest : public testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
  void TearDown() override {
    controller.reset();
    ICallSystemUi::SetInstance(nullptr);
    AppLifecycle::OnDidEnterForeground();
    AppRuntime::Shutdown();
  }
  void Make(const bool system_screen) {
    if (system_screen) {
      ICallSystemUi::SetInstance(&system_ui);
    }
    controller = std::make_unique<CallController>();
    controller->BindCallUiShown([this]() { ++shown; });
  }
  void Ring(const std::string& call_id) {
    CallScreen& screen = CallControllerTestAccess::Screen(*controller);
    screen = {};
    screen.kind = CallScreen::Kind::Ringing;
    screen.ring_call_id = call_id;
    screen.caller_identity = "alice";
    CallControllerTestAccess::Render(*controller);
  }
  void Render(const CallScreen::Kind kind, const std::string& call_id = "c1") {
    CallScreen& screen = CallControllerTestAccess::Screen(*controller);
    screen = {};
    screen.kind = kind;
    screen.ring_call_id = call_id;
    screen.caller_identity = "alice";
    CallControllerTestAccess::Render(*controller);
  }

  FakeSystemCallScreen system_ui;
  std::unique_ptr<CallController> controller;
  int shown = 0;
};

// 1: a ring comes up in the app.
TEST_F(CallUiShownTest, ARingComingUpInTheAppNotifiesOnceNotOnEveryFrame) {
  Make(false);
  Ring("c1");
  EXPECT_EQ(shown, 1);
  Ring("c1");
  Ring("c1");
  EXPECT_EQ(shown, 1) << "the same ring drawn again is not a new appearance";
}

// 2: a push woke the app into the "incoming call, connecting" page.
TEST_F(CallUiShownTest, TheConnectingPlaceholderNotifies) {
  Make(false);
  CallControllerTestAccess::RenderAwaitingRing(*controller);
  EXPECT_EQ(shown, 1);
}

// 3: a tapped call alert lands on the same Ringing screen (the tap is only noted, then the Ringing branch draws).
TEST_F(CallUiShownTest, ATappedCallAlertGoesThroughTheRingBranchAndNotifiesOnce) {
  Make(false);
  controller->BindOpenThread([](const std::string&) {});
  CallScreen& screen = CallControllerTestAccess::Screen(*controller);
  screen = {};
  screen.kind = CallScreen::Kind::Ringing;
  screen.ring_call_id = "c1";
  screen.caller_identity = "alice";
  screen.thread_id = "t1";
  CallControllerTestAccess::Render(*controller);
  EXPECT_EQ(shown, 1);
}

// 4: answering (the app's button, the lock screen, CallKit, the Android notification) and then the call itself.
TEST_F(CallUiShownTest, AnsweringNotifiesAndTheCallThatFollowsDoesNotNotifyAgain) {
  Make(false);
  Render(CallScreen::Kind::Answering);
  EXPECT_EQ(shown, 1);
  CallControllerTestAccess::InCallSync(*controller);
  EXPECT_EQ(shown, 1) << "ring or answer, then the call: one appearance";
}

// 5: a call placed from here comes up as an InCall screen straight away.
TEST_F(CallUiShownTest, AnOutgoingCallScreenNotifies) {
  Make(false);
  CallControllerTestAccess::InCallSync(*controller);
  EXPECT_EQ(shown, 1);
  CallControllerTestAccess::InCallSync(*controller);
  EXPECT_EQ(shown, 1);
}

// 6: the phone's own call screen rings: the app's ring is cleared (it never shows), yet the screen has come up.
TEST_F(CallUiShownTest, ARingOnThePhonesOwnCallScreenNotifiesOncePerRing) {
  Make(true);
  AppLifecycle::OnWillEnterBackground();
  AppLifecycle::OnDidEnterBackground(); // not visible: the ring goes to the system screen
  Ring("c1");
  ASSERT_EQ(system_ui.reported, 1) << "the ring went to the phone's own call screen";
  EXPECT_EQ(shown, 1);
  Ring("c1");
  Ring("c1");
  EXPECT_EQ(shown, 1) << "the same ring on the same system screen";
  Ring("c2"); // the first ended, another rings there
  EXPECT_EQ(shown, 2);
}

// 7: a failed call's page is kept on screen like a call.
TEST_F(CallUiShownTest, TheKeptFailedPageNotifies) {
  Make(false);
  CallControllerTestAccess::Screen(*controller) = {}; // Kind::None
  CallControllerTestAccess::Failed(*controller) = CallFailedPage{};
  CallControllerTestAccess::Render(*controller);
  EXPECT_EQ(shown, 1);
}

// Edges: a second call ringing over a call does not notify again; once everything ended the next call does.
TEST_F(CallUiShownTest, ASecondCallOverACallIsNotANewAppearanceButTheNextCallAfterTheEndIs) {
  Make(false);
  CallControllerTestAccess::InCallSync(*controller);
  ASSERT_EQ(shown, 1);
  Ring("c2");
  EXPECT_EQ(shown, 1);
  CallControllerTestAccess::EndCall(*controller);
  Ring("c3");
  EXPECT_EQ(shown, 2);
}

// Hanging up (nothing on screen) notifies nothing.
TEST_F(CallUiShownTest, HangingUpNotifiesNothing) {
  Make(false);
  CallControllerTestAccess::InCallSync(*controller);
  ASSERT_EQ(shown, 1);
  CallControllerTestAccess::EndCall(*controller);
  EXPECT_EQ(shown, 1);
  Render(CallScreen::Kind::None);
  EXPECT_EQ(shown, 1);
}

} // namespace
} // namespace pbr
