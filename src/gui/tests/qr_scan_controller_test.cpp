#include "gui/scan/QrScanController.h"
#include "gui/contacts/ContactsController.h"
#include "lib/messaging/ContactLocator.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

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
  navigation.close_layer = [&](int id) { EXPECT_EQ(id, opened); ++closed; };
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

TEST(ContactQrScanTest, FriendLocatorRequiresConfirmationBeforeSaving) {
  ContactsController contacts;
  QrScanResultCallback decoded;
  contacts.BindScanPorts({.scan_qr = [&](QrScanResultCallback done) { decoded = std::move(done); }});
  int saves = 0;
  MessagingContactsPorts ports;
  ports.add_from_locator = [&](const std::string&, auto) { ++saves; };
  contacts.BindContactsPorts(std::move(ports));
  std::function<void(bool)> confirmation;
  ShellFeedbackPorts feedback;
  feedback.show_confirm = [&](const std::string&, const std::string&, std::function<void(bool)> done,
                              const std::string&) { confirmation = std::move(done); };
  contacts.BindShellFeedback(std::move(feedback));
  contacts.OnScanQr();
  ASSERT_TRUE(decoded);
  decoded(EncodeContactLocator({.account_id = "account:alice", .nickname = "Alice"}));
  EXPECT_EQ(saves, 0);
  ASSERT_TRUE(confirmation);
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
  ports.add_from_locator = [&](const std::string&, auto) { ++saves; };
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
