#include "lib/delivery/BackendClientSet.h"

#include "common/ValueJson.h"
#include "common/tests/ScriptedHttpServer.h"
#include "lib/net/testing/TestRelayCertificates.h"

#include <gtest/gtest.h>

#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

const SignedRelayCommand kSignedFinish{.audience = "QmRelay", .timestamp = 1, .signature = "sig"};

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

// MR007, MR008-PIN, SIGNED_FORMATS § 3.4: every client of a relay signs for the PeerId its held certificate proves for
// the relay id its address names; the lockers at the org's registration URL for its. Nothing is asked of the relays
// to sign when the certificates are held (the build's, client-compat's).
TEST(BackendClientSetTest, EachRelaysClientsSignForItsPin) {
  const test::TestRelay relay_a(0x21);
  const test::TestRelay relay_b(0x22);
  const std::string kRelayA = test::TestRelay::PeerIdOf(0x31);
  const std::string kRelayB = test::TestRelay::PeerIdOf(0x32);
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  auto pins = std::make_shared<RelayPins>("");
  int asked = 0;
  pins->SetFetch([&asked](const std::string&, bool) -> Roe<RelayIdentity> {
    ++asked;
    return Error("not asked");
  });
  ASSERT_TRUE(pins->Expect("https://relay.example/api/relay", relay_a.Id()));
  ASSERT_TRUE(pins->Expect("https://b.example/api/relay", relay_b.Id()));
  ASSERT_TRUE(pins->OfferCertificate(
      relay_a.Id(),
      relay_a.Certificate(1, {"https://relay.example/api/relay", "https://registration.example/api/relay"}, 0x31),
      "the build"));
  ASSERT_TRUE(pins->OfferCertificate(relay_b.Id(), relay_b.Certificate(1, {"https://b.example/api/relay"}, 0x32),
                                     "client-compat"));
  clients.SetPins(pins);
  auto endpoints = At("https://relay.example/api/relay", "https://relay.example/api/relay",
                      "https://registration.example/api/relay");
  endpoints.extra_relays = {"https://b.example/api/relay"};
  clients.Apply(endpoints);
  ASSERT_NE(clients.Registration()->Audience(), nullptr);
  EXPECT_EQ(clients.Registration()->Audience()->PeerId().valueOr(""), kRelayA);
  ASSERT_EQ(clients.OurRelays().size(), 2u);
  EXPECT_EQ(clients.OurRelays()[1].registration->Audience()->PeerId().valueOr(""), kRelayB)
      << "another relay of ours: its own";
  EXPECT_EQ(asked, 0) << "its address names it: nothing learned from the relay";
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
  IRegistrationClient* registration = clients.Registration().get();

  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.example"));
  EXPECT_EQ(clients.Relay().get(), relay) << "holders keep a client whose endpoint did not change";
  EXPECT_EQ(clients.Registration().get(), registration);

  ILockerClient* locker = clients.Locker().get();
  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.other"));
  EXPECT_EQ(clients.Relay().get(), relay);
  EXPECT_EQ(clients.Registration().get(), registration) << "registration is the preferred relay's (MR006)";
  // The new client is made before the old one goes, so a rebuilt client never shares the old address.
  EXPECT_NE(clients.Locker().get(), locker) << "a changed endpoint gets a new client";
  EXPECT_NE(clients.Locker(), nullptr);

  clients.Apply(At("https://relay.other", "https://directory.example", "https://registration.other"));
  EXPECT_NE(clients.Relay().get(), relay);
  EXPECT_NE(clients.Registration().get(), registration);
}

