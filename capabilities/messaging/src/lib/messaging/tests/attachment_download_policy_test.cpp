#include "lib/messaging/AttachmentDownloadPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr uint64_t kSmartMax = 4000000;

TEST(AttachmentDownloadPolicyTest, SmartAutoEnqueuesSmallFilesOnly) {
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, 1024, false, kSmartMax, false));
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kSmartMax, false, kSmartMax, false));
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kSmartMax + 1, false, kSmartMax, false));
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kSmartMax + 1, false, kSmartMax, true))
      << "the network only matters to \"download everything\"";
}

TEST(AttachmentDownloadPolicyTest, SmartThresholdIsTheCallersLimit) {
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, 2000, false, 1000, false));
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, 2000, false, 2000, false));
}

TEST(AttachmentDownloadPolicyTest, AlwaysAutoTakesEverythingOnlyOnAnAllowedNetwork) {
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::AlwaysAuto, 30'000'000, false, kSmartMax, true));
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::AlwaysAuto, 30'000'000, false, kSmartMax, false))
      << "not allowed here: as Smart";
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::AlwaysAuto, 3'000'000, false, kSmartMax, false))
      << "as Smart: a small one still comes";
}

TEST(AttachmentDownloadPolicyTest, OnDemandTakesNothingWhateverTheNetwork) {
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::OnDemand, 3'000'000, false, kSmartMax, true));
  EXPECT_FALSE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::OnDemand, 1024, false, kSmartMax, false));
}

TEST(AttachmentDownloadPolicyTest, BacklogDrainOverridesSmart) {
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::Smart, kSmartMax + 1, true, kSmartMax, false));
  EXPECT_TRUE(ShouldAutoEnqueueAttachment(AttachmentDownloadPolicy::OnDemand, 1024, true, kSmartMax, false));
}

TEST(AttachmentDownloadPolicyTest, AlwaysAutoWithNeitherNetworkTickedIsSmart) {
  using P = AttachmentDownloadPolicy;
  EXPECT_EQ(EffectiveAttachmentDownloadPolicy(P::AlwaysAuto, false, false), P::Smart);
  EXPECT_EQ(EffectiveAttachmentDownloadPolicy(P::AlwaysAuto, true, false), P::AlwaysAuto);
  EXPECT_EQ(EffectiveAttachmentDownloadPolicy(P::AlwaysAuto, false, true), P::AlwaysAuto);
  EXPECT_EQ(EffectiveAttachmentDownloadPolicy(P::OnDemand, false, false), P::OnDemand);
  EXPECT_EQ(EffectiveAttachmentDownloadPolicy(P::Smart, true, true), P::Smart);
}

NetworkState Net(const bool online, const NetworkTransport transport, const bool expensive) {
  NetworkState state;
  state.online = online;
  state.transport = transport;
  state.expensive = expensive;
  return state;
}

TEST(AttachmentDownloadPolicyTest, OnAPhoneOnlyAnInexpensiveWifiIsWifi) {
  EXPECT_EQ(AutoDownloadNetworkOf(Net(true, NetworkTransport::Wifi, false), true), AutoDownloadNetwork::Wifi);
  EXPECT_EQ(AutoDownloadNetworkOf(Net(true, NetworkTransport::Wifi, true), true), AutoDownloadNetwork::Mobile)
      << "an expensive Wi-Fi (hotspot, low data mode) is the mobile network";
  EXPECT_EQ(AutoDownloadNetworkOf(Net(true, NetworkTransport::Cellular, false), true), AutoDownloadNetwork::Mobile);
  EXPECT_EQ(AutoDownloadNetworkOf(Net(true, NetworkTransport::Cellular, true), true), AutoDownloadNetwork::Mobile);
  EXPECT_EQ(AutoDownloadNetworkOf(Net(true, NetworkTransport::Other, false), true), AutoDownloadNetwork::Mobile)
      << "a VPN shell or an unrecognised link is the mobile network";
}

TEST(AttachmentDownloadPolicyTest, OfflineOrAnUnknownLinkIsNotAClassification) {
  EXPECT_EQ(AutoDownloadNetworkOf(Net(false, NetworkTransport::Wifi, false), true), AutoDownloadNetwork::Unknown);
  EXPECT_EQ(AutoDownloadNetworkOf(Net(true, NetworkTransport::Unknown, false), true), AutoDownloadNetwork::Unknown);
  EXPECT_EQ(AutoDownloadNetworkOf(NetworkState{}, true), AutoDownloadNetwork::Unknown);
}

TEST(AttachmentDownloadPolicyTest, ADesktopIsAlwaysWifi) {
  EXPECT_EQ(AutoDownloadNetworkOf(Net(true, NetworkTransport::Cellular, true), false), AutoDownloadNetwork::Wifi);
  EXPECT_EQ(AutoDownloadNetworkOf(NetworkState{}, false), AutoDownloadNetwork::Wifi);
}

TEST(AttachmentDownloadPolicyTest, TheTickOfTheCurrentNetworkDecides) {
  EXPECT_TRUE(AutoDownloadAllowedOn(AutoDownloadNetwork::Wifi, true, false));
  EXPECT_FALSE(AutoDownloadAllowedOn(AutoDownloadNetwork::Mobile, true, false));
  EXPECT_TRUE(AutoDownloadAllowedOn(AutoDownloadNetwork::Mobile, false, true));
  EXPECT_FALSE(AutoDownloadAllowedOn(AutoDownloadNetwork::Wifi, false, true));
  EXPECT_FALSE(AutoDownloadAllowedOn(AutoDownloadNetwork::Unknown, true, true));
}

TEST(AttachmentDownloadPolicyTest, AVoiceMessageIsKnownByItsDeclaredFields) {
  EXPECT_TRUE(IsDeclaredVoiceMessage("audio/ogg", "voice-4200.ogg", 25'000));
  EXPECT_TRUE(IsDeclaredVoiceMessage("Audio/OGG", "voice-4200.ogg", 25'000));
  EXPECT_TRUE(IsDeclaredVoiceMessage("audio/ogg", "voice-4200.ogg", 1'000'000));
  EXPECT_FALSE(IsDeclaredVoiceMessage("audio/ogg", "voice-4200.ogg", 1'000'001));
  EXPECT_FALSE(IsDeclaredVoiceMessage("audio/ogg", "voice-4200.ogg", 0));
  EXPECT_FALSE(IsDeclaredVoiceMessage("audio/ogg", "song.ogg", 25'000));
  EXPECT_FALSE(IsDeclaredVoiceMessage("audio/ogg", "voice-abc.ogg", 25'000));
  EXPECT_FALSE(IsDeclaredVoiceMessage("audio/ogg", "voice-.ogg", 25'000));
  EXPECT_FALSE(IsDeclaredVoiceMessage("audio/ogg", "voice-1234567890.ogg", 25'000));
  EXPECT_FALSE(IsDeclaredVoiceMessage("audio/mpeg", "voice-4200.ogg", 25'000));
}

TEST(AttachmentDownloadPolicyTest, WireRoundTrip) {
  EXPECT_EQ(AttachmentDownloadPolicyFromString("smart"), AttachmentDownloadPolicy::Smart);
  EXPECT_EQ(AttachmentDownloadPolicyToString(AttachmentDownloadPolicy::AlwaysAuto), "always_auto");
}

} // namespace
} // namespace pbr
