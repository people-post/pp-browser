#include "lib/net/OurRelaysStore.h"

#include "lib/net/RelayOrigins.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// My relays (multi-relay R3, MR006; docs/contracts/DATA_LAYOUT.md `our_relays.json`): user data in the profile,
// seeded once from the deployment's relay and `relay.extra_base_urls`, each relay with its last registration.

constexpr const char kA[] = "https://a.example/api/relay";
constexpr const char kB[] = "https://b.example/api/relay";
constexpr const char kC[] = "https://c.example/api/relay";
constexpr const char kD[] = "https://d.example/api/relay";
constexpr const char kE[] = "https://e.example/api/relay";
constexpr const char kIdA[] = "relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
constexpr const char kIdB[] = "relay:AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE";
constexpr const char kIdC[] = "relay:AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgI";
constexpr const char kPeerId[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";

RelayId Id(const char* text) {
  return RelayId::Parse(text).value();
}

using Origins = std::vector<std::string>;

OurRelaysList ListOf(const Origins& origins) {
  OurRelaysList list;
  for (const std::string& origin : origins) {
    list.relays.push_back(OurRelayEntry{.origin = origin});
  }
  return list;
}

std::string ProblemOf(const Roe<OurRelaysList>& changed) {
  return changed ? std::string("ok") : changed.error().message;
}

class OurRelaysStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() / ("pp_our_relays_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path dir_;
};

TEST_F(OurRelaysStoreTest, ANewProfileIsSeededFromTheDeploymentsRelayAndTheConfigsOthers) {
  OurRelaysStore store(dir_.string());
  EXPECT_EQ(store.LoadOrSeed(kA, RelayId{}, {kB, "not a url", kA}, true).Origins(), Origins({kA, kB}));
  EXPECT_TRUE(std::filesystem::exists(dir_ / "our_relays.json")) << "the seed is kept: the user's list from now on";
}

TEST_F(OurRelaysStoreTest, OnceKeptTheConfigIsOnlyANewProfilesDefault) {
  {
    OurRelaysStore store(dir_.string());
    store.LoadOrSeed(kA, RelayId{}, {}, true);
    ASSERT_TRUE(static_cast<bool>(store.Replace(ListOf({kC, kA}))));
  }
  OurRelaysStore again(dir_.string());
  EXPECT_EQ(again.LoadOrSeed(kA, RelayId{}, {kB}, true).Origins(), Origins({kC, kA}))
      << "the config's others do not come back";
}

TEST_F(OurRelaysStoreTest, NoDeploymentRelaySeedsNothingAndKeepsNoFile) {
  OurRelaysStore store(dir_.string());
  EXPECT_TRUE(store.LoadOrSeed("", RelayId{}, {kB}, true).relays.empty());
  EXPECT_FALSE(std::filesystem::exists(dir_ / "our_relays.json"));
}

TEST_F(OurRelaysStoreTest, AnUnreadableFileIsSeededAgain) {
  std::ofstream(dir_ / "our_relays.json") << "not json";
  OurRelaysStore store(dir_.string());
  EXPECT_EQ(store.LoadOrSeed(kA, RelayId{}, {}, true).Origins(), Origins({kA}));
  OurRelaysStore again(dir_.string());
  EXPECT_EQ(again.LoadOrSeed(kB, RelayId{}, {}, true).Origins(), Origins({kA})) << "the seed was kept";
}

// D9 (DATA_LAYOUT: a newer file is refused): a newer app's list is never overwritten, and nothing changes it.
TEST_F(OurRelaysStoreTest, ANewerAppsFileIsRefusedAndNeverWritten) {
  const std::string newer = R"({"schema_version": 9, "relays": [{"origin": "https://z.example/api/relay"}]})";
  std::ofstream(dir_ / "our_relays.json") << newer;
  OurRelaysStore store(dir_.string());
  EXPECT_EQ(store.LoadOrSeed(kA, RelayId{}, {}, true).Origins(), Origins({kA})) << "this run uses the deployment's";
  EXPECT_FALSE(static_cast<bool>(store.Replace(ListOf({kA, kB})))) << "changes are refused";
  store.Record(kA, RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Registered, .at_ms = 1});
  std::ifstream in(dir_ / "our_relays.json");
  const std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(kept, newer);
}