TEST(BackendClientSetTest, AClientTakenBeforeAChangeStaysUsableAfterIt) {
  // A worker took the preferred relay's clients, then the preferred relay changed (MR006): what it holds is its own.
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.Apply(At("https://relay.example", "https://directory.example", "https://registration.example"));
  const std::shared_ptr<IRegistrationClient> registration = clients.Registration();
  const std::shared_ptr<IPushDeviceClient> push = clients.PushDevices();
  const std::shared_ptr<IClientCompatClient> compat = clients.ClientCompat();
  const std::shared_ptr<ILockerClient> locker = clients.Locker();
  const std::weak_ptr<IRegistrationClient> watched = registration;
  clients.Apply(At("https://relay.other", "https://directory.example", "https://registration.other"));
  EXPECT_NE(clients.Registration(), registration) << "rebuilt for the new relay";
  EXPECT_EQ(registration->BaseUrl(), "https://relay.example") << "the old one still answers to its holder";
  EXPECT_EQ(registration.use_count(), 1) << "held by the worker alone now";
  EXPECT_NE(push, nullptr);
  EXPECT_NE(compat, nullptr);
  EXPECT_NE(locker, nullptr);
  EXPECT_FALSE(watched.expired());
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

// A relay a contact's card names gets a client only on a public host (IsPublicRelayOrigin); ours are never held to it.
TEST(BackendClientSetTest, AForeignRelayOutsideThePolicyGetsNoClient) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.SetPins(std::make_shared<RelayPins>(""));
  clients.Apply(At("https://relay.example", "https://relay.example", "https://relay.example"));
  const RelayId named = RelayId::FromRaw(ByteVector(kRelayIdBytes, 0x55)).value();
  for (const char* origin : {"https://127.0.0.1", "https://localhost/api", "https://[::1]", "https://169.254.169.254",
                             "https://10.0.0.5:8443", "http://other.example", "https://intranet"}) {
    EXPECT_EQ(clients.Sender().ClientFor({.origin = origin, .relay_id = named}), nullptr) << origin;
    EXPECT_EQ(clients.Sender().ClientFor({.origin = origin}), nullptr) << origin << " (no relay id)";
  }
  EXPECT_NE(clients.Sender().ClientFor({.origin = "https://other.example/api/relay", .relay_id = named}), nullptr)
      << "a card's relay: its audience is what that relay id's certificate proves (ContactAudience)";
  EXPECT_EQ(clients.Sender().ClientFor({.origin = "https://unnamed.example/api/relay"}), nullptr)
      << "MR008: a relay not ours whose address names no relay id gets nothing signed";
  EXPECT_NE(clients.Sender().ClientFor({.origin = "https://relay.example"}), nullptr) << "our own relay";
}

TEST(BackendClientSetTest, OurOwnRelayOnAPrivateAddressKeepsItsClient) {
  SwitchableDirectoryClient directory;
  BackendClientSet clients(directory);
  clients.Apply(At("http://192.168.1.10:8080", "http://192.168.1.10:8080", "http://192.168.1.10:8080"));
  EXPECT_NE(clients.Sender().ClientFor({.origin = "http://192.168.1.10:8080"}), nullptr);
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
        if (request.target.find("/v1/users/by-account/") != std::string::npos) {
          return Reply{.status = 404, .headers = {}, .body = R"({"code":"not_registered"})"};
        }
        if (request.target == "/api/llm/v1/token/renew") {
          return Reply{.status = 200,
                       .headers = {},
                       .body = R"({"llm_token":"ppl1.renewed","llm_token_expires_at":"2099-01-15T00:00:00.000Z"})"};
        }
        if (request.target.find("/v1/register/start") != std::string::npos) {
          return Reply{.status = 200, .headers = {}, .body = R"({"challenge":"c","signature_alg":"ml-dsa-65"})"};
        }
        if (request.target.find("/v1/register/finish") != std::string::npos) {
          return Reply{.status = 200,
                       .headers = {},
                       .body = R"({"success":true,"account_id":"account:me","llm_token":"brf_llm_x",)"
                               R"("llm_token_expires_at":"2099-01-08T00:00:00.000Z","llm_api_key":"legacy"})"};
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
    auto finished = clients_.Registration()->FinishRegistration("c", "pk", "me", kSignedFinish, "ml-dsa-65", "kem");
    return finished ? finished->llm_token.Reveal() : "error: " + finished.error().message;
  }

  test::ScriptedHttpServer org_;
  test::ScriptedHttpServer other_;
  SwitchableDirectoryClient directory_;
  BackendClientSet clients_{directory_};
  // The scripted relays are on loopback: as a developer build, which may add one there.
  bool local_allowed_ = [this]() {
    clients_.SetLocalRelaysAllowed(true);
    return true;
  }();
};

TEST_F(BackendClientSetOriginTest, ByDefaultEveryServiceIsTheOrgsRelay) {
  Apply({});
  EXPECT_EQ(clients_.Registration()->BaseUrl(), OrgRelay());
  EXPECT_EQ(clients_.HistoryRelay(), clients_.Relay());
  ASSERT_EQ(clients_.OurRelays().size(), 1u);
  EXPECT_EQ(clients_.OurRelays()[0].origin, OrgRelay());
  ASSERT_TRUE(clients_.ClientCompat()->Fetch());
  ASSERT_TRUE(directory_.SearchPeople("account:bob"));
  EXPECT_EQ(Asked(org_, "/v1/client-compat"), 1u);
  EXPECT_EQ(Asked(org_, "/v1/users/by-account/"), 1u);
  EXPECT_EQ(Asked(org_, "/v1/search"), 0u) << "the legacy search is never asked (relay 1.0 removes it)";
  EXPECT_EQ(FinishedKey(), "brf_llm_x") << "the org's relay gives the Brief LLM token, not the legacy key";
}

