#include "lib/people/ContactJson.h"
#include "common/ValueJson.h"
#include "common/directory/DirectoryJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/TypedBlob.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace {

using namespace pbr;

/** Typed keys of the right sizes (shape only: a row's keys are checked by the key card, not here). */
std::string TypedSigning(uint8_t fill = 0x42, uint8_t id = 1) {
  ByteVector blob = WriteTypedBlob(pp::BlobKind::kSignaturePublicKey, ByteVector(1952, fill)).value();
  blob[0] = id;
  return Base64Encode(blob);
}
std::string TypedKem(uint8_t fill = 0x43, uint8_t id = 1) {
  ByteVector blob = WriteTypedBlob(pp::BlobKind::kKemPublicKey, ByteVector(1184, fill)).value();
  blob[0] = id;
  return Base64Encode(blob);
}
std::string Binding(uint8_t version = 2, uint8_t signature_id = 1) {
  ByteVector signature = WriteTypedBlob(pp::BlobKind::kSignature, ByteVector(3309, 0x44)).value();
  signature[0] = signature_id;
  ByteVector blob = KemBindingBlob(signature);
  blob[0] = version;
  return Base64Encode(blob);
}

Object MustParseObject(const std::string& json_utf8) {
  auto parsed = TryParseObject(json_utf8);
  EXPECT_TRUE(parsed.has_value()) << json_utf8;
  return parsed.value_or(Object{});
}

// The flat row from before the local/remote split is not read: pre-release, no leftover contact is carried over.
TEST(ContactJsonCompat, AFlatRowFromBeforeTheSplitIsNotRead) {
  const Object json = MustParseObject(R"({
    "id": "c1",
    "display_name": "Alice",
    "nickname": "alice",
    "account_id": "account:alice123",
    "peer_id": "12D3KooWPeer",
    "multiaddr": "/ip4/127.0.0.1/tcp/4001/p2p/12D3KooWPeer",
    "trust": "friendly"
  })");

  const Contact contact = ContactFromJson(json);
  EXPECT_EQ(contact.id, "c1");
  EXPECT_TRUE(contact.local.display_name.empty());
  EXPECT_TRUE(contact.remote.nickname.empty());
  EXPECT_EQ(contact.local.trust, TrustLevel::Unknown);
  EXPECT_TRUE(contact.ids.empty());
  EXPECT_TRUE(contact.multiaddrs.empty());
}

TEST(ContactJsonCompat, NestedRoundTripPreservesLocalRemote) {
  Contact contact;
  contact.id = "c2";
  contact.local.display_name = "Local Alice";
  contact.local.trust = TrustLevel::Friendly;
  contact.remote.nickname = "alice";
  contact.remote.ids = {{mesh::ContactIdKind::Account, "account:alice", true}};
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
  const Object dumped = ContactToJson(again);
  ASSERT_TRUE(dumped.contains("overrides"));
  const Object* overrides = dumped.getObject("overrides");
  ASSERT_NE(overrides, nullptr);
  EXPECT_TRUE(overrides->empty());
}

// The relay's lookup row (`GET /users/by-account`, multi-relay MR001) names its account and nothing else.
TEST(ContactJsonCompat, ParsesTheLookupRowByItsAccountId) {
  Object json = MustParseObject(R"({
    "account_id": "account:demon",
    "relay_user_id": "relay:retired",
    "nickname": "demon"
  })");
  json.set("signing_public_key_b64", TypedSigning());
  json.set("kem_public_key_b64", TypedKem());
  json.set("kem_binding_b64", Binding());

  const mesh::DirectoryHit hit = DirectoryHitFromJson(json);
  EXPECT_EQ(hit.hit_id, "account:demon");
  EXPECT_EQ(hit.nickname, "demon");
  EXPECT_EQ(hit.display_name, "demon");
  ASSERT_EQ(hit.ids.size(), 1u) << "a retired relay id is no id";
  EXPECT_EQ(hit.ids[0].kind, mesh::ContactIdKind::Account);
  EXPECT_EQ(hit.ids[0].value, "account:demon");
  EXPECT_TRUE(hit.ids[0].primary);
  EXPECT_EQ(hit.signing_public_key_b64, TypedSigning());
  EXPECT_EQ(hit.kem_public_key_b64, TypedKem());
  EXPECT_EQ(hit.kem_binding_b64, Binding());
  EXPECT_FALSE(hit.keys_update_needed);
}

