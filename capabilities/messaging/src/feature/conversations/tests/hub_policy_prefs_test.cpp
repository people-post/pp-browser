#include "feature/conversations/HubPolicyPrefs.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(HubPolicyPrefsTest, DownloadEverythingIsOnWifiOnlyByDefault) {
  const HubPolicyPrefs prefs;
  EXPECT_TRUE(prefs.attachment_auto_on_wifi);
  EXPECT_FALSE(prefs.attachment_auto_on_cellular);
}

// Only a tick changed: it must still count as a change, or the hub would skip applying it.
TEST(HubPolicyPrefsTest, EitherNetworkTickAloneIsAChange) {
  const HubPolicyPrefs before;
  HubPolicyPrefs wifi = before;
  wifi.attachment_auto_on_wifi = false;
  EXPECT_NE(wifi, before);
  HubPolicyPrefs cellular = before;
  cellular.attachment_auto_on_cellular = true;
  EXPECT_NE(cellular, before);
  EXPECT_EQ(HubPolicyPrefs{}, before);
}

} // namespace
} // namespace pbr
