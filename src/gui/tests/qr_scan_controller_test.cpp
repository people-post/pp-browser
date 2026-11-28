#include "gui/scan/QrScanController.h"
#include "gui/UserFeedback.h"
#include "gui/contacts/ContactsController.h"
#include "lib/messaging/ContactLocator.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

/** The OS camera permission and the UI queue, under the test's hand. */
struct FakeCamera {
  CameraPermission state = CameraPermission::NotDetermined;
  int requests = 0;
  std::function<void(bool)> answer;
  std::vector<std::function<void()>> posted;

  QrScanCameraPorts Ports() {
    return {
        .state = [this]() { return state; },
        .request =
            [this](std::function<void(bool)> done) {
              ++requests;
              answer = std::move(done);
            },
        .post_ui = [this](std::function<void()> task) { posted.push_back(std::move(task)); },
    };
  }
  void RunUi() {
    while (!posted.empty()) {
      auto tasks = std::move(posted);
      posted.clear();
      for (auto& task : tasks) {
        task();
      }
    }
  }
};

/** A scanner over a fake camera permission, counting the pages it pushes and the pills it shows. */
class QrScanPermissionTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    ShellNavigationPorts navigation;
    navigation.push_layer = [this](const PaneSpec& spec) {
      EXPECT_EQ(spec.key, "qr_scan");
      return ++pushed;
    };
    navigation.close_layer = [this](int) { ++closed; };
    scan.BindShellNavigation(std::move(navigation));
    ShellFeedbackPorts feedback;
    feedback.show_toast = [this](const std::string& message, ToastDuration) { pills.push_back(message); };
    UserFeedback::BindPorts(std::move(feedback));
  }
  void TearDown() override {
    UserFeedback::BindPorts({});
    AppLifecycle::OnDidEnterForeground();
  }
  void Open() {
    scan.Open([this](std::string) { ++delivered; });
  }
  /** The OS prompt is up: iOS resigns the app active (SDL's WILL_ENTER_BACKGROUND). */
  static void PromptShows() { AppLifecycle::OnWillEnterBackground(); }
  /** The prompt has gone: the app is active again. */
  static void PromptGoes() { AppLifecycle::OnDidEnterForeground(); }

  FakeCamera camera;
  MediaDeviceArbiter devices{CreateNullMediaDeviceBackend()};
  QrScanController scan{devices, camera.Ports()};
  int pushed = 0;
  int closed = 0;
  int delivered = 0;
  std::vector<std::string> pills;
};

// A phone lists no camera until the camera is allowed: Scan stays offered there, or the permission is never asked.
TEST(QrScanOfferedTest, APhoneAlwaysOffersScanAndADesktopFollowsTheCameraList) {
  EXPECT_TRUE(QrScanController::ScanOffered(/*desktop=*/false, /*camera_listed=*/false));
  EXPECT_TRUE(QrScanController::ScanOffered(false, true));
  EXPECT_TRUE(QrScanController::ScanOffered(/*desktop=*/true, /*camera_listed=*/true));
  EXPECT_FALSE(QrScanController::ScanOffered(true, false)) << "a computer with no camera listed";
}

TEST_F(QrScanPermissionTest, NotAskedYetAsksFirstThenAllowOpensThePageExactlyOnce) {
  Open();
  EXPECT_EQ(camera.requests, 1);
  EXPECT_EQ(pushed, 0) << "no scan page behind the system prompt";
  PromptShows();
  Open(); // a tap can't reach the app while the prompt is up; were it to, it would not ask twice
  EXPECT_EQ(camera.requests, 1);
  PromptGoes();
  ASSERT_TRUE(camera.answer);
  camera.answer(true);
  EXPECT_EQ(pushed, 0) << "the answer is handled on the UI thread";
  camera.RunUi();
  EXPECT_EQ(pushed, 1) << "Allow opens the page by itself";
  EXPECT_TRUE(scan.IsOpen());
  PromptShows(); // later trips through the background never reopen it
  PromptGoes();
  camera.RunUi();
  EXPECT_EQ(pushed, 1);
  EXPECT_TRUE(pills.empty());
  EXPECT_EQ(delivered, 0);
}