// A row's keys by the mesh's shape rule (pp-cpp-mesh MeshNodeHitCodec.h): a malformed or older field is dropped, one
// of an algorithm or binding version this build does not know is kept and flags the row "update needed".
TEST(ContactJsonCompat, ARowsKeysAreReadByTheirShape) {
  const auto read = [](const std::string& signing, const std::string& kem, const std::string& binding) {
    Object row;
    row.set("account_id", std::string("account:x"));
    row.set("signing_public_key_b64", signing);
    row.set("kem_public_key_b64", kem);
    row.set("kem_binding_b64", binding);
    return DirectoryHitFromJson(row);
  };
  const mesh::DirectoryHit ok = read(TypedSigning(), TypedKem(), Binding());
  EXPECT_TRUE(ok.signing_public_key_b64 && ok.kem_public_key_b64 && ok.kem_binding_b64);
  EXPECT_FALSE(ok.keys_update_needed);

  const mesh::DirectoryHit raw = read(Base64Encode(ByteVector(1952, 0x01)), "kemkey", Binding(1));
  EXPECT_FALSE(raw.signing_public_key_b64) << "an untyped key (id 1, the wrong size): malformed, dropped";
  EXPECT_FALSE(raw.kem_public_key_b64) << "not base64 of a key: dropped";
  EXPECT_FALSE(raw.kem_binding_b64) << "binding v1: older, dropped";
  EXPECT_FALSE(raw.keys_update_needed);

  for (const auto& [hit, what] :
       {std::pair{read(TypedSigning(0x42, 0x7f), TypedKem(), Binding()), "a signing key's algorithm"},
        std::pair{read(TypedSigning(), TypedKem(0x43, 0x7f), Binding()), "a KEM key's algorithm"},
        std::pair{read(TypedSigning(), TypedKem(), Binding(3)), "the binding's version"},
        std::pair{read(TypedSigning(), TypedKem(), Binding(2, 0x7f)), "the binding signature's algorithm"}}) {
    EXPECT_TRUE(hit.keys_update_needed) << what;
    EXPECT_TRUE(hit.signing_public_key_b64 && hit.kem_public_key_b64 && hit.kem_binding_b64)
        << what << ": kept as received";
  }
  const mesh::DirectoryHit zero = read(TypedSigning(0x42, 0), TypedKem(), Binding(2, 0));
  EXPECT_FALSE(zero.signing_public_key_b64 || zero.kem_binding_b64) << "id 0: malformed";
  EXPECT_FALSE(zero.keys_update_needed);
}

TEST(ContactJsonCompat, DirectoryHitRoundTripPreservesKeys) {
  mesh::DirectoryHit hit;
  hit.hit_id = "account:x";
  hit.display_name = "X";
  hit.nickname = "x";
  hit.ids = {{mesh::ContactIdKind::Account, "account:x", true}};
  hit.signing_public_key_b64 = TypedSigning();
  hit.kem_public_key_b64 = TypedKem();
  hit.initiation_floor = 55;
  hit.endpoints = {{"12D3KooWX", {"/ip4/1.2.3.4/tcp/1/p2p/12D3KooWX"}, 99}};

  const mesh::DirectoryHit again = DirectoryHitFromJson(DirectoryHitToJson(hit));
  EXPECT_EQ(again.hit_id, hit.hit_id);
  EXPECT_EQ(again.signing_public_key_b64, TypedSigning());
  EXPECT_EQ(again.kem_public_key_b64, TypedKem());
  EXPECT_EQ(again.initiation_floor, 55);
  ASSERT_EQ(again.endpoints.size(), 1u);
  EXPECT_EQ(again.endpoints[0].peer_id, "12D3KooWX");
  EXPECT_EQ(again.endpoints[0].updated_at, 99);
  ASSERT_EQ(again.multiaddrs.size(), 1u);
}

TEST(ContactJsonCompat, DirectoryHitIgnoresTopLevelPeerId) {
  const Object json = MustParseObject(R"({
    "account_id": "account:y",
    "nickname": "y",
    "peer_id": "12D3Legacy",
    "multiaddrs": ["/ip4/9.9.9.9/tcp/1"],
    "endpoints": [{
      "peer_id": "12D3New",
      "multiaddrs": ["/ip4/1.1.1.1/tcp/1"],
      "updated_at": 5
    }]
  })");
  const mesh::DirectoryHit hit = DirectoryHitFromJson(json);
  ASSERT_EQ(hit.endpoints.size(), 1u);
  EXPECT_EQ(hit.endpoints[0].peer_id, "12D3New");
  ASSERT_EQ(hit.multiaddrs.size(), 1u);
  EXPECT_EQ(hit.multiaddrs[0], "/ip4/1.1.1.1/tcp/1");
  bool saw_legacy = false;
  for (const mesh::ContactId& id : hit.ids) {
    if (id.kind == mesh::ContactIdKind::PeerId && id.value == "12D3Legacy") {
      saw_legacy = true;
    }
  }
  EXPECT_FALSE(saw_legacy);
}

TEST(ContactJsonCompat, DirectoryHitMissingFloorDefaultsZero) {
  const Object json = MustParseObject(R"({"account_id":"account:y","nickname":"y"})");
  const mesh::DirectoryHit hit = DirectoryHitFromJson(json);
  EXPECT_EQ(hit.initiation_floor, 0);
}

TEST(ContactJsonCompat, ParsesDirectoryIconAndRoundTripsContactRemote) {
  const Object json = MustParseObject(R"({
    "account_id": "account:alice",
    "nickname": "alice",
    "icon": {
      "url": "https://cdn.example/icon.jpg",
      "blob_id": "blob-1",
      "kind": "image/jpeg"
    }
  })");

  const mesh::DirectoryHit hit = DirectoryHitFromJson(json);
  ASSERT_TRUE(hit.icon.has_value());
  EXPECT_EQ(hit.icon->url, "https://cdn.example/icon.jpg");
  EXPECT_EQ(hit.icon->blob_id, "blob-1");
  EXPECT_EQ(hit.icon->kind, "image/jpeg");

  Contact contact;
  contact.id = "c-icon";
  contact.remote.nickname = hit.nickname;
  contact.remote.ids = hit.ids;
  contact.remote.icon = hit.icon;
  SyncContactMirrors(contact);

  const Contact again = ContactFromJson(ContactToJson(contact));
  ASSERT_TRUE(again.remote.icon.has_value());
  EXPECT_EQ(again.remote.icon->url, hit.icon->url);
  EXPECT_EQ(again.remote.icon->blob_id, hit.icon->blob_id);

  const mesh::DirectoryHit hit_again = DirectoryHitFromJson(DirectoryHitToJson(hit));
  ASSERT_TRUE(hit_again.icon.has_value());
  EXPECT_EQ(hit_again.icon->url, hit.icon->url);
}

} // namespace
