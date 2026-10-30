// Me → Security → Your devices (M021 d4): the first device is the main one; only it may remove another
// device or hand on the role; a signed-out device manages nothing.
#include "feature/settings/OwnDevices.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

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
  const pbr::OwnDevicesView view = pbr::OwnDevicesViewFor({"12D3KooSelf000001", "12D3KooOtherABCDEF", "12D3KooThird123456"},
                                                          "12D3KooSelf000001", false);
  EXPECT_TRUE(view.this_is_main);
  EXPECT_FALSE(view.signed_out);
  ASSERT_EQ(view.devices.size(), 3u);

  EXPECT_TRUE(view.devices[0].is_main);
  EXPECT_TRUE(view.devices[0].is_this_device);
  EXPECT_FALSE(view.devices[0].can_manage);  // never itself
  EXPECT_EQ(view.devices[0].label, "This device");

  EXPECT_FALSE(view.devices[1].is_main);
  EXPECT_FALSE(view.devices[1].is_this_device);
  EXPECT_TRUE(view.devices[1].can_manage);
  EXPECT_EQ(view.devices[1].peer_id, "12D3KooOtherABCDEF");
  EXPECT_EQ(view.devices[1].label, "Device ABCDEF");

  EXPECT_TRUE(view.devices[2].can_manage);
  EXPECT_EQ(view.devices[2].label, "Device 123456");
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

TEST(OwnDevicesTest, ShortPeerIdIsShownWhole) {
  LoadLocales();
  EXPECT_EQ(pbr::OwnDeviceLabel("abc", false), "Device abc");
  EXPECT_EQ(pbr::OwnDeviceLabel("abc", true), "This device");
}
