#include "domain/node/NodeMeshServices.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/KemKeyBindingV2.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "mesh/discovery/MeshNodeHitCodec.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

using namespace pbr;

namespace {

MeshConfig AllCapabilities() {
  MeshConfig mesh;
  mesh.capabilities.circuit_relay = true;
  mesh.capabilities.media_relay = true;
  mesh.capabilities.dht = true;
  return mesh;
}

} // namespace

// A client role hosts no node services, whatever its capabilities say.
TEST(NodeMeshServicesTest, ClientHostsNothing) {
  mesh::MeshHostConfig cfg;
  ApplyNodeHosting(cfg, AllCapabilities(), /*node_role=*/false);
  EXPECT_FALSE(cfg.host_circuit_relay);
  EXPECT_FALSE(cfg.host_media_relay);
  EXPECT_FALSE(cfg.host_dht);
  EXPECT_FALSE(cfg.host_directory);
}

// A node hosts relays / DHT per capability and always serves the directory twin (N029 nd4).
TEST(NodeMeshServicesTest, NodeHostsPerCapabilityAndDirectory) {
  MeshConfig mesh = AllCapabilities();
  mesh.capabilities.media_relay = false;
  mesh::MeshHostConfig cfg;
  ApplyNodeHosting(cfg, mesh, /*node_role=*/true);
  EXPECT_TRUE(cfg.host_circuit_relay);
  EXPECT_FALSE(cfg.host_media_relay);
  EXPECT_TRUE(cfg.host_dht);
  EXPECT_TRUE(cfg.host_directory);

  mesh::MeshHostConfig bare;
  ApplyNodeHosting(bare, MeshConfig{}, /*node_role=*/true);
  EXPECT_TRUE(bare.host_directory);
}

// Query peers: directory rows first (N027 L1), then bootstrap seeds, deduped by PeerId.
TEST(NodeMeshServicesTest, QueryPeersDirectoryFirstDeduped) {
  mesh::MeshDirectoryNode a;
  a.peer_id = "QmDirA";
  mesh::MeshDirectoryNode dup;
  dup.peer_id = "QmSeed1";
  mesh::MeshDirectoryNode empty;
  const std::vector<std::string> seeds = {
      "/ip4/1.2.3.4/udp/443/adp/2/p2p/QmSeed1",
      "/ip4/5.6.7.8/udp/443/adp/2/p2p/QmSeed2",
      "/ip4/5.6.7.8/udp/443/adp/2",
  };
  EXPECT_EQ(CollectNodeQueryPeerKeys(seeds, {a, dup, empty}),
            (std::vector<std::string>{"QmDirA", "QmSeed1", "QmSeed2"}));
  EXPECT_EQ(CollectNodeQueryPeerKeys(seeds), (std::vector<std::string>{"QmSeed1", "QmSeed2"}));
}

// The node's row carries its account's keys as a key card does (mesh v10, docs/DIRECTORY.md): typed keys and a KEM
// binding v2 the account signed, so the mesh keeps all three and a reader can check them.
TEST(NodeMeshServicesTest, RowKeysAreTheAccountsTypedWithABindingV2) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "pp_node_row_keys_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  EnsureSodiumInit();
  {
    IdentityStore identity(dir.string(), "node");
    ASSERT_TRUE(identity.SetDek(ByteVector(kDataEncryptionKeySize, 7)));
    ASSERT_TRUE(identity.LoadOrCreate());
    const LocalIdentity local = identity.Get().value();

    auto keys = LocalDirectoryRowKeys(identity);
    ASSERT_TRUE(keys) << keys.error().message;
    EXPECT_EQ(mesh::SigningPublicKeyShape(keys->signing_public_key_b64), mesh::DirectoryKeyShape::Ok);
    EXPECT_EQ(mesh::KemPublicKeyShape(keys->kem_public_key_b64), mesh::DirectoryKeyShape::Ok);
    EXPECT_EQ(mesh::KemBindingShape(keys->kem_binding_b64), mesh::DirectoryKeyShape::Ok);

    // The account's keys (not the device's): the signing key hashes to the row's Account ID.
    const TypedRead signing = ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, keys->signing_public_key_b64, "key");
    const TypedRead kem = ReadTypedBlobB64(pp::BlobKind::kKemPublicKey, keys->kem_public_key_b64, "kem");
    ASSERT_TRUE(signing.ok() && kem.ok());
    EXPECT_EQ(AccountIdFromMlDsaPublicKey(signing.raw).value(), local.account_id);
    EXPECT_EQ(Base64Encode(signing.raw), local.account_signing_public_key_b64);
    EXPECT_EQ(Base64Encode(kem.raw), local.kem_public_key_b64);

    // u8 2 ‖ ext area (empty) ‖ LenBytes(typed signature), the signature the account's over the v2 signed bytes.
    const ByteVector blob = Base64Decode(keys->kem_binding_b64).value();
    SignedBlobReader in(blob);
    EXPECT_EQ(in.U8(), kKemBindingV2Version);
    EXPECT_TRUE(in.Ext().empty());
    const ByteVector signature_blob = in.Bytes();
    ASSERT_TRUE(in.Done());
    const TypedRead signature = ReadTypedBlob(pp::BlobKind::kSignature, signature_blob, "binding signature");
    ASSERT_TRUE(signature.ok());
    const ByteVector signed_bytes = KemBindingV2SignBytes(Base64Decode(keys->signing_public_key_b64).value(),
                                                          Base64Decode(keys->kem_public_key_b64).value());
    EXPECT_TRUE(VerifyTyped(signing, signed_bytes, signature));

    // Through the mesh's row codec: kept, nothing flagged.
    mesh::MeshNodeHit hit;
    hit.account_id = local.account_id;
    hit.signing_public_key_b64 = keys->signing_public_key_b64;
    hit.kem_public_key_b64 = keys->kem_public_key_b64;
    hit.kem_binding_b64 = keys->kem_binding_b64;
    auto back = mesh::MeshNodeHitFromObject(mesh::MeshNodeHitToObject(hit));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->kem_binding_b64, keys->kem_binding_b64);
    EXPECT_EQ(back->signing_public_key_b64, keys->signing_public_key_b64);
    EXPECT_FALSE(back->keys_update_needed);
  }
  std::filesystem::remove_all(dir);
}