TEST_F(QrScanPermissionTest, DontAllowOpensNothingAndSaysWhereToAllowIt) {
  Open();
  PromptShows();
  PromptGoes();
  camera.answer(false);
  camera.RunUi();
  EXPECT_EQ(pushed, 0);
  EXPECT_FALSE(scan.IsOpen());
  ASSERT_EQ(pills.size(), 1u);
  EXPECT_EQ(pills[0], Tr("scan.camera_denied"));
  EXPECT_NE(pills[0], "scan.camera_denied");
}

TEST_F(QrScanPermissionTest, RefusedEarlierShowsTheHintWithoutAsking) {
  camera.state = CameraPermission::Denied;
  Open();
  EXPECT_EQ(camera.requests, 0);
  EXPECT_EQ(pushed, 0);
  ASSERT_EQ(pills.size(), 1u);
  EXPECT_EQ(pills[0], Tr("scan.camera_denied"));
}

TEST_F(QrScanPermissionTest, AlreadyAllowedOpensAtOnce) {
  camera.state = CameraPermission::Granted;
  Open();
  EXPECT_EQ(camera.requests, 0);
  EXPECT_EQ(pushed, 1);
  EXPECT_TRUE(scan.IsOpen());
  EXPECT_TRUE(camera.posted.empty());
}

TEST_F(QrScanPermissionTest, AllowAnsweredWhileStillInTheBackgroundOpensOnTheNextForeground) {
  Open();
  PromptShows();
  camera.answer(true); // iOS may hand the answer over before the app is active again
  camera.RunUi();
  EXPECT_EQ(pushed, 0) << "never pushed while not in the foreground";
  PromptGoes();
  camera.RunUi();
  EXPECT_EQ(pushed, 1) << "opened on the foreground transition, not dropped";
  EXPECT_TRUE(scan.IsOpen());
  PromptShows();
  PromptGoes();
  camera.RunUi();
  EXPECT_EQ(pushed, 1) << "once";
}

TEST_F(QrScanPermissionTest, RefusalAnsweredInTheBackgroundShowsTheHintOnForeground) {
  Open();
  PromptShows();
  camera.answer(false);
  camera.RunUi();
  EXPECT_TRUE(pills.empty());
  PromptGoes();
  camera.RunUi();
  EXPECT_EQ(pushed, 0);
  EXPECT_EQ(pills.size(), 1u);
}

TEST_F(QrScanPermissionTest, ANewerTapOpeningTheScannerCancelsTheStoredForegroundOpen) {
  Open();
  PromptShows();
  camera.answer(true); // answered in the background: the open waits for the foreground
  camera.RunUi();
  PromptGoes(); // the foreground event posts the stored open to the UI queue
  camera.state = CameraPermission::Granted;
  Open(); // before the queue runs, the user taps again and the scanner opens
  EXPECT_EQ(pushed, 1);
  camera.RunUi();
  EXPECT_EQ(pushed, 1) << "the older stored open must not present a second time";
  EXPECT_EQ(closed, 0);
  EXPECT_TRUE(scan.IsOpen());
}

TEST_F(QrScanPermissionTest, ANewerTapOpeningTheScannerCancelsTheStoredRefusalPill) {
  Open();
  PromptShows();
  camera.answer(false);
  camera.RunUi();
  PromptGoes();
  camera.state = CameraPermission::Granted;
  Open();
  camera.RunUi();
  EXPECT_EQ(pushed, 1);
  EXPECT_TRUE(pills.empty()) << "no stale denial pill after a successful open";
}

TEST_F(QrScanPermissionTest, ALatePermissionAnswerForAnAbandonedRequestDoesNothing) {
  Open(); // asks
  camera.state = CameraPermission::Granted;
  Open(); // a newer tap finds it granted and opens
  EXPECT_EQ(pushed, 1);
  camera.answer(true);
  camera.RunUi();
  EXPECT_EQ(pushed, 1);
  EXPECT_EQ(closed, 0);
}

