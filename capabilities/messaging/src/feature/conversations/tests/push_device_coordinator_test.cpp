#include "feature/conversations/PushDeviceCoordinator.h"

#include "common/IPushDeviceRegistrar.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

/** The relay's device endpoints, recording each binding. */
class FakePushDeviceClient final : public IPushDeviceClient {
public:
  Roe<void> RegisterDevice(const PushDeviceRegistration& registration) override {
    registered.push_back(registration);
    if (registration.platform == fail_platform) {
      return Error("refused");
    }
    return {};
  }
  Roe<void> UnregisterDevice(const PushDeviceRegistration& registration) override {
    unregistered.push_back(registration);
    if (registration.platform == fail_platform) {
      return Error("refused");
    }
    return {};
  }

  std::vector<PushDeviceRegistration> registered;
  std::vector<PushDeviceRegistration> unregistered;
  std::string fail_platform;
};

PushDeviceRegistration IosBinding() {
  PushDeviceRegistration reg;
  reg.platform = "ios";
  reg.device_id = "dev-1";
  reg.push_token = "apns-token";
  reg.account_id = "account-1";
  return reg;
}

TEST(PushDeviceCoordinatorTest, TheDefaultPinBindsTheVoipTokenAsTheIosVoipDevice) {
  FakePushDeviceClient client;
  ASSERT_TRUE(PushDeviceCoordinator::RegisterWith(client, IosBinding(), "voip-token", /*voip_wanted=*/true));
  ASSERT_EQ(client.registered.size(), 2u);
  EXPECT_EQ(client.registered[0].platform, "ios");
  EXPECT_EQ(client.registered[0].push_token, "apns-token");
  const PushDeviceRegistration& voip = client.registered[1];
  EXPECT_EQ(voip.platform, std::string(IPushDeviceRegistrar::kVoipPushPlatform));
  EXPECT_EQ(voip.platform, "ios_voip");
  EXPECT_EQ(voip.device_id, "dev-1.voip");
  EXPECT_EQ(voip.push_token, "voip-token");
  EXPECT_EQ(voip.account_id, "account-1");
  EXPECT_TRUE(client.unregistered.empty());
}

// A custom PIN has no key in the background: its calls ring by alert, so the VoIP device is removed.
TEST(PushDeviceCoordinatorTest, ACustomPinUnbindsTheVoipDeviceAndKeepsTheAlertDevice) {
  FakePushDeviceClient client;
  ASSERT_TRUE(PushDeviceCoordinator::RegisterWith(client, IosBinding(), "voip-token", /*voip_wanted=*/false));
  ASSERT_EQ(client.registered.size(), 1u);
  EXPECT_EQ(client.registered[0].platform, "ios");
  ASSERT_EQ(client.unregistered.size(), 1u);
  EXPECT_EQ(client.unregistered[0].platform, "ios_voip");
  EXPECT_EQ(client.unregistered[0].device_id, "dev-1.voip");
}

// No PushKit token (a build without CallKit, Android, or the token not yet delivered): no VoIP binding is touched.
TEST(PushDeviceCoordinatorTest, WithoutAVoipTokenOnlyTheDeviceIsBound) {
  for (const bool wanted : {true, false}) {
    FakePushDeviceClient client;
    ASSERT_TRUE(PushDeviceCoordinator::RegisterWith(client, IosBinding(), "", wanted));
    ASSERT_EQ(client.registered.size(), 1u);
    EXPECT_EQ(client.registered[0].platform, "ios");
    EXPECT_TRUE(client.unregistered.empty());
  }
}

TEST(PushDeviceCoordinatorTest, AFailedVoipBindingIsReportedAfterTheDeviceIsBound) {
  FakePushDeviceClient client;
  client.fail_platform = "ios_voip";
  EXPECT_FALSE(PushDeviceCoordinator::RegisterWith(client, IosBinding(), "voip-token", true));
  EXPECT_EQ(client.registered.size(), 2u);
}

TEST(PushDeviceCoordinatorTest, AFailedVoipUnbindIsReported) {
  FakePushDeviceClient client;
  client.fail_platform = "ios_voip";
  EXPECT_FALSE(PushDeviceCoordinator::RegisterWith(client, IosBinding(), "voip-token", false));
  EXPECT_EQ(client.registered.size(), 1u) << "the alert device is bound all the same";
}

TEST(PushDeviceCoordinatorTest, UnregisterRemovesBothBindings) {
  FakePushDeviceClient client;
  ASSERT_TRUE(PushDeviceCoordinator::UnregisterWith(client, IosBinding(), "voip-token"));
  ASSERT_EQ(client.unregistered.size(), 2u);
  EXPECT_EQ(client.unregistered[0].device_id, "dev-1");
  EXPECT_EQ(client.unregistered[1].device_id, "dev-1.voip");
  EXPECT_EQ(client.unregistered[1].platform, "ios_voip");
}

TEST(PushDeviceCoordinatorTest, TheVoipWishIsAProcessFlagOffByDefault) {
  EXPECT_FALSE(IPushDeviceRegistrar::VoipPushWanted());
  IPushDeviceRegistrar::SetVoipPushWanted(true);
  EXPECT_TRUE(IPushDeviceRegistrar::VoipPushWanted());
  IPushDeviceRegistrar::SetVoipPushWanted(false);
}

} // namespace
} // namespace pbr
