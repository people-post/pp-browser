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
  EXPECT_EQ(contact.server_nickname, "alice");
  EXPECT_EQ(contact.trust, TrustLevel::Friendly);
  ASSERT_EQ(contact.ids.size(), 2u);
  EXPECT_EQ(contact.ids[0].kind, ContactIdKind::RelayUser);
  EXPECT_EQ(contact.ids[0].value, "relay:alice123");
  EXPECT_TRUE(contact.ids[0].primary);
  EXPECT_EQ(contact.ids[1].kind, ContactIdKind::PeerId);
  EXPECT_EQ(contact.ids[1].value, "12D3KooWPeer");
  ASSERT_EQ(contact.multiaddrs.size(), 1u);
  EXPECT_EQ(contact.multiaddrs[0], "/ip4/127.0.0.1/tcp/4001/p2p/12D3KooWPeer");
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
  hit.app_version = "0.4.2";
  hit.protocol_gen = 2;
  hit.min_peer_protocol_gen = 1;

  const DirectoryHit again = DirectoryHitFromJson(DirectoryHitToJson(hit));
  EXPECT_EQ(again.hit_id, hit.hit_id);
  ASSERT_TRUE(again.signing_public_key_b64.has_value());
  EXPECT_EQ(*again.signing_public_key_b64, "sig");
  ASSERT_TRUE(again.kem_public_key_b64.has_value());
  EXPECT_EQ(*again.kem_public_key_b64, "kem");
  ASSERT_TRUE(again.app_version.has_value());
  EXPECT_EQ(*again.app_version, "0.4.2");
  ASSERT_TRUE(again.protocol_gen.has_value());
  EXPECT_EQ(*again.protocol_gen, 2);
  ASSERT_TRUE(again.min_peer_protocol_gen.has_value());
  EXPECT_EQ(*again.min_peer_protocol_gen, 1);
}

} // namespace
