// Relay stream ids: opaque keys the relay indexes records by. A group copy's stream names no group (G018 item 7).
#include "common/chat/RelayStreamKey.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

const std::string kGroup = "group:550e8400-e29b-41d4-a716-446655440000";

TEST(RelayStreamKeyTest, AGroupStreamIsAHashOfTheGroupAndThePairFromOneDevice) {
  // BLAKE2b-128 over LenUtf8 of the tag, the group id, then the pair's accounts, lower first (WIRE_SCHEMAS).
  EXPECT_EQ(BuildGroupRelayStreamKey(kGroup, "account:alice", "account:bob", "QmAlicePhone"),
            "v1:g:3623fab6a219a3dcf495917aae0294ce:from:QmAlicePhone");
}

TEST(RelayStreamKeyTest, AGroupStreamNamesNoGroupOrAccount) {
  const std::string stream = BuildGroupRelayStreamKey(kGroup, "account:alice", "account:bob", "QmAlicePhone");
  EXPECT_EQ(stream.find("550e8400"), std::string::npos);
  EXPECT_EQ(stream.find("group"), std::string::npos);
  EXPECT_EQ(stream.find("account:"), std::string::npos);
}

TEST(RelayStreamKeyTest, BothEndsOfAPairComputeTheSameGroupStream) {
  EXPECT_EQ(BuildGroupRelayStreamKey(kGroup, "account:alice", "account:bob", "QmAlicePhone"),
            BuildGroupRelayStreamKey(kGroup, "account:bob", "account:alice", "QmAlicePhone"))
      << "the receiver computes it with the accounts the other way round";
}

TEST(RelayStreamKeyTest, GroupStreamsDifferPerPairGroupAndSendingDevice) {
  const std::string alice_bob = BuildGroupRelayStreamKey(kGroup, "account:alice", "account:bob", "QmAlicePhone");
  EXPECT_NE(alice_bob, BuildGroupRelayStreamKey(kGroup, "account:alice", "account:carol", "QmAlicePhone"))
      << "alice to bob and alice to carol in one group look unrelated";
  EXPECT_NE(alice_bob, BuildGroupRelayStreamKey(kGroup + "x", "account:alice", "account:bob", "QmAlicePhone"))
      << "another group";
  EXPECT_NE(alice_bob, BuildGroupRelayStreamKey(kGroup, "account:alice", "account:bob", "QmAliceLaptop"))
      << "each sending device has its own stream (the relay keeps one row per sender, stream and index)";
  // The fields are length-prefixed: none runs into the next.
  EXPECT_NE(BuildGroupRelayStreamKey("group:a", "b", "c", "QmD"), BuildGroupRelayStreamKey("group:ab", "", "c", "QmD"));
}

} // namespace
} // namespace pbr
