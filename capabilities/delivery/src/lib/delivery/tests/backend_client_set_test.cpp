#include "lib/delivery/BackendClientSet.h"

#include "common/tests/ScriptedHttpServer.h"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>
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

// A relay a contact's card names gets a client only within the foreign-relay policy; ours are never held to it.
TEST(BackendClientSetTest, AForeignRelayOutsideThePolicyGetsNoClient) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.Apply(At("https://relay.example", "https://relay.example", "https://relay.example"));
  for (const char* origin : {"https://127.0.0.1", "https://localhost/api", "https://[::1]", "https://169.254.169.254",
                             "https://10.0.0.5:8443", "http://other.example", "https://intranet"}) {
    EXPECT_EQ(clients.Sender().ClientFor(origin), nullptr) << origin;
  }
  EXPECT_NE(clients.Sender().ClientFor("https://other.example/api/relay"), nullptr);
  EXPECT_NE(clients.Sender().ClientFor("https://relay.example"), nullptr) << "our own relay";
}

TEST(BackendClientSetTest, OurOwnRelayOnAPrivateAddressKeepsItsClient) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.Apply(At("http://192.168.1.10:8080", "http://192.168.1.10:8080", "http://192.168.1.10:8080"));
  EXPECT_NE(clients.Sender().ClientFor("http://192.168.1.10:8080"), nullptr);
}

// The directory asks our relays (multi-relay R2): a name search every one of them, merged, the preferred relay's
// first; a lookup in order; the card a lookup answer carries goes to DirectoryCards.
class BackendClientSetDirectoryTest : public ::testing::Test {
protected:
  using Reply = test::ScriptedHttpServer::Reply;

  static Reply Hits(const std::string& json_hits) {
    return Reply{.status = 200, .headers = {}, .body = R"({"hits":[)" + json_hits + "]}"};
  }
  static std::string Hit(const std::string& account, const std::string& nickname) {
    return R"({"hit_id":")" + account + R"(","account_id":")" + account + R"(","nickname":")" + nickname + R"("})";
  }
  void Serve(test::ScriptedHttpServer& server, Reply search, Reply lookup) {
    server.SetHandler([search, lookup](const test::ScriptedHttpServer::Request& request) {
      if (request.target.rfind("/api/relay/v1/search", 0) == 0) {
        return search;
      }
      if (request.target.rfind("/api/relay/v1/users/by-account/", 0) == 0) {
        return lookup;
      }
      return Reply{.status = 404, .headers = {}, .body = "{}"};
    });
  }
  void ApplyBoth() {
    auto endpoints =
        At(preferred_.Origin() + "/api/relay", preferred_.Origin() + "/api/relay", preferred_.Origin() + "/api/relay");
    endpoints.extra_relays = {other_.Origin() + "/api/relay"};
    clients_.Apply(endpoints);
  }

  test::ScriptedHttpServer preferred_;
  test::ScriptedHttpServer other_;
  SwitchableDirectoryClient directory_;
  BackendClientSet clients_{directory_};
};

TEST_F(BackendClientSetDirectoryTest, ANameSearchAsksOurOtherRelaysTooThePreferredFirst) {
  Serve(preferred_, Hits(Hit("account:bob", "bob")), Reply{.status = 404, .headers = {}, .body = "{}"});
  Serve(other_, Hits(Hit("account:cat", "cat") + "," + Hit("account:bob", "bob")),
        Reply{.status = 404, .headers = {}, .body = "{}"});
  ApplyBoth();
  auto hits = directory_.SearchPeople("b");
  ASSERT_TRUE(hits) << hits.error().message;
  ASSERT_EQ(hits->size(), 2u);
  EXPECT_EQ(hits->at(0).nickname, "bob");
  EXPECT_EQ(hits->at(1).nickname, "cat");

  auto endpoints =
      At(preferred_.Origin() + "/api/relay", preferred_.Origin() + "/api/relay", preferred_.Origin() + "/api/relay");
  clients_.Apply(endpoints); // the other relay taken out of the config
  hits = directory_.SearchPeople("b");
  ASSERT_TRUE(hits);
  EXPECT_EQ(hits->size(), 1u) << "only the relays of ours are asked";
}

TEST_F(BackendClientSetDirectoryTest, ALookupGoesOnToTheNextRelayAndItsCardIsGiven) {
  Serve(preferred_, Hits(""), Reply{.status = 404, .headers = {}, .body = R"({"code":"not_registered"})"});
  Serve(other_, Hits(""),
        Reply{.status = 200,
              .headers = {},
              .body = R"({"account_id":"account:cat","nickname":"cat","account_card":{"issued_at":5}})"});
  std::mutex mutex;
  std::vector<std::string> given;
  clients_.DirectoryCards().Set([&](const std::string& account_id, const Object& card) {
    std::lock_guard lock(mutex);
    given.push_back(account_id + "@" + std::to_string(card.getIf<int64_t>("issued_at").value_or(0)));
  });
  ApplyBoth();
  auto hit = directory_.LookupByAccount("account:cat");
  ASSERT_TRUE(hit) << hit.error().message;
  EXPECT_EQ(hit->nickname, "cat");
  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(given, (std::vector<std::string>{"account:cat@5"})) << "kept by the owner once it verifies";
  }
  clients_.DirectoryCards().Set(nullptr);
  ASSERT_TRUE(directory_.LookupByAccount("account:cat"));
  std::lock_guard lock(mutex);
  EXPECT_EQ(given.size(), 1u) << "cleared: no card given";
}

TEST_F(BackendClientSetDirectoryTest, ADirectoryConfiguredApartReplacesThePreferredRelays) {
  Serve(preferred_, Hits(Hit("account:bob", "bob")), Reply{.status = 404, .headers = {}, .body = "{}"});
  Serve(other_, Hits(Hit("account:cat", "cat")), Reply{.status = 404, .headers = {}, .body = "{}"});
  clients_.Apply(
      At(preferred_.Origin() + "/api/relay", other_.Origin() + "/api/relay", preferred_.Origin() + "/api/relay"));
  auto hits = directory_.SearchPeople("c");
  ASSERT_TRUE(hits);
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ(hits->at(0).nickname, "cat");
  EXPECT_TRUE(preferred_.Requests().empty()) << "the configured directory is asked instead of the preferred relay's";
}

} // namespace
} // namespace pbr