// RC018 item 2: the token is renewed where it was issued, `token/renew` under the LLM prefix of the org's origin.
TEST_F(BackendClientSetOriginTest, TheOrgsRegistryRenewsTheBriefLlmToken) {
  Apply({OtherRelay(), OrgRelay()});
  EXPECT_FALSE(clients_.Registration()->TakesBriefLlmKey()) << "the preferred relay is not the org's";
  const std::shared_ptr<IRegistrationClient> org = clients_.OurRelays()[1].registration;
  ASSERT_TRUE(org->TakesBriefLlmKey());
  auto renewed = org->RenewLlmToken("account:me", kSignedFinish);
  ASSERT_TRUE(renewed) << renewed.error().message;
  EXPECT_EQ(renewed->llm_token.Reveal(), "ppl1.renewed");
  EXPECT_EQ(renewed->llm_token_expires_at, "2099-01-15T00:00:00.000Z");
  ASSERT_EQ(Asked(org_, "/api/llm/v1/token/renew"), 1u);
  EXPECT_EQ(Asked(org_, "/keys/rotate"), 0u) << "the legacy rotation is never asked (relay 1.0 removes it)";
  for (const auto& request : org_.Requests()) {
    if (request.target == "/api/llm/v1/token/renew") {
      const auto body = TryParseObject(request.body);
      ASSERT_TRUE(body);
      EXPECT_EQ(body->getString("account_id").value_or(""), "account:me");
      EXPECT_FALSE(body->getString("signature").value_or("").empty()) << "signed with the account key";
    }
  }
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

  auto hits = directory_.SearchPeople("account:bob");
  ASSERT_TRUE(hits) << hits.error().message;
  EXPECT_EQ(Asked(other_, "/v1/users/by-account/"), 1u)
      << "the directory left at the org's relay follows the preferred one";
  EXPECT_EQ(Asked(org_, "/v1/users/by-account/"), 1u) << "and asks our other relay too";

  EXPECT_EQ(FinishedKey(), "") << "another relay's Brief LLM key is not the org's: not kept";
  auto org_key =
      clients_.OurRelays()[1].registration->FinishRegistration("c", "pk", "me", kSignedFinish, "ml-dsa-65", "kem");
  ASSERT_TRUE(org_key);
  EXPECT_EQ(org_key->llm_token.Reveal(), "brf_llm_x") << "the org's relay, now another of ours, still gives it";
  EXPECT_EQ(org_key->llm_token_expires_at, "2099-01-08T00:00:00.000Z");
}

// S4: a relay the person added (not configured) is spoken to on public addresses only, outside developer builds: a
// name that resolves to this device or a private network later is refused at connect time. The configured stay.
TEST_F(BackendClientSetOriginTest, ARelayThePersonAddedIsHeldToPublicAddressesOutsideDeveloperBuilds) {
  clients_.SetLocalRelaysAllowed(false);
  Apply({OrgRelay(), OtherRelay()});
  ASSERT_EQ(clients_.OurRelays().size(), 2u);
  EXPECT_FALSE(
      clients_.OurRelays()[1].registration->FinishRegistration("c", "pk", "me", kSignedFinish, "ml-dsa-65", "kem"))
      << "the added relay on loopback is refused";
  EXPECT_EQ(FinishedKey(), "brf_llm_x") << "the configured relay is reached as configured";

  BackendClientSet preferring(directory_);
  preferring.Apply([this]() {
    auto endpoints = At(OrgRelay(), OrgRelay(), OrgRelay());
    endpoints.our_relays = {OtherRelay(), OrgRelay()};
    return endpoints;
  }());
  EXPECT_FALSE(preferring.Registration()->FinishRegistration("c", "pk", "me", kSignedFinish, "ml-dsa-65", "kem"))
      << "preferred, it is held alike";
  EXPECT_FALSE(preferring.Relay()->PollInbox("account:me", ""));
  EXPECT_EQ(Asked(other_, "/v1/"), 0u);
}

