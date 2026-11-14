#include "lib/net/OurRelaysStore.h"

#include "lib/net/RelayOrigins.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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
  EXPECT_EQ(store.LoadOrSeed(kA, {kB, "not a url", kA}).Origins(), Origins({kA, kB}));
  EXPECT_TRUE(std::filesystem::exists(dir_ / "our_relays.json")) << "the seed is kept: the user's list from now on";
}

TEST_F(OurRelaysStoreTest, OnceKeptTheConfigIsOnlyANewProfilesDefault) {
  {
    OurRelaysStore store(dir_.string());
    store.LoadOrSeed(kA, {});
    ASSERT_TRUE(static_cast<bool>(store.Replace(ListOf({kC, kA}))));
  }
  OurRelaysStore again(dir_.string());
  EXPECT_EQ(again.LoadOrSeed(kA, {kB}).Origins(), Origins({kC, kA})) << "the config's others do not come back";
}

TEST_F(OurRelaysStoreTest, NoDeploymentRelaySeedsNothingAndKeepsNoFile) {
  OurRelaysStore store(dir_.string());
  EXPECT_TRUE(store.LoadOrSeed("", {kB}).relays.empty());
  EXPECT_FALSE(std::filesystem::exists(dir_ / "our_relays.json"));
}

TEST_F(OurRelaysStoreTest, AnUnreadableFileIsSeededAgain) {
  std::ofstream(dir_ / "our_relays.json") << R"({"version": 9, "relays": []})";
  OurRelaysStore store(dir_.string());
  EXPECT_EQ(store.LoadOrSeed(kA, {}).Origins(), Origins({kA}));
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
    store.LoadOrSeed(kA, {kB, kC});
    store.Record(kB, registered);
    store.Record(kC, failed);
    store.Record(kD, registered); // not ours: nothing
  }
  OurRelaysStore again(dir_.string());
  const OurRelaysList list = again.LoadOrSeed(kA, {});
  ASSERT_EQ(list.relays.size(), 3u);
  EXPECT_EQ(list.relays[0].registration.state, RelayRegistrationRecord::State::Unknown);
  EXPECT_EQ(list.relays[1].registration, registered);
  EXPECT_EQ(list.relays[2].registration, failed);
}

TEST_F(OurRelaysStoreTest, AReorderedListKeepsEachRelaysRecord) {
  OurRelaysStore store(dir_.string());
  store.LoadOrSeed(kA, {kB});
  const RelayRegistrationRecord registered{.state = RelayRegistrationRecord::State::Registered, .at_ms = 1};
  store.Record(kB, registered);
  ASSERT_TRUE(static_cast<bool>(store.Replace(ListOf({kB, kA}))));
  EXPECT_EQ(store.List().relays[0].registration, registered);
}

TEST_F(OurRelaysStoreTest, AnEmptyListIsRefused) {
  OurRelaysStore store(dir_.string());
  store.LoadOrSeed(kA, {});
  EXPECT_FALSE(static_cast<bool>(store.Replace(OurRelaysList{})));
  EXPECT_EQ(store.List().Origins(), Origins({kA}));
}

// The list's rules (MR004 / MR006): at most four, none twice, the preferred one not removable, never empty.
TEST(OurRelaysListTest, ARelayIsAddedLastOnceAndAtMostFourInAll) {
  auto added = WithRelayAdded(ListOf({kA, kB}), kC);
  ASSERT_TRUE(added);
  EXPECT_EQ(added->Origins(), Origins({kA, kB, kC}));
  EXPECT_EQ(ProblemOf(WithRelayAdded(ListOf({kA, kB}), "https://B.example:443/api/relay")), "relay_already_listed");
  EXPECT_EQ(ProblemOf(WithRelayAdded(ListOf({kA, kB, kC, kD}), kE)), "relay_list_full");
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
