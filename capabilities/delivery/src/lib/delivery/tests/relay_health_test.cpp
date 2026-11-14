#include "lib/delivery/RelayHealth.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// The per-relay health memory of sending in list order (multi-relay R2): a relay that failed lately goes after the
// healthy ones, for a bounded time; the list's order holds within each group.

using Origins = std::vector<std::string>;

TEST(RelayHealthTest, AllHealthyKeepsTheListsOrder) {
  RelayHealth health;
  EXPECT_EQ(health.Order({"https://a", "https://b", "https://c"}, 0), Origins({"https://a", "https://b", "https://c"}));
}

TEST(RelayHealthTest, ARecentFailureGoesToTheBackForABoundedTime) {
  RelayHealth health;
  health.NoteFailed("https://a", 1000);
  health.NoteFailed("https://b", 1000);
  EXPECT_EQ(health.Order({"https://a", "https://b", "https://c"}, 1000),
            Origins({"https://c", "https://a", "https://b"}));
  EXPECT_EQ(health.Order({"https://a", "https://b", "https://c"}, 1000 + RelayHealth::kRelayFailedForMs),
            Origins({"https://a", "https://b", "https://c"}));
}

TEST(RelayHealthTest, ASuccessClearsAFailure) {
  RelayHealth health;
  health.NoteFailed("https://A/", 1000);
  EXPECT_TRUE(health.FailedRecently("https://a", 1001)) << "by origin, as SameRelay compares";
  health.NoteOk("https://a");
  EXPECT_FALSE(health.FailedRecently("https://a", 1001));
}

TEST(RelayHealthTest, TheMemoryIsBounded) {
  RelayHealth health;
  for (size_t i = 0; i <= RelayHealth::kMaxRemembered; ++i) {
    health.NoteFailed("https://r" + std::to_string(i), static_cast<int64_t>(1000 + i));
  }
  EXPECT_FALSE(health.FailedRecently("https://r0", 2000)) << "the oldest is forgotten first";
  EXPECT_TRUE(health.FailedRecently("https://r1", 2000));
}

} // namespace
} // namespace pbr