// T1: a client in use is never written: it signs through the set's one signer slot, set before it is handed out, so
// a signer given later reaches it and an Apply that keeps it changes nothing on it.
TEST_F(BackendClientSetOriginTest, AClientTakenBeforeTheSignerSignsWithItAndApplyNeverRewritesIt) {
  auto pins = std::make_shared<RelayPins>("");
  const test::TestRelay org(0x23);
  ASSERT_TRUE(pins->Expect(OrgRelay(), org.Id())); // the build's relay id and certificate
  ASSERT_TRUE(pins->OfferCertificate(org.Id(), org.Certificate(1, {OrgRelay()}, 0x33), "the build"));
  clients_.SetPins(pins);
  Apply({OrgRelay()});
  const std::shared_ptr<IRelayClient> relay = clients_.Relay();
  EXPECT_FALSE(relay->PollInbox("account:me", ""));
  EXPECT_EQ(Asked(org_, "/v1/inbox/poll"), 0u) << "nothing goes unsigned";
  int signed_calls = 0;
  clients_.SetSigner([&signed_calls](const std::vector<uint8_t>&) -> Roe<std::string> {
    ++signed_calls;
    return std::string("sig");
  });
  (void)relay->PollInbox("account:me", "");
  EXPECT_EQ(signed_calls, 1) << "the client taken before signs with the signer given after";
  EXPECT_EQ(Asked(org_, "/v1/inbox/poll"), 1u);
  Apply({OrgRelay(), OtherRelay()});
  EXPECT_EQ(clients_.Relay(), relay) << "kept";
  (void)relay->PollInbox("account:me", "");
  EXPECT_EQ(signed_calls, 2);
}

TEST_F(BackendClientSetOriginTest, TheBriefLlmKeyFollowsTheOrgsRelayWhenOnlyTheOrgChanges) {
  // Our list stays (org's relay preferred, the other after it): their clients are kept, but the org's relay moves to
  // the other one. Which client keeps the key is decided again at every Apply, not when a client was built.
  Apply({OrgRelay(), OtherRelay()});
  IRegistrationClient* preferred = clients_.Registration().get();
  ASSERT_EQ(FinishedKey(), "brf_llm_x");

  auto endpoints = At(OtherRelay(), OtherRelay(), OtherRelay());
  endpoints.our_relays = {OrgRelay(), OtherRelay()};
  clients_.Apply(endpoints);
  ASSERT_EQ(clients_.Registration().get(), preferred) << "the preferred relay's client is kept";
  EXPECT_EQ(FinishedKey(), "") << "no longer the org's relay: its key is not kept";
  ASSERT_EQ(clients_.OurRelays().size(), 2u);
  auto org_key =
      clients_.OurRelays()[1].registration->FinishRegistration("c", "pk", "me", kSignedFinish, "ml-dsa-65", "kem");
  ASSERT_TRUE(org_key) << org_key.error().message;
  EXPECT_EQ(org_key->llm_token.Reveal(), "brf_llm_x") << "the relay that is now the org's keeps it";
}

TEST_F(BackendClientSetOriginTest, StreamHistoryFallsToThePreferredRelayOnceTheOrgsIsNotOurs) {
  Apply({OtherRelay()});
  EXPECT_EQ(clients_.HistoryRelay(), clients_.Relay());
  EXPECT_NE(clients_.Locker(), nullptr) << "lockers stay at the org's registration URL";
  ASSERT_TRUE(clients_.ClientCompat()->Fetch());
  EXPECT_EQ(Asked(org_, "/v1/client-compat"), 1u);
}

// The directory asks our relays (multi-relay R2): an Account ID search every one of them (`users/by-account`, the legacy
// `GET /search` never), merged, the preferred relay's first; a lookup in order; the card a lookup answer carries goes
// to DirectoryCards.
class BackendClientSetDirectoryTest : public ::testing::Test {
protected:
  using Reply = test::ScriptedHttpServer::Reply;
  using Rows = std::map<std::string, std::string>; // account → its directory row