TEST_F(QrScanPermissionTest, ARealTripToTheBackgroundStillClosesAnOpenScanner) {
  Open();
  PromptShows();
  PromptGoes();
  camera.answer(true);
  camera.RunUi();
  ASSERT_TRUE(scan.IsOpen());
  AppLifecycle::OnWillEnterBackground();
  EXPECT_FALSE(scan.IsOpen());
  EXPECT_EQ(closed, 1);
  AppLifecycle::OnDidEnterForeground();
  camera.RunUi();
  EXPECT_FALSE(scan.IsOpen()) << "coming back does not reopen it";
  EXPECT_EQ(pushed, 1);
  EXPECT_EQ(delivered, 0);
}

// The link page under the PIN gate (M027) steps aside for the camera and must come back when nothing is read: the
// request says so once — cancelled, refused, or replaced by a newer tap — and never after a text.
TEST_F(QrScanPermissionTest, ARequestThatEndsWithoutATextSaysSoOnce) {
  camera.state = CameraPermission::Granted;
  int ended = 0;
  scan.Open([this](std::string) { ++delivered; }, [&ended]() { ++ended; });
  ASSERT_TRUE(scan.IsOpen());
  scan.Close(); // Cancel
  EXPECT_EQ(ended, 1);
  scan.Close();
  EXPECT_EQ(ended, 1) << "once";

  int first = 0;
  int second = 0;
  scan.Open([this](std::string) { ++delivered; }, [&first]() { ++first; });
  scan.Open([this](std::string) { ++delivered; }, [&second]() { ++second; });
  EXPECT_EQ(first, 1) << "a newer tap ends the one before";
  EXPECT_EQ(second, 0);

  camera.state = CameraPermission::Denied;
  int refused = 0;
  scan.Open([this](std::string) { ++delivered; }, [&refused]() { ++refused; });
  EXPECT_EQ(refused, 1) << "the camera refused: nothing will be read";
  EXPECT_EQ(second, 1);
  EXPECT_EQ(delivered, 0);
}

TEST(QrScanControllerTest, CancelClosesItsLayerAndNeverDeliversAResult) {
  MediaDeviceArbiter devices(CreateNullMediaDeviceBackend());
  QrScanController scan(devices);
  int opened = 0;
  int closed = 0;
  int delivered = 0;
  std::function<void()> cancel;
  ShellNavigationPorts navigation;
  navigation.push_layer = [&](const PaneSpec& spec) {
    EXPECT_EQ(spec.key, "qr_scan");
    return ++opened;
  };
  navigation.close_layer = [&](int id) {
    EXPECT_EQ(id, opened);
    ++closed;
  };
  scan.BindShellNavigation(std::move(navigation));
  FlowCoordinatorPorts flow;
  flow.begin_modal = [&](int, std::function<bool()>, std::function<void()> on_cancel) {
    cancel = std::move(on_cancel);
  };
  scan.BindFlowCoordinator(std::move(flow));
  scan.Open([&](std::string) { ++delivered; });
  EXPECT_TRUE(scan.IsOpen());
  scan.Close();
  EXPECT_FALSE(scan.IsOpen());
  EXPECT_EQ(closed, 1);
  scan.Open([&](std::string) { ++delivered; });
  ASSERT_TRUE(cancel);
  cancel(); // FlowCoordinator closes the shell layer after notifying its owner.
  EXPECT_FALSE(scan.IsOpen());
  scan.Tick();
  EXPECT_EQ(delivered, 0);
}

