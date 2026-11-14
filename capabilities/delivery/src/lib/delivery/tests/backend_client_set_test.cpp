#include "lib/delivery/BackendClientSet.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

BackendClientSet::Endpoints At(const std::string& relay, const std::string& directory,
                               const std::string& registration) {
  BackendClientSet::Endpoints endpoints;
  endpoints.relay.base_url = relay;
  endpoints.directory.base_url = directory;
  endpoints.registration.base_url = registration;
  return endpoints;
}

// The backend's clients follow the config's endpoints (capability-repos CR018).
TEST(BackendClientSetTest, BuildsEveryClientForItsEndpoint) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.example"));
  EXPECT_NE(clients.Relay(), nullptr);
  EXPECT_NE(clients.PushDevices(), nullptr);
  EXPECT_NE(clients.ClientCompat(), nullptr);
  EXPECT_NE(clients.Registration(), nullptr);
  EXPECT_NE(clients.Blob(), nullptr);
  EXPECT_NE(clients.Locker(), nullptr);
  EXPECT_TRUE(directory.Configured());
}

TEST(BackendClientSetTest, AnEmptyEndpointTakesTheAppsDefault) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.SetDefaults(At("https://relay.default", "https://directory.default", "https://registration.default"));
  clients.Apply(At("", "", ""));
  EXPECT_NE(clients.Relay(), nullptr);
  EXPECT_NE(clients.Registration(), nullptr);
  EXPECT_TRUE(directory.Configured());
}

TEST(BackendClientSetTest, NoEndpointAndNoDefaultLeavesTheClientsOut) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.Apply(At("", "", ""));
  EXPECT_EQ(clients.Relay(), nullptr);
  EXPECT_EQ(clients.PushDevices(), nullptr);
  EXPECT_EQ(clients.Registration(), nullptr);
  EXPECT_EQ(clients.Locker(), nullptr);
  EXPECT_FALSE(directory.Configured());
}

TEST(BackendClientSetTest, AnUnchangedEndpointKeepsItsClientsAndAChangedOneRebuildsThem) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.example"));
  IRelayClient* relay = clients.Relay();
  IRegistrationClient* registration = clients.Registration();

  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.example"));
  EXPECT_EQ(clients.Relay(), relay) << "holders keep a client whose endpoint did not change";
  EXPECT_EQ(clients.Registration(), registration);

  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.other"));
  EXPECT_EQ(clients.Relay(), relay);
  // The new client is made before the old one goes, so a rebuilt client never shares the old address.
  EXPECT_NE(clients.Registration(), registration) << "a changed endpoint gets a new client";
  EXPECT_NE(clients.Registration(), nullptr);
}

TEST(BackendClientSetTest, ClearDropsEveryClient) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.example"));
  clients.Clear();
  EXPECT_EQ(clients.Relay(), nullptr);
  EXPECT_EQ(clients.Blob(), nullptr);
}

// Our other relays (multi-relay R2): a message, push and registration client each, after the preferred relay; at
// most four in all; the sends go through the one sender, which knows them.
TEST(BackendClientSetTest, OurOtherRelaysGetClientsOfTheirOwnAfterThePreferredOne) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  auto endpoints = At("https://a.example/api/relay", "https://a.example/api/relay", "https://a.example/api/relay");
  endpoints.extra_relays = {"https://b.example/api/relay/", "https://A.example/api/relay",
                            "https://c.example/api/relay", "https://d.example/api/relay",
                            "https://e.example/api/relay"};
  clients.Apply(endpoints);
  const auto& ours = clients.OurRelays();
  ASSERT_EQ(ours.size(), 4u) << "at most four in all; the preferred one listed again is left out";
  EXPECT_EQ(ours[0].origin, "https://a.example/api/relay");
  EXPECT_EQ(ours[0].relay.get(), clients.Relay());
  EXPECT_EQ(ours[0].registration, nullptr) << "the preferred relay registers through Registration()";
  EXPECT_EQ(ours[1].origin, "https://b.example/api/relay");
  EXPECT_EQ(ours[3].origin, "https://d.example/api/relay");
  for (size_t i = 1; i < ours.size(); ++i) {
    EXPECT_NE(ours[i].relay, nullptr);
    EXPECT_NE(ours[i].push_devices, nullptr);
    EXPECT_NE(ours[i].registration, nullptr);
  }
  IRelayClient* b = ours[1].relay.get();
  clients.Apply(endpoints);
  EXPECT_EQ(clients.OurRelays()[1].relay.get(), b) << "unchanged: kept";
}

TEST(BackendClientSetTest, NoPreferredRelayMeansNoOtherRelaysEither) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  auto endpoints = At("", "", "");
  endpoints.extra_relays = {"https://b.example/api/relay"};
  clients.Apply(endpoints);
  EXPECT_TRUE(clients.OurRelays().empty());
}

} // namespace
} // namespace pbr