// T8: a change works from a snapshot taken before it: a registration answer kept meanwhile is not undone by it.
TEST_F(OurRelaysStoreTest, AListTakenBeforeARecordNeverUndoesThatRecord) {
  OurRelaysStore store(dir_.string());
  store.LoadOrSeed(kA, RelayId{}, {kB}, true);
  store.Record(kB, RelayRegistrationRecord{.state = RelayRegistrationRecord::State::Failed, .at_ms = 1});
  const OurRelaysList snapshot = store.List();
  const RelayRegistrationRecord answered{.state = RelayRegistrationRecord::State::Registered, .at_ms = 2};
  store.Record(kB, answered); // a registration answered while the change ran
  ASSERT_TRUE(static_cast<bool>(store.Replace(snapshot)));
  EXPECT_EQ(store.List().relays[1].registration, answered);
}

TEST_F(OurRelaysStoreTest, EachRelaysLastRegistrationIsKeptAcrossARestart) {
  const RelayRegistrationRecord registered{.state = RelayRegistrationRecord::State::Registered,
                                           .expires_at = "2099-01-01T00:00:00.000Z",
                                           .card_issued_at = 7,
                                           .at_ms = 100};
  const RelayRegistrationRecord failed{
      .state = RelayRegistrationRecord::State::Failed, .reason = "HTTP 503", .at_ms = 200};
  {
    OurRelaysStore store(dir_.string());
    store.LoadOrSeed(kA, RelayId{}, {kB, kC}, true);
    store.Record(kB, registered);
    store.Record(kC, failed);
    store.Record(kD, registered); // not ours: nothing
  }
  OurRelaysStore again(dir_.string());
  const OurRelaysList list = again.LoadOrSeed(kA, RelayId{}, {}, true);
  ASSERT_EQ(list.relays.size(), 3u);
  EXPECT_EQ(list.relays[0].registration.state, RelayRegistrationRecord::State::Unknown);
  EXPECT_EQ(list.relays[1].registration, registered);
  EXPECT_EQ(list.relays[2].registration, failed);
}

TEST_F(OurRelaysStoreTest, AReorderedListKeepsEachRelaysRecord) {
  OurRelaysStore store(dir_.string());
  store.LoadOrSeed(kA, RelayId{}, {kB}, true);
  const RelayRegistrationRecord registered{.state = RelayRegistrationRecord::State::Registered, .at_ms = 1};
  store.Record(kB, registered);
  ASSERT_TRUE(static_cast<bool>(store.Replace(ListOf({kB, kA}))));
  EXPECT_EQ(store.List().relays[0].registration, registered);
}

TEST_F(OurRelaysStoreTest, AnEmptyListIsRefused) {
  OurRelaysStore store(dir_.string());
  store.LoadOrSeed(kA, RelayId{}, {}, true);
  EXPECT_FALSE(static_cast<bool>(store.Replace(OurRelaysList{})));
  EXPECT_EQ(store.List().Origins(), Origins({kA}));
}

// The list's rules (MR004 / MR006): at most four, none twice, the preferred one not removable, never empty.
TEST(OurRelaysListTest, ARelayIsAddedLastOnceAndAtMostFourInAll) {
  auto added = WithRelayAdded(ListOf({kA, kB}), kC, Id(kIdC));
  ASSERT_TRUE(added);
  EXPECT_EQ(added->Origins(), Origins({kA, kB, kC}));
  EXPECT_EQ(added->relays.back().relay_id, Id(kIdC)) << "added by its address: its relay id with it (MR008)";
  EXPECT_EQ(ProblemOf(WithRelayAdded(ListOf({kA, kB}), "https://B.example:443/api/relay", Id(kIdB))),
            "relay_already_listed");
  EXPECT_EQ(ProblemOf(WithRelayAdded(ListOf({kA, kB, kC, kD}), kE, RelayId{})), "relay_list_full");
}