TEST(QrScanControllerTest, EnteringBackgroundCancelsCaptureWithoutDeliveringText) {
  MediaDeviceArbiter devices(CreateNullMediaDeviceBackend());
  QrScanController scan(devices);
  ShellNavigationPorts navigation;
  navigation.push_layer = [](const PaneSpec&) { return 1; };
  int closed = 0;
  navigation.close_layer = [&](int) { ++closed; };
  scan.BindShellNavigation(std::move(navigation));
  int delivered = 0;
  scan.Open([&](std::string) { ++delivered; });
  EXPECT_TRUE(scan.IsOpen());
  AppLifecycle::OnWillEnterBackground();
  EXPECT_FALSE(scan.IsOpen());
  EXPECT_EQ(closed, 1);
  EXPECT_EQ(delivered, 0);
  AppLifecycle::OnDidEnterForeground();
}

TEST(QrScanControllerTest, TheHintFollowsTheErrorKindNotTheErrorText) {
  EXPECT_STREQ(ScanErrorTextKey(QrScanner::ErrorKind::PermissionDenied), "scan.camera_denied");
  EXPECT_STREQ(ScanErrorTextKey(QrScanner::ErrorKind::CameraBusy), "scan.camera_busy");
  EXPECT_STREQ(ScanErrorTextKey(QrScanner::ErrorKind::Other), "scan.camera_error");
  EXPECT_STREQ(ScanErrorTextKey(QrScanner::ErrorKind::None), "scan.camera_error");
}

TEST(ContactQrScanTest, FriendLocatorRequiresConfirmationBeforeSaving) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  ContactsController contacts;
  QrScanResultCallback decoded;
  contacts.BindScanPorts({.scan_qr = [&](QrScanResultCallback done) { decoded = std::move(done); }});
  int saves = 0;
  MessagingContactsPorts ports;
  ports.add_from_locator = [&](const std::string&, auto, auto) { ++saves; };
  contacts.BindContactsPorts(std::move(ports));
  std::function<void(bool)> confirmation;
  std::string identity_message;
  ShellFeedbackPorts feedback;
  feedback.show_confirm = [&](const std::string&, const std::string& message, std::function<void(bool)> done,
                              const std::string&) {
    identity_message = message;
    confirmation = std::move(done);
  };
  contacts.BindShellFeedback(std::move(feedback));
  contacts.OnScanQr();
  ASSERT_TRUE(decoded);
  decoded(EncodeContactLocator({.account_id = "account:alice", .nickname = "Alice"}));
  EXPECT_EQ(saves, 0);
  ASSERT_TRUE(confirmation);
  EXPECT_NE(identity_message.find("Alice"), std::string::npos);
  EXPECT_NE(identity_message.find("account:alice"), std::string::npos);
  confirmation(false);
  EXPECT_EQ(saves, 0);
  contacts.OnScanQr();
  decoded(EncodeContactLocator({.account_id = "account:alice", .nickname = "Alice"}));
  ASSERT_TRUE(confirmation);
  confirmation(true);
  EXPECT_EQ(saves, 1);
}

TEST(ContactQrScanTest, OrdinaryUrlAndMalformedContactNeverOpenOrSave) {
  ContactsController contacts;
  QrScanResultCallback decoded;
  contacts.BindScanPorts({.scan_qr = [&](QrScanResultCallback done) { decoded = std::move(done); }});
  int saves = 0;
  MessagingContactsPorts ports;
  ports.add_from_locator = [&](const std::string&, auto, auto) { ++saves; };
  contacts.BindContactsPorts(std::move(ports));
  int notices = 0;
  int confirmations = 0;
  ShellFeedbackPorts feedback;
  feedback.show_toast = [&](const std::string&, ToastDuration) { ++notices; };
  feedback.show_confirm = [&](const std::string&, const std::string&, auto, const std::string&) { ++confirmations; };
  contacts.BindShellFeedback(std::move(feedback));
  contacts.OnScanQr();
  decoded("https://example.test/");
  decoded("pp-browser-contact-v1:broken");
  EXPECT_EQ(notices, 2);
  EXPECT_EQ(confirmations, 0);
  EXPECT_EQ(saves, 0);
}

} // namespace
} // namespace pbr
