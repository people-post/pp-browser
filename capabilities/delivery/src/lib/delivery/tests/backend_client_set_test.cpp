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
  IRelayClient* relay = clients.Relay().get();
  IRegistrationClient* registration = clients.Registration();

  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.example"));
  EXPECT_EQ(clients.Relay().get(), relay) << "holders keep a client whose endpoint did not change";
  EXPECT_EQ(clients.Registration(), registration);

  ILockerClient* locker = clients.Locker();
  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.other"));
  EXPECT_EQ(clients.Relay().get(), relay);
  EXPECT_EQ(clients.Registration(), registration) << "registration is the preferred relay's (MR006)";
  // The new client is made before the old one goes, so a rebuilt client never shares the old address.
  EXPECT_NE(clients.Locker(), locker) << "a changed endpoint gets a new client";
  EXPECT_NE(clients.Locker(), nullptr);

  clients.Apply(At("https://relay.other", "https://directory.example", "https://registration.other"));
  EXPECT_NE(clients.Relay().get(), relay);
  EXPECT_NE(clients.Registration(), registration);
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
  EXPECT_EQ(ours[0].relay, clients.Relay());
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

// Relay services follow the preferred relay; org services stay on the org's origin (multi-relay MR006,
// docs/contracts/SERVICE_ENDPOINTS.md § Relay services and org services).
class BackendClientSetOriginTest : public ::testing::Test {
protected:
  using Reply = test::ScriptedHttpServer::Reply;

  void SetUp() override {
    for (test::ScriptedHttpServer* server : {&org_, &other_}) {
      server->SetHandler([](const test::ScriptedHttpServer::Request& request) {
        if (request.target.find("/v1/client-compat") != std::string::npos) {
          return Reply{.status = 200, .headers = {}, .body = R"({"schema_version":1})"};
        }
        if (request.target.find("/v1/search") != std::string::npos) {
          return Reply{.status = 200, .headers = {}, .body = R"({"hits":[]})"};
        }
        if (request.target.find("/v1/register/start") != std::string::npos) {
          return Reply{.status = 200, .headers = {}, .body = R"({"challenge":"c","signature_alg":"ml-dsa-65"})"};
        }
        if (request.target.find("/v1/register/finish") != std::string::npos) {
          return Reply{.status = 200,
                       .headers = {},
                       .body = R"({"success":true,"account_id":"account:me","llm_api_key":"brf_llm_x"})"};
        }
        return Reply{.status = 404, .headers = {}, .body = "{}"};
      });
    }
  }
  std::string OrgRelay() const { return org_.Origin() + "/api/relay"; }
  std::string OtherRelay() const { return other_.Origin() + "/api/relay"; }
  /** The org's config; ours `our_relays` (empty: the default). */
  void Apply(std::vector<std::string> our_relays) {
    auto endpoints = At(OrgRelay(), OrgRelay(), OrgRelay());
    endpoints.our_relays = std::move(our_relays);
    clients_.Apply(endpoints);
  }
  static size_t Asked(const test::ScriptedHttpServer& server, const std::string& path) {
    size_t asked = 0;
    for (const auto& request : server.Requests()) {
      asked += request.target.find(path) != std::string::npos ? 1 : 0;
    }
    return asked;
  }
  std::string FinishedKey() {
    auto finished = clients_.Registration()->FinishRegistration("c", "pk", "me", "sig", 1, "ml-dsa-65", "kem");
    return finished ? finished->llm_api_key.Reveal() : "error: " + finished.error().message;
  }

  test::ScriptedHttpServer org_;
  test::ScriptedHttpServer other_;
  SwitchableDirectoryClient directory_;
  BackendClientSet clients_{directory_};
};

TEST_F(BackendClientSetOriginTest, ByDefaultEveryServiceIsTheOrgsRelay) {
  Apply({});
  EXPECT_EQ(clients_.Registration()->BaseUrl(), OrgRelay());
  EXPECT_EQ(clients_.HistoryRelay(), clients_.Relay());
  ASSERT_EQ(clients_.OurRelays().size(), 1u);
  EXPECT_EQ(clients_.OurRelays()[0].origin, OrgRelay());
  ASSERT_TRUE(clients_.ClientCompat()->Fetch());
  ASSERT_TRUE(directory_.SearchPeople("bob"));
  EXPECT_EQ(Asked(org_, "/v1/client-compat"), 1u);
  EXPECT_EQ(Asked(org_, "/v1/search"), 1u);
  EXPECT_EQ(FinishedKey(), "brf_llm_x") << "the org's relay gives the Brief LLM key";
}

