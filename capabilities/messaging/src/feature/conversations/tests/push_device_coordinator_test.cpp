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
  reg.relay_user_id = "relay-user";
  return reg;
}

// P016: a CallKit iPhone's calls come by VoIP push, so its PushKit token is bound beside the APNs one.
TEST(PushDeviceCoordinatorTest, AVoipTokenIsBoundAsTheIosVoipDevice) {
  FakePushDeviceClient client;
  ASSERT_TRUE(PushDeviceCoordinator::RegisterWith(client, IosBinding(), "voip-token"));
  ASSERT_EQ(client.registered.size(), 2u);
  EXPECT_EQ(client.registered[0].platform, "ios");
  EXPECT_EQ(client.registered[0].push_token, "apns-token");
  const PushDeviceRegistration& voip = client.registered[1];
  EXPECT_EQ(voip.platform, std::string(IPushDeviceRegistrar::kVoipPushPlatform));
  EXPECT_EQ(voip.platform, "ios_voip");
  EXPECT_EQ(voip.device_id, "dev-1.voip");
  EXPECT_EQ(voip.push_token, "voip-token");
  EXPECT_EQ(voip.relay_user_id, "relay-user");
}

// A build without CallKit (China, Android) has no VoIP token: only its own binding, and its calls ring by alert.
TEST(PushDeviceCoordinatorTest, WithoutAVoipTokenOnlyTheDeviceIsBound) {
  FakePushDeviceClient client;
  ASSERT_TRUE(PushDeviceCoordinator::RegisterWith(client, IosBinding(), ""));
  ASSERT_EQ(client.registered.size(), 1u);
  EXPECT_EQ(client.registered[0].platform, "ios");
}

TEST(PushDeviceCoordinatorTest, AFailedVoipBindingIsReportedAfterTheDeviceIsBound) {
  FakePushDeviceClient client;
  client.fail_platform = "ios_voip";
  EXPECT_FALSE(PushDeviceCoordinator::RegisterWith(client, IosBinding(), "voip-token"));
  EXPECT_EQ(client.registered.size(), 2u);
}

TEST(PushDeviceCoordinatorTest, UnregisterRemovesBothBindings) {
  FakePushDeviceClient client;
  ASSERT_TRUE(PushDeviceCoordinator::UnregisterWith(client, IosBinding(), "voip-token"));
  ASSERT_EQ(client.unregistered.size(), 2u);
  EXPECT_EQ(client.unregistered[0].device_id, "dev-1");
  EXPECT_EQ(client.unregistered[1].device_id, "dev-1.voip");
  EXPECT_EQ(client.unregistered[1].platform, "ios_voip");
}

} // namespace
} // namespace pbr