TEST(OurRelaysListTest, ThePreferredRelayAndTheLastAreNotRemovable) {
  auto removed = WithRelayRemoved(ListOf({kA, kB, kC}), kB);
  ASSERT_TRUE(removed);
  EXPECT_EQ(removed->Origins(), Origins({kA, kC}));
  EXPECT_EQ(ProblemOf(WithRelayRemoved(ListOf({kA, kB}), kA)), "relay_preferred_not_removable");
  EXPECT_EQ(ProblemOf(WithRelayRemoved(ListOf({kA}), kA)), "relay_list_last");
  EXPECT_EQ(ProblemOf(WithRelayRemoved(ListOf({kA, kB}), kC)), "relay_not_listed");
}

TEST(OurRelaysListTest, MarkingPreferredMovesItFirstTheOthersInTheirOrder) {
  auto preferred = WithRelayPreferred(ListOf({kA, kB, kC}), kC);
  ASSERT_TRUE(preferred);
  EXPECT_EQ(preferred->Origins(), Origins({kC, kA, kB}));
  EXPECT_EQ(ProblemOf(WithRelayPreferred(ListOf({kA}), kB)), "relay_not_listed");
}

TEST(OurRelaysListTest, AProblemCodeNamesItsProblem) {
  EXPECT_EQ(RelayListProblemFromCode("relay_list_full"), RelayListProblem::Full);
  EXPECT_EQ(RelayListProblemFromCode("something else"), RelayListProblem::None);
}

// MR008 (SIGNED_FORMATS § 3.3): a relay of ours is kept with the relay id its address names; the seed's org relay
// carries the build's (or the config's), the config's other relays are relay links, and a bare one is seeded in
// developer builds only.
TEST_F(OurRelaysStoreTest, TheSeedCarriesEachRelaysIdFromItsAddress) {
  OurRelaysStore store(dir_.string());
  const std::string link_b = std::string(kB) + "#" + kIdB;
  const OurRelaysList list = store.LoadOrSeed(kA, Id(kIdA), {link_b, kC, std::string(kD) + "#" + kPeerId}, false);
  ASSERT_EQ(list.Origins(), Origins({kA, kB})) << "a bare extra outside developer builds, or a PeerId: left out";
  EXPECT_EQ(list.relays[0].relay_id, Id(kIdA));
  EXPECT_EQ(list.relays[1].relay_id, Id(kIdB));

  OurRelaysStore again(dir_.string());
  const OurRelaysList kept = again.LoadOrSeed(kA, RelayId{}, {}, false);
  ASSERT_EQ(kept.relays.size(), 2u);
  EXPECT_EQ(kept.relays[1].relay_id, Id(kIdB)) << "kept with the list";

  OurRelaysStore developer((dir_ / "dev").string());
  EXPECT_EQ(developer.LoadOrSeed(kA, RelayId{}, {kC}, true).Origins(), Origins({kA, kC}))
      << "a developer build seeds a bare one (checked on first use)";
}

TEST_F(OurRelaysStoreTest, ThePeerIdListShapeIsNotReadAndAnEntryNamingNoRelayIdIsDropped) {
  std::ofstream(dir_ / "our_relays.json")
      << R"({"schema_version":2,"relays":[{"origin":"https://z.example/api/relay","peer_id":")" << kPeerId << R"("}]})";
  OurRelaysStore store(dir_.string());
  EXPECT_EQ(store.LoadOrSeed(kA, Id(kIdA), {}, false).Origins(), Origins({kA})) << "seeded again (no bridge)";
  EXPECT_FALSE(OurRelaysFromJson(R"({"schema_version":3,"relays":[{"origin":"https://a.example/api/relay",)"
                                 R"("relay_id":"relay:nope"}]})"))
      << "an address whose relay id is not one";
}

// What the user types as a relay (My relays' Add): an https origin, or its base URL; nothing else.
TEST(ParseRelayOriginTest, AnOriginOrItsBaseUrlIsTheRelaysBaseUrl) {
  EXPECT_EQ(ParseRelayOrigin("  https://Relay.Example.org/ ", false).base_url, "https://relay.example.org/api/relay");
  EXPECT_EQ(ParseRelayOrigin("https://relay.example.org:8443/api/relay/", false).base_url,
            "https://relay.example.org:8443/api/relay");
}