  static std::string Row(const std::string& account, const std::string& nickname) {
    return R"({"account_id":")" + account + R"(","nickname":")" + nickname + R"("})";
  }
  static std::string Encoded(const std::string& account) {
    return "account%3A" + account.substr(std::string("account:").size());
  }
  void Serve(test::ScriptedHttpServer& server, Rows rows) {
    server.SetHandler([rows = std::move(rows)](const test::ScriptedHttpServer::Request& request) {
      constexpr std::string_view kLookup = "/api/relay/v1/users/by-account/";
      if (request.target.rfind(kLookup, 0) == 0) {
        for (const auto& [account, row] : rows) {
          if (request.target.substr(kLookup.size()) == Encoded(account)) {
            return Reply{.status = 200, .headers = {}, .body = row};
          }
        }
        return Reply{.status = 404, .headers = {}, .body = R"({"code":"not_registered"})"};
      }
      return Reply{.status = 404, .headers = {}, .body = "{}"};
    });
  }
  static size_t Asked(const test::ScriptedHttpServer& server, const std::string& path) {
    size_t asked = 0;
    for (const auto& request : server.Requests()) {
      asked += request.target.find(path) != std::string::npos ? 1 : 0;
    }
    return asked;
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
  bool local_allowed_ = [this]() {
    clients_.SetLocalRelaysAllowed(true); // the scripted relays are on loopback
    return true;
  }();
};

TEST_F(BackendClientSetDirectoryTest, AnAccountSearchAsksOurOtherRelaysTooThePreferredFirst) {
  Serve(preferred_, {{"account:bob", Row("account:bob", "bob")}});
  Serve(other_, {{"account:bob", Row("account:bob", "bobby")}, {"account:cat", Row("account:cat", "cat")}});
  ApplyBoth();
  auto hits = directory_.SearchPeople("account:bob");
  ASSERT_TRUE(hits) << hits.error().message;
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ(hits->at(0).nickname, "bob") << "the preferred relay's row first";
  hits = directory_.SearchPeople("account:cat");
  ASSERT_TRUE(hits) << hits.error().message;
  ASSERT_EQ(hits->size(), 1u) << "not on the preferred relay: another of ours has it";
  EXPECT_EQ(hits->at(0).nickname, "cat");

  auto endpoints =
      At(preferred_.Origin() + "/api/relay", preferred_.Origin() + "/api/relay", preferred_.Origin() + "/api/relay");
  clients_.Apply(endpoints); // the other relay taken out of the config
  hits = directory_.SearchPeople("account:cat");
  ASSERT_TRUE(hits);
  EXPECT_TRUE(hits->empty()) << "only the relays of ours are asked";
  EXPECT_EQ(Asked(preferred_, "/v1/search") + Asked(other_, "/v1/search"), 0u);
}

TEST_F(BackendClientSetDirectoryTest, ANameMatchesNothingOnARelayAndIsNotSent) {
  // The relay's directory matches an exact Account ID only (SPECS): a name has no hit there, so it is asked nowhere.
  Serve(preferred_, {{"account:bob", Row("account:bob", "bob")}});
  Serve(other_, {});
  ApplyBoth();
  auto hits = directory_.SearchPeople("bob");
  ASSERT_TRUE(hits) << hits.error().message;
  EXPECT_TRUE(hits->empty());
  EXPECT_TRUE(preferred_.Requests().empty());
  EXPECT_TRUE(other_.Requests().empty());
}

TEST_F(BackendClientSetDirectoryTest, ANodeIsNoSearchHit) {
  Serve(preferred_, {{"account:node", R"({"account_id":"account:node","nickname":"n1","entity_kind":"mesh_node"})"}});
  Serve(other_, {});
  ApplyBoth();
  auto hits = directory_.SearchPeople("account:node");
  ASSERT_TRUE(hits) << hits.error().message;
  EXPECT_TRUE(hits->empty()) << "a search finds people, as the legacy search did";
}

TEST_F(BackendClientSetDirectoryTest, ALookupGoesOnToTheNextRelayAndItsCardIsGiven) {
  Serve(preferred_, {});
  Serve(other_, {{"account:cat", R"({"account_id":"account:cat","nickname":"cat","account_card":{"issued_at":5}})"}});
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
  Serve(preferred_, {{"account:cat", Row("account:cat", "preferred cat")}});
  Serve(other_, {{"account:cat", Row("account:cat", "cat")}});
  clients_.Apply(
      At(preferred_.Origin() + "/api/relay", other_.Origin() + "/api/relay", preferred_.Origin() + "/api/relay"));
  auto hits = directory_.SearchPeople("account:cat");
  ASSERT_TRUE(hits);
  ASSERT_EQ(hits->size(), 1u);
  EXPECT_EQ(hits->at(0).nickname, "cat");
  EXPECT_TRUE(preferred_.Requests().empty()) << "the configured directory is asked instead of the preferred relay's";
}

} // namespace
} // namespace pbr