TEST_F(BackendClientSetOriginTest, TheListGivenWholeIsTheDefaultsSame) {
  Apply({});
  IRelayClient* relay = clients_.Relay().get();
  Apply({OrgRelay()});
  EXPECT_EQ(clients_.Relay().get(), relay) << "a list of the org's relay alone is the default: nothing rebuilt";
}

TEST_F(BackendClientSetOriginTest, RelayServicesFollowThePreferredRelayAndOrgServicesStay) {
  Apply({OtherRelay(), OrgRelay()});
  EXPECT_EQ(clients_.Registration()->BaseUrl(), OtherRelay()) << "registration is the preferred relay's";
  ASSERT_EQ(clients_.OurRelays().size(), 2u);
  EXPECT_EQ(clients_.OurRelays()[0].origin, OtherRelay());
  EXPECT_EQ(clients_.OurRelays()[0].relay, clients_.Relay()) << "messages and push: the preferred relay's";
  EXPECT_EQ(clients_.OurRelays()[1].origin, OrgRelay());
  EXPECT_EQ(clients_.HistoryRelay(), clients_.OurRelays()[1].relay)
      << "stream history stays on the relay that holds it";
  EXPECT_EQ(clients_.OrgRelay(), OrgRelay());

  ASSERT_TRUE(clients_.ClientCompat()->Fetch());
  EXPECT_EQ(Asked(org_, "/v1/client-compat"), 1u) << "client compatibility is the org's";
  EXPECT_EQ(Asked(other_, "/v1/client-compat"), 0u);

  auto hits = directory_.SearchPeople("bob");
  ASSERT_TRUE(hits) << hits.error().message;
  EXPECT_EQ(Asked(other_, "/v1/search"), 1u) << "the directory left at the org's relay follows the preferred one";
  EXPECT_EQ(Asked(org_, "/v1/search"), 1u) << "and asks our other relay too";

  EXPECT_EQ(FinishedKey(), "") << "another relay's Brief LLM key is not the org's: not kept";
  auto org_key =
      clients_.OurRelays()[1].registration->FinishRegistration("c", "pk", "me", "sig", 1, "ml-dsa-65", "kem");
  ASSERT_TRUE(org_key);
  EXPECT_EQ(org_key->llm_api_key.Reveal(), "brf_llm_x") << "the org's relay, now another of ours, still gives it";
}

TEST_F(BackendClientSetOriginTest, TheBriefLlmKeyFollowsTheOrgsRelayWhenOnlyTheOrgChanges) {
  // Our list stays (org's relay preferred, the other after it): their clients are kept, but the org's relay moves to
  // the other one. Which client keeps the key is decided again at every Apply, not when a client was built.
  Apply({OrgRelay(), OtherRelay()});
  IRegistrationClient* preferred = clients_.Registration();
  ASSERT_EQ(FinishedKey(), "brf_llm_x");

  auto endpoints = At(OtherRelay(), OtherRelay(), OtherRelay());
  endpoints.our_relays = {OrgRelay(), OtherRelay()};
  clients_.Apply(endpoints);
  ASSERT_EQ(clients_.Registration(), preferred) << "the preferred relay's client is kept";
  EXPECT_EQ(FinishedKey(), "") << "no longer the org's relay: its key is not kept";
  ASSERT_EQ(clients_.OurRelays().size(), 2u);
  auto org_key =
      clients_.OurRelays()[1].registration->FinishRegistration("c", "pk", "me", "sig", 1, "ml-dsa-65", "kem");
  ASSERT_TRUE(org_key) << org_key.error().message;
  EXPECT_EQ(org_key->llm_api_key.Reveal(), "brf_llm_x") << "the relay that is now the org's keeps it";
}

TEST_F(BackendClientSetOriginTest, StreamHistoryFallsToThePreferredRelayOnceTheOrgsIsNotOurs) {
  Apply({OtherRelay()});
  EXPECT_EQ(clients_.HistoryRelay(), clients_.Relay());
  EXPECT_NE(clients_.Locker(), nullptr) << "lockers stay at the org's registration URL";
  ASSERT_TRUE(clients_.ClientCompat()->Fetch());
  EXPECT_EQ(Asked(org_, "/v1/client-compat"), 1u);
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