TEST(ParseRelayOriginTest, WhatIsNotAnHttpsOriginIsRefusedWithItsReason) {
  EXPECT_EQ(ParseRelayOrigin("", false).problem, RelayOriginProblem::Empty);
  EXPECT_EQ(ParseRelayOrigin("relay.example.org", false).problem, RelayOriginProblem::NotAUrl);
  EXPECT_EQ(ParseRelayOrigin("ftp://relay.example.org", false).problem, RelayOriginProblem::NotAUrl);
  EXPECT_EQ(ParseRelayOrigin("https://user@relay.example.org", false).problem, RelayOriginProblem::NotAUrl);
  EXPECT_EQ(ParseRelayOrigin("http://relay.example.org", false).problem, RelayOriginProblem::NotHttps);
  EXPECT_EQ(ParseRelayOrigin("https://relay.example.org/other", false).problem, RelayOriginProblem::HasPath);
  EXPECT_EQ(ParseRelayOrigin("https://relay.example.org/?x=1", false).problem, RelayOriginProblem::HasQuery);
  EXPECT_EQ(ParseRelayOrigin("https://relay.example.org#top", false).problem, RelayOriginProblem::HasQuery);
  EXPECT_EQ(ParseRelayOrigin("https://" + std::string(120, 'a') + ".org", false).problem, RelayOriginProblem::TooLong);
}

TEST(ParseRelayOriginTest, PlainHttpOnlyInDeveloperBuilds) {
  EXPECT_EQ(ParseRelayOrigin("http://127.0.0.1:8080", true).base_url, "http://127.0.0.1:8080/api/relay");
}

} // namespace
} // namespace pbr

namespace pbr {
namespace {

// MR008, SIGNED_FORMATS § 3.3: a relay link is its base URL and its relay id after `#` (the fragment never reaches the
// relay); a PeerId after `#` (the links before the cutover) is no relay id.
TEST(ParseRelayLinkTest, ALinkNamesTheRelaysBaseUrlAndItsRelayId) {
  const ParsedRelayOrigin link = ParseRelayLink(std::string("https://Relay.Example.org/api/relay#") + kIdA, false);
  EXPECT_EQ(link.problem, RelayOriginProblem::None);
  EXPECT_EQ(link.base_url, "https://relay.example.org/api/relay");
  EXPECT_EQ(link.relay_id, Id(kIdA));
  EXPECT_EQ(RelayLink(link.base_url, link.relay_id), std::string("https://relay.example.org/api/relay#") + kIdA);
  EXPECT_EQ(ParseRelayLink(std::string("https://relay.example.org#") + kIdA + " \n", false).relay_id, Id(kIdA))
      << "trailing spaces after the id";
  const ParsedRelayOrigin bare = ParseRelayLink("https://relay.example.org", false);
  EXPECT_EQ(bare.problem, RelayOriginProblem::None);
  EXPECT_TRUE(bare.relay_id.empty()) << "a bare origin: the caller decides (developer builds only)";
  EXPECT_EQ(RelayLink(bare.base_url, bare.relay_id), "https://relay.example.org/api/relay");
  EXPECT_EQ(ParseRelayLink(std::string("https://relay.example.org#") + kPeerId, false).problem,
            RelayOriginProblem::BadRelayId);
  EXPECT_EQ(ParseRelayLink("https://relay.example.org#relay:short", false).problem, RelayOriginProblem::BadRelayId);
  EXPECT_EQ(ParseRelayLink("https://relay.example.org#", false).problem, RelayOriginProblem::BadRelayId);
  EXPECT_EQ(ParseRelayLink(std::string("http://relay.example.org#") + kIdA, false).problem,
            RelayOriginProblem::NotHttps);
  EXPECT_EQ(ParseRelayLink(std::string("http://127.0.0.1:8787/api/relay#") + kIdA, true).relay_id, Id(kIdA))
      << "the lab's plain relays, in developer builds";
  EXPECT_EQ(OurRelayOrigins(kA, {std::string(kB) + "#" + kIdB}), Origins({kA, kB})) << "a link's origin";
}

} // namespace
} // namespace pbr
