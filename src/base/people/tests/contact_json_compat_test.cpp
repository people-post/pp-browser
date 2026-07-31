#include "base/people/ContactJson.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

using namespace pbr;

TEST(ContactJsonCompat, ParsesLegacyFlatContactFields) {
  const nlohmann::json json = {{"id", "c1"},
                               {"display_name", "Alice"},
                               {"nickname", "alice"},
                               {"relay_user_id", "relay:alice123"},
                               {"peer_id", "12D3KooWPeer"},
                               {"multiaddr", "/ip4/127.0.0.1/tcp/4001/p2p/12D3KooWPeer"},
                               {"trust", "friendly"}};

  const Contact contact = ContactFromJson(json);
  EXPECT_EQ(contact.id, "c1");
  EXPECT_EQ(contact.display_name, "Alice");
  EXPECT_EQ(contact.local.display_name, "Alice");
  EXPECT_EQ(contact.server_nickname, "alice");
  EXPECT_EQ(contact.remote.nickname, "alice");
  EXPECT_EQ(contact.trust, TrustLevel::Friendly);
  EXPECT_EQ(contact.local.trust, TrustLevel::Friendly);
  ASSERT_EQ(contact.ids.size(), 2u);
  EXPECT_EQ(contact.ids[0].kind, ContactIdKind::RelayUser);
  EXPECT_EQ(contact.ids[0].value, "relay:alice123");
  EXPECT_TRUE(contact.ids[0].primary);
  EXPECT_EQ(contact.ids[1].kind, ContactIdKind::PeerId);
  EXPECT_EQ(contact.ids[1].value, "12D3KooWPeer");
  ASSERT_EQ(contact.multiaddrs.size(), 1u);
  EXPECT_EQ(contact.multiaddrs[0], "/ip4/127.0.0.1/tcp/4001/p2p/12D3KooWPeer");
  EXPECT_EQ(contact.remote.fetched_at, 0);
}

TEST(ContactJsonCompat, NestedRoundTripPreservesLocalRemote) {
  Contact contact;
  contact.id = "c2";
  contact.local.display_name = "Local Alice";
  contact.local.trust = TrustLevel::Friendly;
  contact.remote.nickname = "alice";
  contact.remote.ids = {{ContactIdKind::RelayUser, "relay:alice", true}};
  contact.remote.multiaddrs = {"/ip4/1.2.3.4/tcp/1/p2p/12D3"};
  contact.remote.fetched_at = 1700000000000;
  SyncContactMirrors(contact);

  const Contact again = ContactFromJson(ContactToJson(contact));
  EXPECT_EQ(again.local.display_name, "Local Alice");
  EXPECT_EQ(again.remote.nickname, "alice");
  EXPECT_EQ(again.remote.fetched_at, 1700000000000);
  ASSERT_EQ(again.remote.ids.size(), 1u);
  EXPECT_EQ(again.remote.multiaddrs.size(), 1u);
  EXPECT_EQ(again.display_name, "Local Alice");
  const nlohmann::json dumped = ContactToJson(again);
  EXPECT_TRUE(dumped.contains("overrides"));
  EXPECT_TRUE(dumped["overrides"].is_object());
  EXPECT_TRUE(dumped["overrides"].empty());
}

TEST(ContactJsonCompat, ParsesDirectoryWireShapeWithRelayUserId) {
  const nlohmann::json json = {{"relay_user_id", "relay:demon"},
                               {"nickname", "demon"},
                               {"signing_public_key_b64", "sigkey"},
                               {"kem_public_key_b64", "kemkey"}};

  const DirectoryHit hit = DirectoryHitFromJson(json);
  EXPECT_EQ(hit.hit_id, "relay:demon");
  EXPECT_EQ(hit.nickname, "demon");
  EXPECT_EQ(hit.display_name, "demon");
  ASSERT_EQ(hit.ids.size(), 1u);
  EXPECT_EQ(hit.ids[0].value, "relay:demon");
  EXPECT_TRUE(hit.ids[0].primary);
  ASSERT_TRUE(hit.signing_public_key_b64.has_value());
  EXPECT_EQ(*hit.signing_public_key_b64, "sigkey");
  ASSERT_TRUE(hit.kem_public_key_b64.has_value());
  EXPECT_EQ(*hit.kem_public_key_b64, "kemkey");
}

TEST(ContactJsonCompat, DirectoryHitRoundTripPreservesKeys) {
  DirectoryHit hit;
  hit.hit_id = "relay:x";
  hit.display_name = "X";
  hit.nickname = "x";
  hit.ids = {{ContactIdKind::RelayUser, "relay:x", true}};
  hit.signing_public_key_b64 = "sig";
  hit.kem_public_key_b64 = "kem";

  const DirectoryHit again = DirectoryHitFromJson(DirectoryHitToJson(hit));
  EXPECT_EQ(again.hit_id, hit.hit_id);
  ASSERT_TRUE(again.signing_public_key_b64.has_value());
  EXPECT_EQ(*again.signing_public_key_b64, "sig");
  ASSERT_TRUE(again.kem_public_key_b64.has_value());
  EXPECT_EQ(*again.kem_public_key_b64, "kem");
}

} // namespace
