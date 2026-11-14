// Me → Security → Your devices (M021 d4): the first device is the main one; only it may remove or rename another
// device or hand on the role; a signed-out device manages nothing. Each device shows its name, never a piece of its
// PeerId.
#include "feature/settings/OwnDevices.h"

#include "common/Logger.h"
#include "common/Metrics.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

void LoadLocales() {
#ifdef PP_BROWSER_ASSETS_DIR
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
  pbr::LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace

TEST(OwnDevicesTest, MainDeviceManagesTheOthers) {
  LoadLocales();
  const pbr::OwnDevicesView view = pbr::OwnDevicesViewFor(
      {"12D3KooSelf000001", "12D3KooOtherABCDEF", "12D3KooThird123456"}, "12D3KooSelf000001", false);
  EXPECT_TRUE(view.this_is_main);
  EXPECT_FALSE(view.signed_out);
  ASSERT_EQ(view.devices.size(), 3u);

  EXPECT_TRUE(view.devices[0].is_main);
  EXPECT_TRUE(view.devices[0].is_this_device);
  EXPECT_FALSE(view.devices[0].can_manage); // never itself
  EXPECT_EQ(view.devices[0].name, "New device") << "no name yet";
  EXPECT_FALSE(view.devices[0].named);

  EXPECT_FALSE(view.devices[1].is_main);
  EXPECT_FALSE(view.devices[1].is_this_device);
  EXPECT_TRUE(view.devices[1].can_manage);
  EXPECT_EQ(view.devices[1].peer_id, "12D3KooOtherABCDEF");
  EXPECT_EQ(view.devices[1].name, "New device");

  EXPECT_TRUE(view.devices[2].can_manage);
  EXPECT_EQ(view.devices[2].name, "New device");
}

TEST(OwnDevicesTest, DevicesShowTheirNamesAndNeverAPieceOfAnId) {
  LoadLocales();
  const pbr::OwnDevicesView view = pbr::OwnDevicesViewFor(
      {"12D3KooSelf000001", "12D3KooOtherABCDEF", "12D3KooThird123456"}, "12D3KooSelf000001", false,
      {{"12D3KooSelf000001", "My iPhone"}, {"12D3KooOtherABCDEF", "Office Mac"}, {"12D3KooGone999999", "Not listed"}});
  ASSERT_EQ(view.devices.size(), 3u) << "a name for a device the card does not list is not shown";
  EXPECT_EQ(view.devices[0].name, "My iPhone");
  EXPECT_TRUE(view.devices[0].named);
  EXPECT_EQ(view.devices[1].name, "Office Mac");
  EXPECT_EQ(view.devices[2].name, "New device");
  EXPECT_FALSE(view.devices[2].named);
  for (const pbr::OwnDeviceView& device : view.devices) {
    EXPECT_EQ(device.name.find("ABCDEF"), std::string::npos);
    EXPECT_EQ(device.name.find("123456"), std::string::npos);
    EXPECT_EQ(device.name.find("Device "), std::string::npos);
  }
}

TEST(OwnDevicesTest, ANameIsCleanedAgainAndAnEmptyOneIsNoName) {
  LoadLocales();
  const pbr::OwnDevicesView view =
      pbr::OwnDevicesViewFor({"self", "other", "third"}, "self", false,
                             {{"self", "  Phone\n(old) "}, {"other", " \t\n "}, {"third", "Name {device} {id}"}});
  EXPECT_EQ(view.devices[0].name, "Phone(old)");
  EXPECT_EQ(view.devices[1].name, "New device");
  EXPECT_EQ(view.devices[2].name, "Name {device} {id}") << "a name is a value, not a template";
}

TEST(OwnDevicesTest, ALinkedDeviceSeesTheMainDevicesName) {
  LoadLocales();
  const pbr::OwnDevicesView view =
      pbr::OwnDevicesViewFor({"main", "self", "third"}, "self", false, {{"main", "Home iPhone"}, {"self", "Mac"}});
  EXPECT_TRUE(view.devices[0].is_main);
  EXPECT_EQ(view.devices[0].name, "Home iPhone");
  EXPECT_TRUE(view.devices[0].named);
  EXPECT_TRUE(view.devices[1].is_this_device);
  EXPECT_EQ(view.devices[1].name, "Mac");
  EXPECT_FALSE(view.devices[2].is_main || view.devices[2].is_this_device);
}

namespace {
struct Collect : pbr::logging::Handler {
  void emit(pbr::logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard lock(mu);
    text += message + "\n";
  }
  std::mutex mu;
  std::string text;
};
} // namespace

TEST(OwnDevicesTest, ANameNeverReachesALogOrAMetricOnTheWayToTheView) {
  LoadLocales();
  auto capture = std::make_shared<Collect>();
  for (const char* channel : {"Metrics", "Settings", "AccountLocker", "ConversationsHub"}) {
    pbr::logging::getLogger(channel).addHandler(capture);
  }
  const pbr::OwnDevicesView view = pbr::OwnDevicesViewFor({"self", "other"}, "self", false,
                                                          {{"self", "ZZ-NAME-MARKER"}, {"other", "ZZ-NAME-MARKER 2"}});
  EXPECT_EQ(view.devices[0].name, "ZZ-NAME-MARKER");
  pbr::MetricsLine("settings.devices_shown").Add("devices", static_cast<int>(view.devices.size())).Emit();
  std::lock_guard lock(capture->mu);
  EXPECT_NE(capture->text.find("settings.devices_shown"), std::string::npos) << "the capture is wired";
  EXPECT_EQ(capture->text.find("ZZ-NAME-MARKER"), std::string::npos);
}

TEST(OwnDevicesTest, OtherDevicesOnlyShowTheList) {
  LoadLocales();
  const pbr::OwnDevicesView view =
      pbr::OwnDevicesViewFor({"main-peer-aaaaaa", "self-peer-bbbbbb"}, "self-peer-bbbbbb", false);
  EXPECT_FALSE(view.this_is_main);
  ASSERT_EQ(view.devices.size(), 2u);
  EXPECT_TRUE(view.devices[0].is_main);
  EXPECT_FALSE(view.devices[0].is_this_device);
  EXPECT_TRUE(view.devices[1].is_this_device);
  EXPECT_FALSE(view.devices[1].is_main);
  for (const pbr::OwnDeviceView& device : view.devices) {
    EXPECT_FALSE(device.can_manage) << device.peer_id;
  }
}

TEST(OwnDevicesTest, SignedOutDeviceManagesNothing) {
  LoadLocales();
  // The newest list leaves this device out; even an entry naming it (not the case in practice) offers nothing.
  const pbr::OwnDevicesView view = pbr::OwnDevicesViewFor({"main-peer-aaaaaa", "other-peer-cccccc"}, "gone", true);
  EXPECT_TRUE(view.signed_out);
  EXPECT_FALSE(view.this_is_main);
  for (const pbr::OwnDeviceView& device : view.devices) {
    EXPECT_FALSE(device.can_manage) << device.peer_id;
    EXPECT_FALSE(device.is_this_device) << device.peer_id;
  }
}

TEST(OwnDevicesTest, NoListYetCountsAsMain) {
  LoadLocales();
  // The first card starts the list with this device first (the backend lets it give out a transfer code).
  const pbr::OwnDevicesView view = pbr::OwnDevicesViewFor({}, "self", false);
  EXPECT_TRUE(view.this_is_main);
  EXPECT_TRUE(view.devices.empty());
}
