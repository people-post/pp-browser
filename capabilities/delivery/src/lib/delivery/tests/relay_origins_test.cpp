#include "lib/delivery/RelayOrigins.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Our relays (multi-relay R2, docs/ops/CONFIGURATION.md `relay.extra_base_urls`): the backend's first, then the
// configured others in order, at most four in all (MR004), no relay twice.

using Origins = std::vector<std::string>;

TEST(RelayOriginsTest, ThePreferredRelayComesFirstThenTheOthersInOrder) {
  EXPECT_EQ(
      OurRelayOrigins("https://a.example/api/relay", {"https://b.example/api/relay", "https://c.example/api/relay"}),
      Origins({"https://a.example/api/relay", "https://b.example/api/relay", "https://c.example/api/relay"}));
}

TEST(RelayOriginsTest, AtMostFourRelaysInAll) {
  EXPECT_EQ(OurRelayOrigins("https://a.example",
                            {"https://b.example", "https://c.example", "https://d.example", "https://e.example"}),
            Origins({"https://a.example", "https://b.example", "https://c.example", "https://d.example"}));
}

TEST(RelayOriginsTest, ARelayListedTwiceOrNotAUrlIsLeftOut) {
  EXPECT_EQ(OurRelayOrigins("https://a.example/api/relay",
                            {"https://A.example:443/api/relay/", "relay.example", "", "ftp://x.example",
                             "https:///no-host", "https://q.example/?x=1", " https://b.example/api/relay/ "}),
            Origins({"https://a.example/api/relay", "https://b.example/api/relay"}));
}

TEST(RelayOriginsTest, NoPreferredRelayMeansNone) {
  EXPECT_TRUE(OurRelayOrigins("", {"https://b.example"}).empty());
}

TEST(RelayOriginsTest, TheSameRelayWrittenDifferently) {
  EXPECT_TRUE(SameRelay("https://Relay.Example:443/api/relay/", "https://relay.example/api/relay"));
  EXPECT_TRUE(SameRelay("http://127.0.0.1:80/api/relay", "http://127.0.0.1/api/relay"));
  EXPECT_FALSE(SameRelay("https://relay.example:8443/api/relay", "https://relay.example/api/relay"));
  EXPECT_FALSE(SameRelay("http://relay.example/api/relay", "https://relay.example/api/relay"));
  EXPECT_FALSE(SameRelay("https://relay.example/api/Relay", "https://relay.example/api/relay")) << "paths keep case";
}

} // namespace
} // namespace pbr
